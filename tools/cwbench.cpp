// cwbench: measure decoder accuracy vs SNR on synthetic band conditions.
//
// For every SNR point, runs N trials of a random station (callsign exchange
// or random 5-character groups) through the full engine and reports the
// character error rate, the fraction of perfect copies, callsign spot rate and
// false spots.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "callsign.hpp"
#include "engine.hpp"
#include "morse.hpp"
#include "synth.hpp"

namespace {

struct Options {
  std::vector<double> snrs;
  int trials = 20;
  double wpmMin = 18, wpmMax = 32;
  double jitter = 0.0;
  double weight = 0.0;
  double qsb = 0.0;          // depth 0..1 (rate 0.3 Hz)
  double qrn = 0.0;          // impulses / s
  int interferers = 0;
  bool randomText = false;
  bool nb = true;
  double nbThreshold = 8.0;
  double threshold = 4.0;
  double lm = 0.4;
  double llrScale = 0.6, switchRate = 0.6;
  bool hard = false;
  int beam = 64;
  double startPenalty = 8.0;
  double match = 0.7;
  bool noFade = false;
  double fadeFloor = 10.0;
  double fs = 8000;
  bool verbose = false;
  int only = -1;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
};

size_t editDistance(const std::string& a, const std::string& b) {
  std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); ++j)
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

