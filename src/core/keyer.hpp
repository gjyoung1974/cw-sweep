#pragma once
// Key-up / key-down detection for one channel.
//
// Per frame we compute the exact log-likelihood ratio of "carrier present" vs
// "noise only" for a Rician signal in complex Gaussian noise:
//
//   LLR = -SNR + ln I0( 2 sqrt(SNR * P/N) )
//
// A two-state HMM with speed-dependent switching probability is decoded with
// a fixed-lag Viterbi, which integrates evidence over each element instead of
// thresholding single frames. The resulting runs are cleaned of glitches
// shorter than a fraction of a dit and emitted as mark/space durations.

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace cw {

double logBesselI0(double z);

// LLR of key-down given bin power P, noise power N and signal power S.
inline double keyLlr(double P, double N, double S) {
  double snr = S / N;
  double x = P / N;
  return -snr + logBesselI0(2.0 * std::sqrt(snr * x));
}

// LLR marginalised over an uncertain signal level (a mixture of Rician
// components below and above the nominal level S). Robust to QSB: a mark that
// has faded 10 dB is still recognised as a mark rather than as noise.
// Components weaker than `minSnr` (linear, relative to N) are excluded: a
// fade into the noise cannot be told apart from noise, and including it would
// make every noise peak look like a possible mark.
double keyLlrFading(double P, double N, double S, double minSnr);

struct KeyEvent {
  enum Type : uint8_t { Mark, Space, SpaceProgress } type;
  double len;  // frames
};

class Keyer {
 public:
  Keyer();
  // Expected dit length in frames; tunes HMM switching and glitch filter.
  void setTiming(double ditFrames, double minRunFrames, double switchRate = 0.6);
  // Push one frame's LLR. Appends (frame index, state) of newly committed
  // frames to `committed` (oldest first; index counts frames pushed so far,
  // starting at 0) and key events to `events`.
  void push(double llr, std::vector<KeyEvent>& events,
            std::vector<std::pair<int64_t, int>>& committed);
  // Commit everything (end of stream).
  void flush(std::vector<KeyEvent>& events);
  int lag() const { return lag_; }

 private:
  void commit(int state, std::vector<KeyEvent>& events);
  void runComplete(int state, double len, std::vector<KeyEvent>& events);
  void drainQueue(std::vector<KeyEvent>& events);

  static constexpr int kRing = 1024;
  double s_[2] = {0.0, -10.0};
  double stay_ = 0, switch_ = 0;
  uint8_t bp_[kRing] = {};   // bit k = predecessor of state k
  int64_t t_ = -1;           // index of newest frame
  int64_t committedUpTo_ = -1;
  int lag_ = 40;
  double minRun_ = 2.0;

  int curState_ = 0;
  double curLen_ = 0;
  struct Run { int state; double len; };
  std::vector<Run> q_;
  std::vector<int> scratch_;
};

}  // namespace cw
