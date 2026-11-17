// cwsweep: multi-channel CW decoder and DX-cluster spotter.
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <thread>

#include "callsign.hpp"
#include "engine.hpp"
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

Input
  --iq                  input is I/Q (stereo file, 2-channel raw, sound card I/Q)
  --swap-iq             swap I and Q
  --rate HZ             sample rate (raw, pulse: 48000, soapy: 192000)
  --format F            raw sample format: s16 (default), f32, u8 (rtl_sdr)
  --freq F              dial (audio) or centre (I/Q) frequency, e.g. 14025k, 7.03M
  --lsb                 audio input from a radio in LSB / CW-R mode
  --gain DB             SoapySDR gain (default automatic)
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
  --call CALL           spotter callsign for spots (N0CALL)
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
  std::string record, call = "N0CALL", scpPath;

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
      else if (a == "--gain") so.gain = std::stod(next());
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
  cfg.centerHz = so.freq;
  for (auto& ch : call) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  cfg.spotter = call + "-#";
  if (cfg.iq) {
    if (!fminSet) cfg.fmin = -0.48 * cfg.sampleRate;
    if (!fmaxSet) cfg.fmax = 0.48 * cfg.sampleRate;
  } else if (!fmaxSet) {
    cfg.fmax = std::min(cfg.fmax, 0.5 * cfg.sampleRate - 100);
  }
  const bool haveRf = so.freq > 0;

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
  double consumed = 0;
  while (!gStop) {
    if (!src->read(buf, block)) break;
    if (buf.empty()) continue;
    if (rec) rec->write(buf);
    eng.process(buf.data(), buf.size());
    consumed += static_cast<double>(buf.size());
    auto now = std::chrono::steady_clock::now();
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
  if (telnet) telnet->stop();
  return 0;
}
