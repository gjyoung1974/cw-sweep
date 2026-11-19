#pragma once
// Multi-channel CW decoding engine.
//
//   samples -> [noise blanker] -> STFT channelizer -> per-bin noise floor
//           -> signal detection (every bin, 10x per second)
//           -> per-channel: Rician LLR -> fixed-lag Viterbi keyer
//                           -> Bayesian beam-search decoder -> text
//           -> callsign extraction -> spots

#include <algorithm>
#include <complex>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "stft.hpp"

namespace cw {

class KnownCalls;

struct EngineConfig {
  double sampleRate = 8000;
  bool iq = false;             // complex I/Q input (wideband SDR)
  double centerHz = 0;         // dial frequency (audio) or LO frequency (I/Q)
  bool lsb = false;            // audio: RF = dial - audio offset
  double fmin = 200, fmax = 3500;  // decoded offset range, Hz (I/Q: may be negative)
  double binHz = 50;           // STFT resolution (window length = 1/binHz)
  int oversample = 2;          // FFT zero-padding factor (bin spacing = binHz/oversample)
  double hopMs = 5;            // STFT frame step
  double thresholdDb = 4.0;    // detection threshold, signal/noise in one bin
  bool fadingModel = true;     // marginalise LLR over signal level (QSB robustness)
  double fadeFloorDb = 10.0;   // weakest faded level considered, dB over noise
  bool noiseBlanker = true;
  double nbThreshold = 8.0;
  int maxChannels = 500;
  double lmWeight = 0.4;
  double llrScale = 0.6;       // compensates for correlation between overlapping frames
  double matchFactor = 0.7;    // channel filter length as a fraction of the dit
  double switchRate = 0.6;     // keyer HMM: expected state changes per dit
  bool softDecode = true;      // joint soft-decision decoder (vs. hard run-length decoder)
  int beamWidth = 64;
  double startPenalty = 8.0;   // soft decoder: prior against keying after long silence
  double idleTimeoutSec = 12;  // drop channels silent for this long
  double respotSec = 600;      // don't re-spot the same call/freq within this
  int lineWidth = 64;
  std::string spotter = "N0CALL-#";
  const KnownCalls* knownCalls = nullptr;  // optional MASTER.SCP
};

struct ChannelInfo {
  int id = 0;
  double offsetHz = 0;   // audio / baseband offset
  double rfHz = 0;       // absolute frequency
  double wpm = 0;
  double snrDb = 0;
  bool locked = false;
  double spread = 0;     // relative timing spread: ~0.1-0.3 for real CW, higher for noise
  std::string recent;    // last ~200 decoded characters
};

struct Spot {
  std::string spotter;
  std::string call;
  double rfHz = 0;
  double snrDb = 0;
  double wpm = 0;
  bool cq = false;
  double streamTime = 0;
  // DX-cluster formatted line, timestamped with the current UTC time.
  std::string format() const;
};

class Engine {
 public:
  explicit Engine(EngineConfig cfg);
  ~Engine();

  void process(const cfloat* x, size_t n);
  void processReal(const float* x, size_t n);
  // End of stream: flush all decoders.
  void finish();

  std::vector<ChannelInfo> channels() const;
  double streamTime() const;
  const EngineConfig& config() const { return cfg_; }
  int fftSize() const { return nfft_; }
  int windowLen() const { return win_; }
  int hop() const { return hop_; }

  // Decoded text, a line at a time (on line width, or channel going idle).
  std::function<void(const ChannelInfo&, const std::string&)> onLine;
  // Every decoded fragment as soon as it is committed.
  std::function<void(const ChannelInfo&, const std::string&)> onText;
  std::function<void(const Spot&)> onSpot;

 private:
  struct Channel;
  void frame(const float* power, const cfloat* spec);
  void detect();
  void createChannel(int a);
  void runChannel(Channel& ch, double power);
  void handleText(Channel& ch, const std::string& text);
  void emitLine(Channel& ch);
  void closeChannel(Channel& ch);
  ChannelInfo info(const Channel& ch) const;
  double offsetOfBin(double k) const;
  int binOfOffset(double hz) const;
  double interpolatedOffset(int a) const;
  double signalLevel(int a, int64_t window) const;
  void findPassband();
  bool isHarmonicImage(int a) const;
  double channelSignal(const Channel& ch, int64_t from, int64_t to) const;
  // Channel filter: coherent sum of K consecutive phase-aligned STFT frames
  // of the channel's bin (Hann window convolved with a K-frame boxcar).
  void setFilter(Channel& ch, double hz, int k);
  std::complex<double> aligned(const Channel& ch, int64_t f) const;
  double filterPower(Channel& ch, int64_t f);
  int64_t recomputeHistory(Channel& ch, int64_t from);  // returns first recomputed frame
  double noiseGain(int k) const;
  // SNR gain of a K-frame filter for a steady carrier.
  double gainOf(int k) const { return static_cast<double>(k) * k / noiseGain(k); }
  // Filter length matched to the shortest expected element (dit or element
  // gap; keying weight makes one shorter than the other).
  int matchedK(double shortestFrames) const {
    double l1 = static_cast<double>(win_) / hop_;
    return std::clamp(static_cast<int>(std::lround(cfg_.matchFactor * shortestFrames - (l1 - 1))), 1, kMaxK);
  }
  double llrScaleFor(const Channel& ch) const;
  // Frame-major history rings: one contiguous row of all bins per frame.
  size_t slot(int64_t f) const { return static_cast<size_t>(f % kRing) * bins_.size(); }
  float& ring(int a, int64_t f) { return ring_[slot(f) + static_cast<size_t>(a)]; }
  float ring(int a, int64_t f) const { return ring_[slot(f) + static_cast<size_t>(a)]; }
  cfloat cring(int a, int64_t f) const { return cring_[slot(f) + static_cast<size_t>(a)]; }

  static constexpr int kRing = 512;     // frames of history kept per bin
  static constexpr int kWindow = 400;   // frames used for noise/detection statistics
  static constexpr int kDetectEvery = 40;  // frames between detection passes (200 ms)
  static constexpr size_t kChunk = 4096;

  EngineConfig cfg_;
  int nfft_ = 0, win_ = 0, hop_ = 0;
  int os_ = 2;                     // bins per resolution width
  static constexpr int kMaxK = 24;  // longest channel filter, in frames
  std::vector<double> noiseGain_;  // noise power gain of a K-frame coherent sum
  double hopSec_ = 0;
  std::unique_ptr<Stft> stft_;
  std::unique_ptr<NoiseBlanker> nb_;
  std::vector<int> bins_;          // active FFT bin per active index (ascending frequency)
  std::vector<float> ring_;        // power per bin per frame
  std::vector<cfloat> cring_;      // complex spectrum per bin per frame
  std::vector<double> noise_, noiseRaw_, noiseLocal_, peak_, snr_;
  int bandLo_ = 0, bandHi_ = 0;    // active-bin range inside the receiver's passband
  std::vector<int> candCount_;
  std::vector<float> detectBuf_;
  std::vector<std::unique_ptr<Channel>> channels_;
  std::map<std::string, std::pair<double, double>> lastSpot_;  // call -> (time, rf)
  std::vector<cfloat> scratch_, chunk_;
  int64_t frame_ = -1;
  int nextId_ = 1;
  bool noiseReady_ = false;
};

}  // namespace cw
