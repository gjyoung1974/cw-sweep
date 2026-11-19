#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "callsign.hpp"
#include "decoder.hpp"
#include "keyer.hpp"
#include "morse.hpp"
#include "softdecoder.hpp"

namespace cw {
namespace {
int debugLevel() {
  static const int d = std::getenv("CWSWEEP_DEBUG") ? std::atoi(std::getenv("CWSWEEP_DEBUG")) : 0;
  return d;
}
}  // namespace

struct Engine::Channel {
  int id = 0;
  int a = 0;              // nearest active FFT bin (noise reference, detection bookkeeping)
  double freqHz = 0;      // tracked carrier offset (AFC)
  double sig = 0;         // key-down signal power: fast tracker from committed marks
  double sigRef = 0;      // key-down signal power: robust estimate from filter history
  Keyer keyer;
  Decoder decoder;
  int64_t keyerBase = 0;  // global frame index of keyer frame 0
  std::string line, recent;
  int64_t lastTextFrame = 0;
  int lowCount = 0;
  CallTracker calls;
  std::vector<KeyEvent> events;
  std::vector<std::pair<int64_t, int>> committed;
  double lastDit = -1, lastBias = 0, lastSigma = 0, lastCharUnits = 0, lastCharSigma = 0;

  // Soft decoder: activated once the hard decoder has locked on to the speed;
  // LLRs seen before that are buffered and replayed.
  SoftDecoder soft;
  bool softActive = false;
  std::vector<float> llrHist;
  int64_t llrHistStart = 0;  // frame index of llrHist[0]

  // Channel filter: coherent sum of `k` phase-aligned STFT frames.
  int k = 1;
  double rotPerFrame = 0;   // cycles of phase advance per frame at freqHz
  std::complex<double> sum, prevSum;
  bool havePrev = false;
  std::vector<float> pow;   // filter output power per frame (ring of kRing)
  std::complex<double> afcAcc;
  int afcCount = 0;

