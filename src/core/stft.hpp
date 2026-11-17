#pragma once
// Short-time Fourier transform channelizer.
//
// Splits the whole receiver passband into narrow bins (default 50 Hz wide,
// one frame every 5 ms). Each bin acts as a filter matched to a CW element of
// roughly 20 ms, which is what lets us decode every signal in the passband in
// parallel.

#include <complex>
#include <functional>
#include <vector>

struct fftwf_plan_s;

namespace cw {

using cfloat = std::complex<float>;

class Stft {
 public:
  // windowLen <= fftSize; the remainder is zero padding (finer bin spacing,
  // same resolution, lower scalloping loss).
  Stft(int fftSize, int windowLen, int hop);
  ~Stft();
  Stft(const Stft&) = delete;
  Stft& operator=(const Stft&) = delete;

  int size() const { return n_; }
  int hop() const { return hop_; }
  // Equivalent noise bandwidth of the window, in bins.
  double enbwBins() const { return enbw_; }

  // Feed samples; invokes `onFrame(power, spectrum)` for every completed
  // frame, both with `size()` entries in FFT order. Normalised so that complex
  // white noise of variance v per sample gives E[power] = E|spectrum|^2 = v.
  // The spectrum's phase is referenced to the first sample of the frame.
  using FrameFn = std::function<void(const float*, const cfloat*)>;
  void push(const cfloat* x, size_t n, const FrameFn& onFrame);
  // Normalised autocorrelation of the window at lags of m hops (noise
  // correlation between frames m hops apart, after phase alignment).
  double windowCorrelation(int m) const;

 private:
  int n_, win_, hop_;
  double enbw_ = 1.5;
  std::vector<float> window_;
  std::vector<cfloat> ring_;   // last win_ samples
  size_t ringPos_ = 0;
  size_t filled_ = 0;
  int sinceHop_ = 0;
  cfloat* in_ = nullptr;
  cfloat* out_ = nullptr;
  fftwf_plan_s* plan_ = nullptr;
  std::vector<float> power_;
  std::vector<cfloat> spec_;
  float scale_ = 1.0f;
};

// Impulse noise blanker operating on raw samples.
//
// Samples are delayed by a few milliseconds so that each excursion above
// `threshold` times the background magnitude can be measured before it is
// output. Above-threshold samples separated by dips shorter than `gap_` belong
// to the same excursion (for real audio a tone's magnitude touches zero twice
// per cycle). Only short excursions (lightning crashes, ignition and
// power-line clicks) are blanked, with a guard interval; anything longer is a
// real signal, however strong, and passes untouched.
class NoiseBlanker {
 public:
  NoiseBlanker(double sampleRate, double threshold);
  void process(cfloat* x, size_t n);

 private:
  double threshold_;
  double avg_ = 0.0;
  double alpha_;
  int maxRun_, gap_, guard_;
  std::vector<cfloat> buf_;
  int64_t in_ = 0;            // samples received
  int64_t runStart_ = -1;     // first above-threshold sample of current excursion
  int64_t lastAbove_ = -1;    // most recent above-threshold sample
  std::vector<std::pair<int64_t, int64_t>> blank_;  // pending [start, end] ranges
  bool primed_ = false;
};

}  // namespace cw
