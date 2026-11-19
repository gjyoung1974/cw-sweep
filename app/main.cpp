// cwsweep: multi-channel CW decoder and DX-cluster spotter.
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <tuple>
#include <memory>
#include <string>
#include <thread>

#include "callsign.hpp"
#include "engine.hpp"
#include "hpsdr.hpp"
#include "rigctl.hpp"
#include "sources.hpp"
#include "telnet.hpp"

namespace {

volatile std::sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }

void usage(FILE* f) {
  std::fprintf(f, R"(usage: cwsweep [options] SOURCE

Decodes every CW signal in the receiver passband, prints the decoded text and
spots callsigns (optionally to a DX-cluster telnet server).

SOURCE
  FILE.wav              any sound file libsndfile reads (mono audio, or I/Q with --iq)
  -  | raw:PATH         raw samples from stdin / a file or FIFO (needs --rate, --format)
  pulse[:DEVICE]        PulseAudio / PipeWire capture (sound card, virtual cable)
  soapy[:ARGS]          SoapySDR device, e.g. soapy:driver=rtlsdr,direct_samp=2 (needs --freq)
  hpsdr[:ADDRESS]       Hermes Lite 2, Radioberry or other openHPSDR (protocol 1) radio on
                        the network; found by broadcast unless an address is given.
                        Receive only. Needs --band, --segment or --freq.

Input
  --iq                  input is I/Q (stereo file, 2-channel raw, sound card I/Q)
  --swap-iq             swap I and Q
  --rate HZ             sample rate (raw, pulse: 48000, soapy: 192000,
                        hpsdr: 48k/96k/192k/384k, default the smallest that fits)
  --format F            raw sample format: s16 (default), f32, u8 (rtl_sdr)
  --freq F              dial (audio) or centre (I/Q) frequency, e.g. 14025k, 7.03M
  --lsb                 audio input from a radio in LSB / CW-R mode
  --band NAME           decode the CW part of a band: 160m 80m 40m 30m 20m 17m 15m 12m
                        10m 6m. For SDRs this also sets the centre frequency.
  --segment LO-HI       decode this frequency range instead, e.g. 7000k-7040k
  --gain DB             SoapySDR gain (default automatic); hpsdr LNA gain, -12..48 (20)
  --rigctl HOST[:PORT]  follow the radio's frequency and mode through Hamlib's rigctld
                        (port 4532), and pause decoding while it transmits
  --pitch HZ            audio pitch of a signal on the dial frequency, for a radio in
                        CW mode (600 when rigctld reports CW mode, otherwise 0)
  --discover            list the openHPSDR radios on the network and exit
  --record FILE.wav     record the raw input for later replay
  --realtime            play files at real-time speed

Decoder
  --fmin HZ --fmax HZ   offset range to decode (audio: 200..3500; I/Q: whole band)
  --bin-hz HZ           frequency resolution of the channelizer (50)
  --threshold DB        detection threshold, signal/noise in one bin (4)
  --nb-threshold X      impulse noise blanker threshold, x background (8)
  --no-nb               disable the impulse noise blanker
  --lm WEIGHT           language-model weight, 0 disables (0.4)

Output
  --call CALL           your callsign: names the spotter, and is never spotted itself
  --telnet PORT         run a DX-cluster telnet server (e.g. 7300)
  --tui                 live channel table instead of scrolling text
  --no-text             only print spots
  --no-spots            only print decoded text
  --min-snr DB          hide text from channels weaker than this (-100)
  --respot SEC          minimum time before re-spotting a call (600)
  --scp FILE            known-calls list (MASTER.SCP): allows spotting a call
                        after a single copy following CQ/DE/TEST/TU; otherwise
                        two identical copies are required
  --all-text            also show text that looks like decoded noise
  --no-color
  -h, --help
)");
}

double parseFreq(const std::string& s) {
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end && *end) {
    switch (*end) {
      case 'k': case 'K': v *= 1e3; break;
      case 'm': case 'M': v *= 1e6; break;
      case 'g': case 'G': v *= 1e9; break;
      default: break;
    }
  }
  return v;
}