  Channel(const DecoderParams& p, int beamWidth, double startPen, const KnownCalls* known)
      : decoder(CharLM::instance(), p), calls(known),
        soft(CharLM::instance(), p.lmWeight, beamWidth, startPen) {}
};

std::string Spot::format() const {
  // Column layout used by Reverse Beacon Network telnet servers, which
  // loggers parse by position (0-based): frequency ends at column 23, the
  // call starts at column 26, the time is at column 70.
  //   DX de KM3T-#:    14025.0  K1ABC        CW    18 dB  24 WPM  CQ      1830Z
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  char comment[40];
  std::snprintf(comment, sizeof comment, "CW %5.0f dB %3.0f WPM  %s", snrDb, wpm, cq ? "CQ" : "DX");
  char buf[160];
  std::string who = spotter + ":";
  std::snprintf(buf, sizeof buf, "DX de %-9s%9.1f  %-12s %-30s %02d%02dZ", who.c_str(), rfHz / 1000.0,
                call.c_str(), comment, tm.tm_hour, tm.tm_min);
  return buf;
}

Engine::Engine(EngineConfig cfg) : cfg_(std::move(cfg)) {
  os_ = std::max(1, cfg_.oversample);
  win_ = std::max(16, static_cast<int>(std::lround(cfg_.sampleRate / cfg_.binHz)));
  nfft_ = win_ * os_;
  hop_ = std::max(1, static_cast<int>(std::lround(cfg_.sampleRate * cfg_.hopMs / 1000.0)));
  hopSec_ = hop_ / cfg_.sampleRate;
  stft_ = std::make_unique<Stft>(nfft_, win_, hop_);
  if (cfg_.noiseBlanker) nb_ = std::make_unique<NoiseBlanker>(cfg_.sampleRate, cfg_.nbThreshold);

  // Noise gain of a K-frame coherent sum: K + 2 sum (K-m) rho_m.
  noiseGain_.assign(kMaxK + 1, 1.0);
  for (int k = 1; k <= kMaxK; ++k) {
    double g = k;
    for (int m = 1; m < k; ++m) g += 2.0 * (k - m) * stft_->windowCorrelation(m);
    noiseGain_[k] = g;
  }

  double resW = cfg_.sampleRate / win_;
  for (int k = 0; k < nfft_; ++k) {
    double off = offsetOfBin(k);
    if (!cfg_.iq && (k < os_ || k >= nfft_ / 2)) continue;
    if (cfg_.iq && std::fabs(off) < 1.5 * resW) continue;  // DC spur
    if (off < cfg_.fmin || off > cfg_.fmax) continue;
    bins_.push_back(k);
  }
  // Keep I/Q bins in ascending frequency order so neighbours are adjacent.
  std::sort(bins_.begin(), bins_.end(),
            [this](int x, int y) { return offsetOfBin(x) < offsetOfBin(y); });
  size_t nb = bins_.size();
  ring_.assign(nb * kRing, 0.0f);
  cring_.assign(nb * kRing, cfloat{});
  noise_.assign(nb, 1.0);
  noiseRaw_.assign(nb, 1.0);
  noiseLocal_.assign(nb, 1.0);
  bandHi_ = static_cast<int>(nb) - 1;
  peak_.assign(nb, 1.0);
  snr_.assign(nb, 0.0);
  candCount_.assign(nb, 0);
}

Engine::~Engine() = default;

double Engine::offsetOfBin(double k) const {
  double kk = (cfg_.iq && k >= nfft_ / 2.0) ? k - nfft_ : k;
  return kk * cfg_.sampleRate / nfft_;
}

int Engine::binOfOffset(double hz) const {
  auto it = std::lower_bound(bins_.begin(), bins_.end(), hz,
                             [this](int k, double f) { return offsetOfBin(k) < f; });
  int i = static_cast<int>(it - bins_.begin());
  if (i >= static_cast<int>(bins_.size())) return static_cast<int>(bins_.size()) - 1;
  if (i > 0 && hz - offsetOfBin(bins_[i - 1]) < offsetOfBin(bins_[i]) - hz) --i;
  return i;
}

double Engine::streamTime() const { return std::max<int64_t>(frame_, 0) * hopSec_; }

void Engine::processReal(const float* x, size_t n) {
  scratch_.resize(n);
  for (size_t i = 0; i < n; ++i) scratch_[i] = cfloat(x[i], 0.0f);
  process(scratch_.data(), n);
}

void Engine::process(const cfloat* x, size_t n) {
  for (size_t off = 0; off < n; off += kChunk) {
    size_t m = std::min(kChunk, n - off);
    chunk_.assign(x + off, x + off + m);
    if (nb_) nb_->process(chunk_.data(), m);
    stft_->push(chunk_.data(), m, [this](const float* p, const cfloat* x) { frame(p, x); });
  }
}

// ---------------------------------------------------------------------------
// Channel filter
// ---------------------------------------------------------------------------

double Engine::noiseGain(int k) const { return noiseGain_[static_cast<size_t>(std::clamp(k, 1, kMaxK))]; }

void Engine::setFilter(Channel& ch, double hz, int k) {
  ch.freqHz = hz;
  ch.k = std::clamp(k, 1, kMaxK);
  ch.rotPerFrame = hz * hop_ / cfg_.sampleRate;
  ch.a = binOfOffset(hz);
  ch.havePrev = false;
}

std::complex<double> Engine::aligned(const Channel& ch, int64_t f) const {
  // The spectrum's phase is referenced to the frame start (f * hop): remove
  // the carrier's phase advance so successive frames add coherently.
  double cyc = ch.rotPerFrame * static_cast<double>(f);
  cyc -= std::floor(cyc);
  return std::complex<double>(cring(ch.a, f)) * std::polar(1.0, -2.0 * M_PI * cyc);
}

double Engine::filterPower(Channel& ch, int64_t f) {
  std::complex<double> s = 0.0;
  for (int i = 0; i < ch.k && f - i >= 0; ++i) s += aligned(ch, f - i);
  ch.sum = s;
  return std::norm(s) / noiseGain(ch.k);
}

double Engine::llrScaleFor(const Channel& ch) const {
  // Consecutive filter outputs overlap; scale LLRs so that one filter length
  // of evidence counts roughly once.
  double l1 = static_cast<double>(win_) / hop_;
  return cfg_.llrScale * l1 / (l1 + ch.k - 1);
}

double Engine::channelSignal(const Channel& ch, int64_t from, int64_t to) const {
  // Two-cluster split (iterated threshold at the geometric mean of the noise
  // and signal levels) of the channel's filter output history.
  from = std::max({from, to - kRing + 1, int64_t{0}});
  const double n = noise_[ch.a];
  double sig = std::max(ch.sigRef, n);
  for (int it = 0; it < 8; ++it) {
    double thr = std::sqrt(n * (sig + n));
    double sum = 0;
    int cnt = 0;
    for (int64_t f = from; f <= to; ++f) {
      double p = ch.pow[static_cast<size_t>(f % kRing)];
      if (p > thr) { sum += p; ++cnt; }
    }
    if (cnt == 0) break;
    sig = std::max(sum / cnt - n, 0.1 * n);
  }
  return sig;
}

int64_t Engine::recomputeHistory(Channel& ch, int64_t from) {
  from = std::max({from, frame_ - kRing + kMaxK + 1, int64_t{0}});
  for (int64_t f = from; f <= frame_; ++f)
    ch.pow[static_cast<size_t>(f % kRing)] = static_cast<float>(filterPower(ch, f));
  ch.havePrev = false;
  return from;
}

// ---------------------------------------------------------------------------
// Per-frame processing
// ---------------------------------------------------------------------------

void Engine::frame(const float* power, const cfloat* spec) {
  ++frame_;
  {
    const size_t base = slot(frame_);
    float* pr = &ring_[base];
    cfloat* cr = &cring_[base];
    for (size_t a = 0; a < bins_.size(); ++a) {
      pr[a] = power[bins_[a]];
      cr[a] = spec[bins_[a]];
    }
  }

  for (auto& chp : channels_) {
    Channel& ch = *chp;
    double p = filterPower(ch, frame_);
    ch.pow[static_cast<size_t>(frame_ % kRing)] = static_cast<float>(p);
    // AFC: phase advance of the filter output between frames during key-down.
    if (ch.havePrev && p > noise_[ch.a] + 0.5 * ch.sig) {
      ch.afcAcc += ch.sum * std::conj(ch.prevSum);
      ++ch.afcCount;
    }
    ch.prevSum = ch.sum;
    ch.havePrev = true;
    runChannel(ch, p);
  }

  // Idle channels: flush partially filled lines.
  int64_t idleFrames = static_cast<int64_t>(6.0 / hopSec_);
  for (auto& ch : channels_)
    if (!ch->line.empty() && frame_ - ch->lastTextFrame > idleFrames) emitLine(*ch);

  if (frame_ >= kWindow / 4 && frame_ % kDetectEvery == 0) detect();
}

void Engine::runChannel(Channel& ch, double power) {
  double noise = noise_[ch.a];
  const double fadeFloor = std::pow(10.0, cfg_.fadeFloorDb / 10.0);
  double llr = llrScaleFor(ch) * (cfg_.fadingModel ? keyLlrFading(power, noise, ch.sig, fadeFloor)
                                                   : keyLlr(power, noise, ch.sig));
  llr = std::clamp(llr, -20.0, 25.0);
  ch.events.clear();
  ch.committed.clear();
  ch.keyer.push(llr, ch.events, ch.committed);

  // Track signal level (QSB) from frames the Viterbi has committed as key-down.
  for (auto [f, st] : ch.committed) {
    if (!st) continue;
    double p = ch.pow[static_cast<size_t>((ch.keyerBase + f) % kRing)];
    ch.sig += (p - noise - ch.sig) / 40.0;
  }
  // Keep the fast tracker near the robust estimate: this lets it follow QSB
  // without a feedback loop where noise marks drag the level down.
  const double floor = std::pow(10.0, cfg_.thresholdDb / 10.0) * noise;
  ch.sig = std::clamp(ch.sig, std::max(0.5 * ch.sigRef, floor), std::max(2.0 * ch.sigRef, floor));

  for (const KeyEvent& e : ch.events) {
    if (debugLevel() >= 2 || (debugLevel() && e.type != KeyEvent::SpaceProgress))
      std::fprintf(stderr, "[ch%d %.0fHz f=%lld] %s %.1f\n", ch.id, ch.freqHz,
                   static_cast<long long>(frame_),
                   e.type == KeyEvent::Mark ? "MARK " : e.type == KeyEvent::Space ? "space" : "prog ", e.len);
    switch (e.type) {
      case KeyEvent::Mark: ch.decoder.mark(e.len); break;
      case KeyEvent::Space: ch.decoder.space(e.len, true); break;
      case KeyEvent::SpaceProgress: ch.decoder.space(e.len, false); break;
    }
  }
  if (!ch.events.empty()) {
    const Timing& tm = ch.decoder.timing();
    if (ch.decoder.locked() && (tm.dit != ch.lastDit || tm.bias != ch.lastBias || tm.sigma != ch.lastSigma ||
                                tm.charUnits != ch.lastCharUnits || tm.charSigma != ch.lastCharSigma)) {
      if (debugLevel())
        std::fprintf(stderr, "[ch%d %.0fHz f=%lld] timing dit=%.1f bias=%.1f sigma=%.2f gaps=%.1f/%.1f charSigma=%.2f\n",
                     ch.id, ch.freqHz, static_cast<long long>(frame_), tm.dit, tm.bias, tm.sigma, tm.charUnits,
                     tm.wordUnits, tm.charSigma);
      ch.lastCharUnits = tm.charUnits;
      ch.lastCharSigma = tm.charSigma;
      ch.lastSigma = tm.sigma;
      ch.lastDit = tm.dit;
      ch.lastBias = tm.bias;
      ch.keyer.setTiming(tm.dit, ch.decoder.minRun(), cfg_.switchRate);
      ch.soft.setTiming(tm);
      // Follow large speed changes with the matched filter length.
      if (ch.softActive) {
        int target = matchedK(std::min(tm.markMean(0), tm.gapMean(0)));
        if (target != ch.k && std::fabs(std::log((target + 3.0) / (ch.k + 3.0))) > 0.25) {
          double ratio = gainOf(target) / gainOf(ch.k);
          setFilter(ch, ch.freqHz, target);
          ch.sig *= ratio;
          ch.sigRef *= ratio;
        }
      }
    }
    std::string t = ch.decoder.take();
    if (!cfg_.softDecode && !t.empty()) handleText(ch, t);
  }

  if (!cfg_.softDecode) return;
  if (ch.softActive) {
    ch.soft.push(llr);
  } else {
    if (ch.llrHist.empty()) ch.llrHist.reserve(1024);
    ch.llrHist.push_back(static_cast<float>(llr));
    if (ch.llrHist.size() > 6000) {
      ch.llrHist.erase(ch.llrHist.begin(), ch.llrHist.begin() + 1000);
      ch.llrHistStart += 1000;
    }
    if (ch.decoder.spacingReady()) {
      // Speed and spacing are known: switch the channel filter to a boxcar
      // matched to the dit, recompute the buffered history with it and replay
      // it through the soft decoder. Waiting for the spacing (a word or so)
      // means a station with wide letter gaps reads correctly from its first
      // character instead of one letter per word until the fit catches up.
      ch.softActive = true;
      const Timing& tm = ch.decoder.timing();
      setFilter(ch, ch.freqHz, matchedK(std::min(tm.markMean(0), tm.gapMean(0))));
      int64_t firstGood = recomputeHistory(ch, ch.llrHistStart);
      ch.sig = ch.sigRef = channelSignal(ch, firstGood, frame_);
      const double nz = noise_[ch.a];
      const double scale = llrScaleFor(ch);
      for (int64_t f = std::max(firstGood, ch.llrHistStart); f <= frame_; ++f) {
        size_t i = static_cast<size_t>(f - ch.llrHistStart);
        if (i < ch.llrHist.size())
          ch.llrHist[i] = static_cast<float>(std::clamp(
              scale * (cfg_.fadingModel ? keyLlrFading(ch.pow[static_cast<size_t>(f % kRing)], nz, ch.sig,
                                                       std::pow(10.0, cfg_.fadeFloorDb / 10.0))
                                        : keyLlr(ch.pow[static_cast<size_t>(f % kRing)], nz, ch.sig)),
              -20.0, 25.0));
      }
      ch.soft.setTiming(tm);
      for (float v : ch.llrHist) ch.soft.push(v);
      ch.llrHist.clear();
      ch.llrHist.shrink_to_fit();
    }
  }
  std::string t = ch.soft.take();
  if (!t.empty()) handleText(ch, t);
}

void Engine::handleText(Channel& ch, const std::string& text) {
  ch.lastTextFrame = frame_;
  ChannelInfo ci = info(ch);
  if (onText) onText(ci, text);
  for (char c : text) {
    ch.line.push_back(c);
    if (c == ' ' && static_cast<int>(ch.line.size()) >= cfg_.lineWidth) emitLine(ch);
  }
  ch.recent += text;
  if (ch.recent.size() > 200) ch.recent.erase(0, ch.recent.size() - 200);

  std::vector<CallEvent> calls;
  ch.calls.feed(text, streamTime(), calls);
  for (const CallEvent& ce : calls) {
    double rf = ci.rfHz;
    double now = streamTime();
    auto it = lastSpot_.find(ce.call);
    if (it != lastSpot_.end() && now - it->second.first < cfg_.respotSec &&
        std::fabs(rf - it->second.second) < 1000.0)
      continue;
    lastSpot_[ce.call] = {now, rf};
    if (onSpot) {
      Spot s;
      s.spotter = cfg_.spotter;
      s.call = ce.call;
      s.rfHz = rf;
      s.snrDb = ci.snrDb;
      s.wpm = ci.wpm;
      s.cq = ce.cq;
      s.streamTime = now;
      onSpot(s);
    }
  }
}

void Engine::emitLine(Channel& ch) {
  while (!ch.line.empty() && ch.line.back() == ' ') ch.line.pop_back();
  if (!ch.line.empty() && onLine) onLine(info(ch), renderText(ch.line));
  ch.line.clear();
}

ChannelInfo Engine::info(const Channel& ch) const {
  ChannelInfo ci;
  ci.id = ch.id;
  ci.offsetHz = ch.freqHz;
  ci.rfHz = cfg_.centerHz + (cfg_.lsb ? -ch.freqHz : ch.freqHz);
  ci.locked = ch.decoder.locked();
  ci.wpm = ci.locked ? 1.2 / (ch.decoder.timing().dit * hopSec_) : 0.0;
  ci.spread = ch.decoder.timing().sigma;
  // Report SNR as seen in a single FFT bin, regardless of filter length.
  ci.snrDb = 10.0 * std::log10(std::max(ch.sig, 1e-12) / noise_[ch.a] / gainOf(ch.k));
  ci.recent = renderText(ch.recent);
  return ci;
}

std::vector<ChannelInfo> Engine::channels() const {
  std::vector<ChannelInfo> out;
  out.reserve(channels_.size());
  for (const auto& ch : channels_) out.push_back(info(*ch));
  std::sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.rfHz < y.rfHz; });
  return out;
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

