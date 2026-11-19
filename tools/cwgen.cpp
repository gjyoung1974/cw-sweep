// cwgen: write a synthetic CW band recording (WAV) for testing cwsweep.
#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "synth.hpp"

namespace {
void usage() {
  std::fprintf(stderr,
               "usage: cwgen [options] OUT.wav\n"
               "  --fs HZ                sample rate (8000; use 48000+ with --iq for wideband)\n"
               "  --dur SEC              duration (default: long enough for all stations)\n"
               "  --iq                   write 2-channel I/Q instead of mono audio\n"
               "  --station F:WPM:SNR:TEXT   add a station (repeatable); F in Hz offset\n"
               "  --random N             add N random stations\n"
               "  --fmin HZ --fmax HZ    frequency range for random stations (300..3300)\n"
               "  --snr MIN:MAX          SNR range for random stations, dB in 2500 Hz (-10:15)\n"
               "  --wpm MIN:MAX          speed range for random stations (16:36)\n"
               "  --jitter X             fist jitter for all stations (0)\n"
               "  --qsb DEPTH            Rayleigh fading depth for all stations (0)\n"
               "  --qrn RATE             impulse noise per second (0)\n"
               "  --no-noise             no AWGN\n"
               "  --seed N\n");
}
}  // namespace

int main(int argc, char** argv) {
  cw::SynthParams sp;
  sp.duration = 0;
  std::vector<cw::Station> stations;
  int nRandom = 0;
  double fmin = 300, fmax = 3300, snrMin = -10, snrMax = 15, wpmMin = 16, wpmMax = 36;
  double jitter = 0, qsb = 0;
  std::string out;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (a == "--fs") sp.sampleRate = std::stod(next());
    else if (a == "--dur") sp.duration = std::stod(next());
    else if (a == "--iq") sp.iq = true;
    else if (a == "--station") {
      std::string v = next();
      cw::Station s;
      char text[512] = {};
      if (std::sscanf(v.c_str(), "%lf:%lf:%lf:%511[^\n]", &s.freq, &s.wpm, &s.snrDb, text) != 4) {
        std::fprintf(stderr, "bad --station '%s'\n", v.c_str());
        return 2;
      }
      s.text = text;
      stations.push_back(s);
    } else if (a == "--random") nRandom = std::stoi(next());
    else if (a == "--fmin") fmin = std::stod(next());
    else if (a == "--fmax") fmax = std::stod(next());
    else if (a == "--snr") std::sscanf(next().c_str(), "%lf:%lf", &snrMin, &snrMax);
    else if (a == "--wpm") std::sscanf(next().c_str(), "%lf:%lf", &wpmMin, &wpmMax);
    else if (a == "--jitter") jitter = std::stod(next());
    else if (a == "--qsb") qsb = std::stod(next());
    else if (a == "--qrn") sp.qrnRate = std::stod(next());
    else if (a == "--no-noise") sp.noise = false;
    else if (a == "--seed") sp.seed = std::stoull(next());
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { usage(); return 2; }
    else out = a;
  }
  if (out.empty() || (stations.empty() && nRandom == 0)) { usage(); return 2; }

  std::mt19937_64 rng(sp.seed);
  std::uniform_real_distribution<double> U(0, 1);
  for (int i = 0; i < nRandom; ++i) {
    cw::Station s;
    for (int tries = 0; tries < 100; ++tries) {
      s.freq = fmin + (fmax - fmin) * U(rng);
      bool ok = std::all_of(stations.begin(), stations.end(),
                            [&](const cw::Station& o) { return std::fabs(o.freq - s.freq) > 120; });
      if (ok) break;
    }
    s.wpm = wpmMin + (wpmMax - wpmMin) * U(rng);
    s.snrDb = snrMin + (snrMax - snrMin) * U(rng);
    s.text = cw::randomMessage(rng, cw::randomCallsign(rng));
    s.start = 0.5 + 3 * U(rng);
    stations.push_back(s);
  }
  double needed = 0;
  for (auto& s : stations) {
    s.fist.jitter = jitter;
    if (qsb > 0) { s.qsbRate = 0.3; s.qsbDepth = qsb; }
    needed = std::max(needed, s.start + cw::messageDuration(s.text, s.wpm) + 2.0);
  }
  if (sp.duration <= 0) sp.duration = needed;

  auto x = cw::synthesize(stations, sp);
  float peak = 1e-9f;
  for (auto& v : x) peak = std::max({peak, std::fabs(v.real()), std::fabs(v.imag())});
  int ch = sp.iq ? 2 : 1;
  std::vector<float> inter(x.size() * static_cast<size_t>(ch));
  for (size_t i = 0; i < x.size(); ++i) {
    inter[i * ch] = 0.9f * x[i].real() / peak;
    if (sp.iq) inter[i * ch + 1] = 0.9f * x[i].imag() / peak;
  }
  SF_INFO info{};
  info.samplerate = static_cast<int>(sp.sampleRate);
  info.channels = ch;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  SNDFILE* f = sf_open(out.c_str(), SFM_WRITE, &info);
  if (!f) { std::fprintf(stderr, "cannot write %s: %s\n", out.c_str(), sf_strerror(nullptr)); return 1; }
  sf_writef_float(f, inter.data(), static_cast<sf_count_t>(x.size()));
  sf_close(f);

  std::sort(stations.begin(), stations.end(), [](auto& p, auto& q) { return p.freq < q.freq; });
  std::fprintf(stderr, "wrote %s: %.1f s, %g Hz, %s\n", out.c_str(), sp.duration, sp.sampleRate,
               sp.iq ? "I/Q" : "audio");
  for (auto& s : stations)
    std::fprintf(stderr, "  %7.1f Hz %4.1f WPM %+5.1f dB  %s\n", s.freq, s.wpm, s.snrDb, s.text.c_str());
  return 0;
}