// Text decoded from noise has a recognisable signature: element durations
// that don't fit Morse timing (real CW, even a sloppy fist, has a relative
// timing spread below ~0.32), mostly one- and two-element "words" (E, T, I,
// ...), and undecodable characters.
bool looksLikeNoise(const std::string& line, const cw::ChannelInfo& ci) {
  if (ci.spread > 0.36 || ci.wpm > 60) return true;
  int words = 0, junkWords = 0, stars = 0, chars = 0;
  std::string w;
  auto endWord = [&] {
    if (w.empty()) return;
    ++words;
    bool shortJunk = w.size() <= 2 && w.find_first_not_of("ETIANM") == std::string::npos;
    if (shortJunk) ++junkWords;
    w.clear();
  };
  for (char c : line) {
    if (c == ' ') { endWord(); continue; }
    ++chars;
    if (c == '*') ++stars;
    w.push_back(c);
  }
  endWord();
  if (words == 0) return true;
  return stars * 6 > chars || junkWords * 2 >= words;
}

struct Band { const char* name; double lo, hi; };
// CW parts of the amateur bands, stopping short of the FT8/FT4 frequencies.
const Band kBands[] = {
    {"160m", 1800e3, 1836e3},   {"80m", 3500e3, 3570e3},   {"40m", 7000e3, 7045e3},
    {"30m", 10100e3, 10130e3},  {"20m", 14000e3, 14070e3}, {"17m", 18068e3, 18095e3},
    {"15m", 21000e3, 21070e3},  {"12m", 24890e3, 24913e3}, {"10m", 28000e3, 28070e3},
    {"6m", 50000e3, 50100e3},
};

std::string fmtFreq(const cw::ChannelInfo& ci, bool haveRf) {
  char b[32];
  if (haveRf) std::snprintf(b, sizeof b, "%10.2f", ci.rfHz / 1000.0);
  else std::snprintf(b, sizeof b, "%7.0f Hz", ci.offsetHz);
  return b;
}

struct Tui {
  std::deque<std::string> spots;
  void addSpot(const std::string& s) {
    spots.push_back(s);
    while (spots.size() > 10) spots.pop_front();
  }
  bool allText = false;
  void draw(const cw::Engine& eng, const std::string& src, bool haveRf, int clients) {
    winsize ws{};
    int rows = 40, cols = 120;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) { rows = ws.ws_row; cols = ws.ws_col; }
    auto chans = eng.channels();
    std::string out = "\x1b[H\x1b[2J";
    char hdr[256];
    std::snprintf(hdr, sizeof hdr, "\x1b[1mcwsweep\x1b[0m  %s  t=%.0fs  channels=%zu  telnet clients=%d\r\n",
                  src.c_str(), eng.streamTime(), chans.size(), clients);
    out += hdr;
    out += "\x1b[7m      Freq   WPM   SNR  Text" + std::string(static_cast<size_t>(std::max(0, cols - 28)), ' ') + "\x1b[0m\r\n";
    int avail = rows - 4 - static_cast<int>(spots.size()) - 1;
    int textW = std::max(10, cols - 30);
    int shown = 0;
    for (const auto& ci : chans) {
      if (shown >= avail) break;
      if (!ci.locked || ci.recent.empty() || (!allText && looksLikeNoise(ci.recent, ci))) continue;
      std::string t = ci.recent;
      if (static_cast<int>(t.size()) > textW) t = t.substr(t.size() - static_cast<size_t>(textW));
      char line[512];
      std::snprintf(line, sizeof line, "%s %4.0f %4.0fdB  %s\r\n", fmtFreq(ci, haveRf).c_str(), ci.wpm,
                    ci.snrDb, t.c_str());
      out += line;
      ++shown;
    }
    out += "\x1b[1m-- spots --\x1b[0m\r\n";
    for (auto& s : spots) out += "\x1b[32m" + s + "\x1b[0m\r\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
  }
};

}  // namespace