std::string collapse(std::string s) {
  std::string out;
  for (char c : s) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out.push_back(c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

struct TrialResult {
  double cer = 1.0;
  bool perfect = false;
  bool spotted = false;
  int falseSpots = 0;
  std::string truth, decoded;
};

TrialResult runTrial(const Options& o, double snr, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> U(0, 1);
  auto wpm = [&] { return o.wpmMin + (o.wpmMax - o.wpmMin) * U(rng); };

  std::vector<cw::Station> st;
  std::vector<std::string> calls;
  cw::Station t;
  t.freq = 600 + 1800 * U(rng);
  t.wpm = wpm();
  std::string call = cw::randomCallsign(rng);
  calls.push_back(call);
  t.text = o.randomText ? cw::randomText(rng, 8) : cw::randomMessage(rng, call);
  t.snrDb = snr;
  t.start = 1.0 + U(rng);
  t.fist.jitter = o.jitter;
  t.fist.weight = o.weight;
  if (o.qsb > 0) { t.qsbRate = 0.3; t.qsbDepth = o.qsb; }
  st.push_back(t);
  double dur = t.start + cw::messageDuration(t.text, t.wpm) + 2.5;

  for (int i = 0; i < o.interferers; ++i) {
    cw::Station s;
    do { s.freq = 300 + 3000 * U(rng); } while (std::fabs(s.freq - t.freq) < 150);
    s.wpm = wpm();
    std::string c = cw::randomCallsign(rng);
    calls.push_back(c);
    s.text = cw::randomMessage(rng, c);
    s.snrDb = snr + 20 * U(rng) - 5;
    s.start = 0.5 + 2 * U(rng);
    s.fist.jitter = o.jitter;
    st.push_back(s);
  }

  // Every callsign actually sent (including QSO partners) is a legitimate spot.
  for (const auto& s : st) {
    std::string w, txt = cw::normalizeText(s.text) + " ";
    for (char c : txt) {
      if (c != ' ') { w.push_back(c); continue; }
      if (cw::isCallsign(w)) calls.push_back(w);
      w.clear();
    }
  }

  cw::SynthParams sp;
  sp.sampleRate = o.fs;
  sp.duration = dur;
  sp.qrnRate = o.qrn;
  sp.seed = seed * 7919 + 1;
  auto x = cw::synthesize(st, sp);

  cw::EngineConfig cfg;
  cfg.sampleRate = o.fs;
  cfg.thresholdDb = o.threshold;
  cfg.lmWeight = o.lm;
  cfg.llrScale = o.llrScale;
  cfg.switchRate = o.switchRate;
  cfg.softDecode = !o.hard;
  cfg.beamWidth = o.beam;
  cfg.startPenalty = o.startPenalty;
  cfg.matchFactor = o.match;
  cfg.fadingModel = !o.noFade;
  cfg.fadeFloorDb = o.fadeFloor;
  cfg.noiseBlanker = o.nb;
  cfg.nbThreshold = o.nbThreshold;
  cw::Engine eng(cfg);

  struct ChText { int id; double off; std::string text; };
  std::vector<ChText> texts;
  std::vector<std::pair<std::string, double>> spots;
  eng.onText = [&](const cw::ChannelInfo& ci, const std::string& s) {
    for (auto& c : texts) if (c.id == ci.id) { c.text += s; c.off = ci.offsetHz; return; }
    texts.push_back({ci.id, ci.offsetHz, s});
  };
  eng.onSpot = [&](const cw::Spot& s) { spots.emplace_back(s.call, s.rfHz); };
  eng.process(x.data(), x.size());
  eng.finish();

  TrialResult r;
  r.truth = cw::normalizeText(t.text);
  // Best-matching channel near the target frequency.
  for (auto& c : texts) {
    if (std::fabs(c.off - t.freq) > 40) continue;
    std::string d = collapse(c.text);
    double cer = std::min(1.0, static_cast<double>(editDistance(r.truth, d)) / r.truth.size());
    if (cer < r.cer) { r.cer = cer; r.decoded = d; }
  }
  r.perfect = r.cer == 0.0;
  if (o.verbose) {
    char fb[64];
    std::snprintf(fb, sizeof fb, "[%.1f Hz %.1f wpm] ", t.freq, t.wpm);
    r.truth = fb + r.truth;
    for (auto& c : texts) {
      std::snprintf(fb, sizeof fb, "\n         ch%d %.1f Hz: ", c.id, c.off);
      r.decoded += fb + collapse(c.text).substr(0, 80);
    }
  }
  for (auto& [c, f] : spots) {
    if (c == call && std::fabs(f - t.freq) < 60) r.spotted = true;
    if (std::find(calls.begin(), calls.end(), c) == calls.end()) {
      ++r.falseSpots;
      if (o.verbose) r.decoded += "\n         FALSE SPOT " + c;
    }
  }
  return r;
}

void usage() {
  std::fprintf(stderr,
               "usage: cwbench [options]\n"
               "  --snr a,b,c | --snr from:to:step   SNR points in dB (2500 Hz), default -20:0:2\n"
               "  --trials N            trials per point (20)\n"
               "  --wpm MIN:MAX         speed range (18:32)\n"
               "  --jitter X            hand-sent timing jitter, relative std-dev (0)\n"
               "  --weight X            keying weight in dits (0)\n"
               "  --qsb DEPTH           Rayleigh fading depth 0..1 (0)\n"
               "  --qrn RATE            impulse noise per second (0)\n"
               "  --no-nb               disable the impulse noise blanker\n"
               "  --interferers N       extra stations in the passband (0)\n"
               "  --random-text         random 2-6 char groups instead of QSO text\n"
               "  --threshold DB        detection threshold (4)\n"
               "  --lm W                language model weight (0.4)\n"
               "  --hard                hard-decision decoder instead of soft\n"
               "  --beam N              soft decoder beam width (64)\n"
               "  --threads N  -v  --only TRIAL\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (double s = -20; s <= 0.01; s += 2) o.snrs.push_back(s);
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (a == "--snr") {
      std::string v = next();
      o.snrs.clear();
      if (std::count(v.begin(), v.end(), ':') == 2) {
        double f, t, s;
        std::sscanf(v.c_str(), "%lf:%lf:%lf", &f, &t, &s);
        for (double x = f; x <= t + 1e-9; x += s) o.snrs.push_back(x);
      } else {
        size_t p = 0;
        while (p < v.size()) {
          size_t q = v.find(',', p);
          o.snrs.push_back(std::stod(v.substr(p, q - p)));
          if (q == std::string::npos) break;
          p = q + 1;
        }
      }
    } else if (a == "--trials") o.trials = std::stoi(next());
    else if (a == "--wpm") std::sscanf(next().c_str(), "%lf:%lf", &o.wpmMin, &o.wpmMax);
    else if (a == "--jitter") o.jitter = std::stod(next());
    else if (a == "--weight") o.weight = std::stod(next());
    else if (a == "--qsb") o.qsb = std::stod(next());
    else if (a == "--qrn") o.qrn = std::stod(next());
    else if (a == "--no-nb") o.nb = false;
    else if (a == "--nb-threshold") o.nbThreshold = std::stod(next());
    else if (a == "--interferers") o.interferers = std::stoi(next());
    else if (a == "--random-text") o.randomText = true;
    else if (a == "--threshold") o.threshold = std::stod(next());
    else if (a == "--lm") o.lm = std::stod(next());
    else if (a == "--llr-scale") o.llrScale = std::stod(next());
    else if (a == "--hard") o.hard = true;
    else if (a == "--beam") o.beam = std::stoi(next());
    else if (a == "--start-penalty") o.startPenalty = std::stod(next());
    else if (a == "--match") o.match = std::stod(next());
    else if (a == "--no-fade") o.noFade = true;
    else if (a == "--fade-floor") o.fadeFloor = std::stod(next());
    else if (a == "--switch") o.switchRate = std::stod(next());
    else if (a == "--threads") o.threads = static_cast<unsigned>(std::stoi(next()));
    else if (a == "-v") o.verbose = true;
    else if (a == "--only") o.only = std::stoi(next());
    else { usage(); return a == "-h" || a == "--help" ? 0 : 2; }
  }

  std::printf("%7s %8s %9s %9s %8s\n", "SNR dB", "CER %", "perfect%", "spotted%", "false");
  for (double snr : o.snrs) {
    std::vector<TrialResult> res(static_cast<size_t>(o.trials));
    std::atomic<int> nextTrial{0};
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < o.threads; ++k) {
      pool.emplace_back([&] {
        for (int i; (i = nextTrial++) < o.trials;) {
          if (o.only >= 0 && i != o.only) { res[static_cast<size_t>(i)].perfect = true; res[static_cast<size_t>(i)].cer = 0; continue; }
          res[static_cast<size_t>(i)] = runTrial(o, snr, 1000 + static_cast<uint64_t>(i));
        }
      });
    }
    for (auto& th : pool) th.join();
    double cer = 0;
    int perfect = 0, spotted = 0, falses = 0;
    for (auto& r : res) {
      cer += r.cer;
      perfect += r.perfect;
      spotted += r.spotted;
      falses += r.falseSpots;
      if (o.verbose && (!r.perfect || r.falseSpots))
        std::printf("  #%zu truth: %s\n       got:   %s\n", static_cast<size_t>(&r - res.data()),
                    r.truth.c_str(), r.decoded.c_str());
    }
    std::printf("%7.1f %8.1f %9.0f %9.0f %8d\n", snr, 100 * cer / o.trials,
                100.0 * perfect / o.trials, 100.0 * spotted / o.trials, falses);
    std::fflush(stdout);
  }
  return 0;
}