void Engine::detect() {
  const int nb = static_cast<int>(bins_.size());
  if (nb < 5) return;
  // Per-bin order statistics: the 20th percentile sits in the key-up gaps of a
  // CW signal (=> noise), the 85th in its key-down (=> signal + noise).
  // Every 4th frame is enough (neighbouring frames overlap by 75%); gather
  // frame by frame so that the history is read sequentially.
  const int64_t avail = std::min<int64_t>(frame_ + 1, kWindow);
  constexpr int kStride = 4;
  const size_t ns = static_cast<size_t>((avail + kStride - 1) / kStride);
  detectBuf_.resize(ns * static_cast<size_t>(nb));
  {
    size_t j = 0;
    for (int64_t f = frame_ - avail + 1; f <= frame_ && j < ns; f += kStride, ++j) {
      const float* row = &ring_[slot(f)];
      for (int a = 0; a < nb; ++a) detectBuf_[static_cast<size_t>(a) * ns + j] = row[a];
    }
  }
  for (int a = 0; a < nb; ++a) {
    float* v = &detectBuf_[static_cast<size_t>(a) * ns];
    size_t i20 = ns / 5, i85 = ns * 85 / 100;
    std::nth_element(v, v + i20, v + ns);
    double p20 = v[i20];
    std::nth_element(v + i20, v + i85, v + ns);
    peak_[a] = v[i85];
    noiseRaw_[a] = p20 / 0.2231;  // exponential distribution: p20 = 0.2231 * mean
  }
  // Local median across frequency: robust to the signals themselves. Only
  // bins inside the receiver's passband take part: beyond the filter edges
  // there is no antenna noise, and counting those bins would put the floor
  // far too low for everything near (or, with a narrow CW filter, inside) it.
  findPassband();
  const int kHalf = 16 * os_;
  std::vector<double> w;
  for (int a = 0; a < nb; ++a) {
    w.clear();
    int lo = std::max(0, a - kHalf), hi = std::min(nb - 1, a + kHalf);
    if (a >= bandLo_ && a <= bandHi_) {
      lo = std::max(lo, bandLo_);
      hi = std::min(hi, bandHi_);
    }
    for (int b = lo; b <= hi; b += 2) w.push_back(noiseRaw_[b]);
    std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
    // A keyed signal's 20th percentile already sits in its gaps, so the local
    // median is robust even in a crowded band; only steady carriers are outliers.
    double est = w[w.size() / 2];
    noise_[a] = noiseReady_ ? 0.7 * noise_[a] + 0.3 * est : est;
    noise_[a] = std::max(noise_[a], 1e-20);
  }
  noiseReady_ = true;
  for (int a = 0; a < nb; ++a) snr_[a] = peak_[a] / noise_[a] - 1.0;

  const double thr = std::pow(10.0, cfg_.thresholdDb / 10.0);
  const double detectSec = kDetectEvery * hopSec_;
  const double resW = cfg_.sampleRate / win_;

  // Channel upkeep: AFC, signal level, retirement.
  for (size_t i = 0; i < channels_.size();) {
    Channel& ch = *channels_[i];
    if (ch.afcCount >= 15) {
      double df = std::arg(ch.afcAcc) / (2.0 * M_PI * hopSec_);
      if (std::fabs(df) > 0.3) {
        double nf = ch.freqHz + std::clamp(0.6 * df, -0.25 * resW, 0.25 * resW);
        setFilter(ch, nf, ch.k);
      }
      ch.afcAcc = 0.0;
      ch.afcCount = 0;
    }
    if (snr_[ch.a] >= thr || ch.softActive) {
      // Only accept a new level when the window clearly contains the station;
      // in pauses the estimator would otherwise lock onto noise peaks.
      double lvl = channelSignal(ch, frame_ - kWindow / 2 + 1, frame_);
      if (lvl >= 2.0 * thr * noise_[ch.a] * gainOf(ch.k)) ch.sigRef = lvl;
    }
    if (snr_[ch.a] < 0.5 * thr) ++ch.lowCount;
    else ch.lowCount = 0;
    if (ch.lowCount * detectSec > cfg_.idleTimeoutSec) {
      closeChannel(ch);
      channels_.erase(channels_.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }

  // New signals: local maxima above threshold that look keyed (quiet >20% of
  // the time), seen on two consecutive passes.
  if (frame_ < kWindow / 2) return;
  const int span = 2 * os_;  // +-1 resolution width
  for (int a = 0; a < nb; ++a) {
    bool cand = a >= span && a < nb - span && a >= bandLo_ && a <= bandHi_ && snr_[a] >= thr &&
                noiseRaw_[a] <= 3.0 * noise_[a];
    for (int d = 1; cand && d <= span; ++d)
      cand = peak_[a] >= peak_[a - d] && peak_[a] >= peak_[a + d];
    candCount_[a] = cand ? candCount_[a] + 1 : 0;
  }
  for (int a = span; a < nb - span; ++a) {
    if (static_cast<int>(channels_.size()) >= cfg_.maxChannels) break;
    if (candCount_[a] < 2) continue;
    // Is this signal already covered by a channel (checked first: cheap)?
    bool blocked = false;
    int replace = -1;
    for (size_t i = 0; i < channels_.size(); ++i) {
      const auto& ch = channels_[i];
      int d = std::abs(ch->a - a);
      if (d <= 4 * span && snr_[ch->a] > 300.0 * snr_[a]) { blocked = true; break; }
      if (d <= span + os_) {
        // A much stronger signal next to a weak channel (typically one opened on
        // noise just before this station started): the station takes over.
        double chSnr = std::max(snr_[ch->a], ch->sigRef / noise_[ch->a] / gainOf(ch->k));
        if (snr_[a] > 3.0 * chSnr) replace = static_cast<int>(i);
        else { blocked = true; break; }
      }
    }
    if (blocked) continue;
    if (isHarmonicImage(a)) continue;
    // Settle on the bin with the strongest robust signal estimate.
    int best = a;
    double bestLvl = signalLevel(a, kWindow);
    for (int d = -os_; d <= os_; ++d) {
      if (d == 0) continue;
      double l = signalLevel(a + d, kWindow);
      if (l > bestLvl) { bestLvl = l; best = a + d; }
    }
    if (blocked) continue;
    if (replace >= 0) {
      closeChannel(*channels_[static_cast<size_t>(replace)]);
      channels_.erase(channels_.begin() + replace);
    }
    createChannel(best);
  }
}

void Engine::findPassband() {
  // The receiver's passband is the one contiguous stretch of bins carrying
  // band noise. Smooth the per-bin floor with a narrow median (carriers and
  // keyed signals are narrower than it), take the strongest smoothed level as
  // the in-band reference, and place the edges at the outermost bins within
  // kEdgeDb of it. A radio's filter stopband, or the region beyond an SDR's
  // decimation filter, is far below that; ordinary passband ripple, roll-off
  // and the gaps between strong signals are not.
  constexpr double kEdge = 3e-4;  // -35 dB
  const int nb = static_cast<int>(bins_.size());
  const int half = 3 * os_;
  double ref = 0;
  double v[32];
  for (int a = 0; a < nb; ++a) {
    int n = 0;
    for (int b = std::max(0, a - half); b <= std::min(nb - 1, a + half) && n < 32; ++b) v[n++] = noiseRaw_[b];
    std::nth_element(v, v + n / 2, v + n);
    noiseLocal_[a] = v[n / 2];
    ref = std::max(ref, noiseLocal_[a]);
  }
  int lo = 0, hi = nb - 1;
  if (ref > 0) {
    while (lo < hi && noiseLocal_[lo] < kEdge * ref) ++lo;
    while (hi > lo && noiseLocal_[hi] < kEdge * ref) --hi;
  }
  bandLo_ = lo;
  bandHi_ = hi;
}

bool Engine::isHarmonicImage(int a) const {
  // Audio input only: an overdriven sound card or receiver audio stage adds
  // harmonics of every strong signal. They key exactly like the fundamental,
  // so they decode (and would be spotted) at 2x, 3x ... its pitch. A candidate
  // is such an image when a much stronger signal sits at an integer fraction
  // of its frequency and the two rise and fall together.
  if (cfg_.iq) return false;
  const double f = offsetOfBin(bins_[a]);
  const double binW = cfg_.sampleRate / nfft_;
  const int64_t avail = std::min<int64_t>(frame_ + 1, kWindow);
  for (int n = 2; n <= 5; ++n) {
    const double f0 = f / n;
    if (f0 < offsetOfBin(bins_.front()) - binW) break;
    int b = binOfOffset(f0);
    // The candidate bin fixes f only to within a bin, so the fundamental is
    // within a fraction of a bin of f/n: take the strongest of the neighbours.
    for (int d = -1; d <= 1; ++d) {
      int c = binOfOffset(f0) + d;
      if (c >= 0 && c < static_cast<int>(bins_.size()) && peak_[c] > peak_[b]) b = c;
    }
    if (std::fabs(offsetOfBin(bins_[b]) - f0) > 1.5 * binW) continue;
    if (peak_[b] < 10.0 * peak_[a]) continue;
    // Correlation of the two power histories (log domain, so that it measures
    // the keying pattern and not the level).
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    const double fa = noise_[a], fb = noise_[b];
    for (int64_t t = frame_ - avail + 1; t <= frame_; ++t) {
      double x = std::log(ring(a, t) + fa), y = std::log(ring(b, t) + fb);
      sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
    }
    const double m = static_cast<double>(avail);
    double cov = sab - sa * sb / m, va = saa - sa * sa / m, vb = sbb - sb * sb / m;
    if (va > 0 && vb > 0 && cov / std::sqrt(va * vb) > 0.6) return true;
  }
  return false;
}

double Engine::interpolatedOffset(int a) const {
  // Sub-bin frequency by parabolic interpolation of the log peak spectrum.
  double delta = 0;
  if (a > 0 && a + 1 < static_cast<int>(bins_.size())) {
    double ym = std::log(peak_[a - 1]), y0 = std::log(peak_[a]), yp = std::log(peak_[a + 1]);
    double den = ym - 2 * y0 + yp;
    if (den < 0) delta = std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5);
  }
  return offsetOfBin(bins_[a]) + delta * cfg_.sampleRate / nfft_;
}

double Engine::signalLevel(int a, int64_t window) const {
  // Same two-cluster estimate as channelSignal, on an FFT bin's history.
  const int64_t avail = std::min<int64_t>(frame_ + 1, window);
  const double n = noise_[a];
  double sig = std::max(peak_[a] - n, n);
  for (int it = 0; it < 6; ++it) {
    double thr = std::sqrt(n * (sig + n));
    double sum = 0;
    int cnt = 0;
    for (int64_t f = frame_ - avail + 1; f <= frame_; ++f) {
      double p = ring(a, f);
      if (p > thr) { sum += p; ++cnt; }
    }
    if (cnt == 0) break;
    sig = std::max(sum / cnt - n, n);
  }
  return sig;
}

void Engine::createChannel(int a) {
  DecoderParams dp;
  dp.lmWeight = cfg_.lmWeight;
  auto chp = std::make_unique<Channel>(dp, cfg_.beamWidth, cfg_.startPenalty, cfg_.knownCalls);
  Channel& ch = *chp;
  ch.id = nextId_++;
  ch.pow.assign(kRing, 0.0f);
  setFilter(ch, interpolatedOffset(a), 1);
  ch.lastTextFrame = frame_;

  // Replay buffered history so the first words of the transmission are kept.
  int64_t replay = std::min<int64_t>(frame_ + 1, kWindow);
  ch.keyerBase = frame_ - replay + 1;
  ch.llrHistStart = ch.keyerBase;
  recomputeHistory(ch, ch.keyerBase);
  ch.sigRef = signalLevel(ch.a, kWindow);  // seed for the cluster split
  ch.sig = ch.sigRef = std::max(channelSignal(ch, ch.keyerBase, frame_), noise_[ch.a]);
  if (debugLevel())
    std::fprintf(stderr, "[ch%d f=%lld] create at %.1f Hz sig/noise=%.1f\n", ch.id,
                 static_cast<long long>(frame_), ch.freqHz, ch.sig / noise_[ch.a]);
  channels_.push_back(std::move(chp));
  for (int64_t f = ch.keyerBase; f <= frame_; ++f) runChannel(ch, ch.pow[static_cast<size_t>(f % kRing)]);
}

void Engine::closeChannel(Channel& ch) {
  ch.events.clear();
  ch.keyer.flush(ch.events);
  for (const KeyEvent& e : ch.events) {
    if (e.type == KeyEvent::Mark) ch.decoder.mark(e.len);
    else ch.decoder.space(e.len, e.type == KeyEvent::Space);
  }
  ch.decoder.end();
  std::string t = ch.decoder.take();
  if (cfg_.softDecode && ch.softActive) {
    ch.soft.end();
    t = ch.soft.take();
  } else if (cfg_.softDecode && ch.decoder.locked()) {
    ch.soft.setTiming(ch.decoder.timing());
    for (float v : ch.llrHist) ch.soft.push(v);
    ch.soft.end();
    t = ch.soft.take();
  }
  if (!t.empty()) handleText(ch, t);
  handleText(ch, " ");  // completes the final word for callsign extraction
  emitLine(ch);
}

void Engine::finish() {
  for (auto& ch : channels_) closeChannel(*ch);
  channels_.clear();
}

}  // namespace cw