int main(int argc, char** argv) {
  cwapp::SourceOptions so;
  cw::EngineConfig cfg;
  bool fminSet = false, fmaxSet = false, tui = false, showText = true, showSpots = true;
  bool realtime = false, color = isatty(STDOUT_FILENO), allText = false;
  int telnetPort = 0;
  double minSnr = -100;
  std::string record, call = "N0CALL", scpPath, rigTarget;
  double segLo = 0, segHi = 0, pitch = 0;
  bool pitchSet = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
      return argv[++i];
    };
    try {
      if (a == "-h" || a == "--help") { usage(stdout); return 0; }
      else if (a == "--iq") so.iq = true;
      else if (a == "--swap-iq") so.swapIq = true;
      else if (a == "--rate") so.rate = parseFreq(next());
      else if (a == "--format") so.format = next();
      else if (a == "--freq") so.freq = parseFreq(next());
      else if (a == "--lsb") cfg.lsb = true;
      else if (a == "--gain") { so.gain = std::stod(next()); so.gainSet = true; }
      else if (a == "--band") {
        std::string b = next();
        for (auto& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!b.empty() && b.back() != 'm') b += 'm';
        for (const Band& k : kBands) if (b == k.name) { segLo = k.lo; segHi = k.hi; }
        if (segHi <= 0) { std::fprintf(stderr, "unknown band %s\n", b.c_str()); return 2; }
      }
      else if (a == "--segment") {
        std::string v = next();
        size_t dash = v.find('-', 1);
        if (dash == std::string::npos) throw std::invalid_argument(v);
        segLo = parseFreq(v.substr(0, dash));
        segHi = parseFreq(v.substr(dash + 1));
        if (segLo <= 0 || segHi <= segLo) throw std::invalid_argument(v);
      }
      else if (a == "--rigctl") rigTarget = next();
      else if (a == "--pitch") { pitch = std::stod(next()); pitchSet = true; }
      else if (a == "--discover") return cwapp::listHpsdr() ? 0 : 1;
      else if (a == "--record") record = next();
      else if (a == "--realtime") realtime = true;
      else if (a == "--fmin") { cfg.fmin = std::stod(next()); fminSet = true; }
      else if (a == "--fmax") { cfg.fmax = std::stod(next()); fmaxSet = true; }
      else if (a == "--bin-hz") cfg.binHz = std::stod(next());
      else if (a == "--threshold") cfg.thresholdDb = std::stod(next());
      else if (a == "--nb-threshold") cfg.nbThreshold = std::stod(next());
      else if (a == "--no-nb") cfg.noiseBlanker = false;
      else if (a == "--lm") cfg.lmWeight = std::stod(next());
      else if (a == "--call") call = next();
      else if (a == "--telnet") telnetPort = std::stoi(next());
      else if (a == "--tui") tui = true;
      else if (a == "--no-text") showText = false;
      else if (a == "--no-spots") showSpots = false;
      else if (a == "--min-snr") minSnr = std::stod(next());
      else if (a == "--respot") cfg.respotSec = std::stod(next());
      else if (a == "--scp") scpPath = next();
      else if (a == "--no-color") color = false;
      else if (a == "--all-text") allText = true;
      else if (a.size() > 1 && a[0] == '-' && a != "-") { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
      else so.spec = a;
    } catch (const std::exception&) {
      std::fprintf(stderr, "bad value for %s\n", a.c_str());
      return 2;
    }
  }
  if (so.spec.empty()) { usage(stderr); return 2; }

  // A band or segment places an SDR by itself: centre it, and for an
  // openHPSDR radio take the lowest sample rate that covers it.
  const bool hpsdr = so.spec == "hpsdr" || so.spec.rfind("hpsdr:", 0) == 0;
  const bool sdr = hpsdr || so.spec == "soapy" || so.spec.rfind("soapy:", 0) == 0;
  if (segHi > 0 && sdr) {
    if (so.freq <= 0) so.freq = std::round(0.5 * (segLo + segHi) / 1000.0) * 1000.0;
    if (so.rate <= 0 && hpsdr) {
      double span = 2.0 * std::max(std::fabs(segLo - so.freq), std::fabs(segHi - so.freq));
      so.rate = 384000;
      for (double r : {48000.0, 96000.0, 192000.0})
        if (0.9 * r >= span) { so.rate = r; break; }
    }
  }

  std::string err;
  cw::KnownCalls known;
  if (!scpPath.empty()) {
    if (!known.load(scpPath, err)) { std::fprintf(stderr, "cwsweep: %s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "cwsweep: loaded %zu known calls from %s\n", known.size(), scpPath.c_str());
    cfg.knownCalls = &known;
  }
  auto src = cwapp::openSource(so, err);
  if (!src) { std::fprintf(stderr, "cwsweep: %s\n", err.c_str()); return 1; }

  cfg.sampleRate = src->rate();
  cfg.iq = src->iq();
  for (auto& ch : call) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  cfg.spotter = call + "-#";
  if (call != "N0CALL") cfg.ownCall = call;
  if (cfg.iq) {
    if (!fminSet) cfg.fmin = -0.48 * cfg.sampleRate;
    if (!fmaxSet) cfg.fmax = 0.48 * cfg.sampleRate;
    if (segHi > 0 && so.freq > 0) {
      if (!fminSet) cfg.fmin = std::max(cfg.fmin, segLo - so.freq);
      if (!fmaxSet) cfg.fmax = std::min(cfg.fmax, segHi - so.freq);
      if (cfg.fmin >= cfg.fmax) {
        std::fprintf(stderr, "cwsweep: %.1f-%.1f kHz is outside the %.0f kHz received around %.1f kHz\n",
                     segLo / 1e3, segHi / 1e3, cfg.sampleRate / 1e3, so.freq / 1e3);
        return 1;
      }
    }
  } else {
    if (!fmaxSet) cfg.fmax = std::min(cfg.fmax, 0.5 * cfg.sampleRate - 100);
    if (segHi > 0) std::fprintf(stderr, "cwsweep: --band/--segment only apply to I/Q input; ignored\n");
  }

  // Where the engine's zero offset sits, and which way audio runs, for a
  // radio on `dial` in mode `m`. In CW mode a signal on the dial frequency
  // is heard at the pitch, so the zero offset is one pitch away from it.
  using Mode = cwapp::RigState::Mode;
  const bool userLsb = cfg.lsb;
  auto tuning = [&](double dial, Mode m) -> std::pair<double, bool> {
    if (cfg.iq || dial <= 0) return {dial, false};
    bool lower = (m == Mode::LSB || m == Mode::CWR) != userLsb;
    double p = (m == Mode::CW || m == Mode::CWR) ? (pitchSet ? pitch : 600.0) : 0.0;
    return {dial + (lower ? p : -p), lower};
  };
  std::tie(cfg.centerHz, cfg.lsb) = tuning(so.freq, pitchSet ? Mode::CW : Mode::USB);
  bool haveRf = cfg.centerHz > 0;

  std::unique_ptr<cwapp::RigCtl> rig;
  if (!rigTarget.empty()) {
    rig = std::make_unique<cwapp::RigCtl>(rigTarget);
    rig->start();
  }

  std::unique_ptr<cwapp::Recorder> rec;
  if (!record.empty()) {
    rec = std::make_unique<cwapp::Recorder>(record, cfg.sampleRate, cfg.iq, err);
    if (!rec->ok()) { std::fprintf(stderr, "cwsweep: %s\n", err.c_str()); return 1; }
  }

  std::unique_ptr<cwapp::TelnetServer> telnet;
  if (telnetPort > 0) {
    telnet = std::make_unique<cwapp::TelnetServer>(call);
    if (!telnet->start(telnetPort, err)) { std::fprintf(stderr, "cwsweep: %s\n", err.c_str()); return 1; }
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGPIPE, SIG_IGN);

  cw::Engine eng(cfg);
  Tui view;
  view.allText = allText;
  const char* green = color ? "\x1b[1;32m" : "";
  const char* dim = color ? "\x1b[2m" : "";
  const char* reset = color ? "\x1b[0m" : "";

  std::fprintf(stderr, "cwsweep: %s, %d-point FFT, %.1f Hz resolution, %.1f ms frames, decoding %.0f..%.0f Hz%s\n",
               src->describe().c_str(), eng.fftSize(), cfg.sampleRate / eng.windowLen(),
               1000.0 * eng.hop() / cfg.sampleRate, cfg.fmin, cfg.fmax,
               telnet ? (", telnet port " + std::to_string(telnetPort)).c_str() : "");

  eng.onLine = [&](const cw::ChannelInfo& ci, const std::string& line) {
    if (tui || !showText || ci.snrDb < minSnr || (!allText && looksLikeNoise(line, ci))) return;
    std::printf("%s %s%3.0f WPM %+4.0f dB%s  %s\n", fmtFreq(ci, haveRf).c_str(), dim, ci.wpm, ci.snrDb,
                reset, line.c_str());
    std::fflush(stdout);
  };
  eng.onSpot = [&](const cw::Spot& s) {
    if (rig && !haveRf) return;  // the radio's frequency isn't known yet
    std::string line = s.format();
    if (telnet) telnet->broadcast(line);
    if (tui) view.addSpot(line);
    else if (showSpots) {
      std::printf("%s%s%s\n", green, line.c_str(), reset);
      std::fflush(stdout);
    }
  };

  std::vector<std::complex<float>> buf;
  const size_t block = std::max<size_t>(256, static_cast<size_t>(cfg.sampleRate * 0.05));
  auto t0 = std::chrono::steady_clock::now();
  auto lastDraw = t0;
  auto lastHealth = t0;
  double consumed = 0, rigFreq = 0;
  Mode rigMode = Mode::Other;
  bool transmitting = false, pitchNoted = false;
  while (!gStop) {
    if (!src->read(buf, block)) break;
    if (buf.empty()) continue;
    if (rec) rec->write(buf);

    bool tx = src->transmitting();
    if (rig) {
      cwapp::RigState rs = rig->state();
      tx = tx || rs.ptt;
      if (rs.connected && rs.freqHz > 0 && (rs.freqHz != rigFreq || rs.mode != rigMode)) {
        rigFreq = rs.freqHz;
        rigMode = rs.mode;
        auto [center, lower] = tuning(rigFreq, rigMode);
        eng.retune(center, lower);
        haveRf = true;
        if (!cfg.iq && !pitchSet && !pitchNoted && (rigMode == Mode::CW || rigMode == Mode::CWR)) {
          pitchNoted = true;
          std::fprintf(stderr, "cwsweep: the radio is in CW mode; assuming a 600 Hz pitch (set --pitch)\n");
        }
      }
    }
    // Our own transmission: stop decoding, and afterwards start from a clean
    // history, so that nothing of it is taken for a station.
    if (tx != transmitting) {
      transmitting = tx;
      if (tx) eng.finish();
      else eng.retune(eng.config().centerHz, eng.config().lsb);
    }
    if (!tx) eng.process(buf.data(), buf.size());
    consumed += static_cast<double>(buf.size());
    auto now = std::chrono::steady_clock::now();
    if (now - lastHealth > std::chrono::seconds(10)) {
      lastHealth = now;
      std::string h = src->health();
      if (!h.empty()) std::fprintf(stderr, "cwsweep: %s\n", h.c_str());
    }
    if (realtime && !src->live()) {
      auto due = t0 + std::chrono::duration<double>(consumed / cfg.sampleRate);
      if (due > now) std::this_thread::sleep_until(due);
    }
    if (tui && now - lastDraw > std::chrono::milliseconds(250)) {
      lastDraw = now;
      view.draw(eng, src->describe(), haveRf, telnet ? telnet->clientCount() : 0);
    }
  }
  if (tui) view.draw(eng, src->describe(), haveRf, telnet ? telnet->clientCount() : 0);
  eng.finish();
  if (std::string h = src->health(); !h.empty()) std::fprintf(stderr, "cwsweep: %s\n", h.c_str());
  if (telnet) telnet->stop();
  return 0;
}
