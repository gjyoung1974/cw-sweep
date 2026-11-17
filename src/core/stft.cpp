#include "stft.hpp"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace cw {
namespace {
std::mutex& fftwMutex() {  // FFTW planner is not thread-safe
  static std::mutex m;
  return m;
}
}  // namespace

Stft::Stft(int fftSize, int windowLen, int hop) : n_(fftSize), win_(windowLen), hop_(hop) {
  window_.resize(win_);
  double s1 = 0, s2 = 0;
  for (int i = 0; i < win_; ++i) {
    window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * i / win_));  // periodic Hann
    s1 += window_[i];
    s2 += window_[i] * window_[i];
  }
  enbw_ = win_ * s2 / (s1 * s1);
  scale_ = static_cast<float>(1.0 / s2);
  ring_.assign(win_, cfloat{});
  power_.resize(n_);
  spec_.resize(n_);
  std::lock_guard lk(fftwMutex());
  in_ = reinterpret_cast<cfloat*>(fftwf_malloc(sizeof(fftwf_complex) * n_));
  out_ = reinterpret_cast<cfloat*>(fftwf_malloc(sizeof(fftwf_complex) * n_));
  plan_ = fftwf_plan_dft_1d(n_, reinterpret_cast<fftwf_complex*>(in_),
                            reinterpret_cast<fftwf_complex*>(out_), FFTW_FORWARD, FFTW_ESTIMATE);
}

Stft::~Stft() {
  std::lock_guard lk(fftwMutex());
  fftwf_destroy_plan(plan_);
  fftwf_free(in_);
  fftwf_free(out_);
}

double Stft::windowCorrelation(int m) const {
  double num = 0, den = 0;
  for (int i = 0; i < win_; ++i) {
    den += static_cast<double>(window_[i]) * window_[i];
    int j = i + m * hop_;
    if (j < win_) num += static_cast<double>(window_[i]) * window_[j];
  }
  return num / den;
}

void Stft::push(const cfloat* x, size_t n, const FrameFn& onFrame) {
  for (size_t i = 0; i < n; ++i) {
    ring_[ringPos_] = x[i];
    ringPos_ = (ringPos_ + 1) % win_;
    if (filled_ < static_cast<size_t>(win_)) ++filled_;
    if (++sinceHop_ < hop_ || filled_ < static_cast<size_t>(win_)) continue;
    sinceHop_ = 0;
    for (int k = 0; k < win_; ++k) in_[k] = ring_[(ringPos_ + k) % win_] * window_[k];
    for (int k = win_; k < n_; ++k) in_[k] = cfloat{};
    fftwf_execute(plan_);
    const float amp = std::sqrt(scale_);
    for (int k = 0; k < n_; ++k) {
      spec_[k] = out_[k] * amp;
      power_[k] = std::norm(spec_[k]);
    }
    onFrame(power_.data(), spec_.data());
  }
}

NoiseBlanker::NoiseBlanker(double sampleRate, double threshold)
    : threshold_(threshold),
      alpha_(1.0 / (0.05 * sampleRate)),
      maxRun_(std::max(1, static_cast<int>(0.008 * sampleRate))),
      gap_(std::max(1, static_cast<int>(0.004 * sampleRate))),
      guard_(std::max(1, static_cast<int>(0.0005 * sampleRate))) {
  buf_.assign(static_cast<size_t>(maxRun_ + gap_ + 2 * guard_ + 2), cfloat{});
}

void NoiseBlanker::process(cfloat* x, size_t n) {
  const auto D = static_cast<int64_t>(buf_.size());
  for (size_t i = 0; i < n; ++i, ++in_) {
    const cfloat s = x[i];
    const double m = std::abs(s);
    if (!primed_) { avg_ = std::max(m, 1e-12); primed_ = true; }

    // Close the current excursion once it has been quiet for `gap_` samples.
    if (runStart_ >= 0 && in_ - lastAbove_ > gap_) {
      if (lastAbove_ - runStart_ < maxRun_) blank_.push_back({runStart_ - guard_, lastAbove_ + guard_});
      runStart_ = -1;
    }
    if (m > threshold_ * avg_) {
      if (runStart_ < 0) runStart_ = in_;
      lastAbove_ = in_;
    }
    // Background level: frozen during a short excursion, follows long ones
    // (a real signal) and everything else.
    if (runStart_ < 0 || in_ - runStart_ >= maxRun_) avg_ += alpha_ * (m - avg_);

    // Output the sample from D samples ago.
    const int64_t outIdx = in_ - D;
    const auto slot = static_cast<size_t>(in_ % D);
    cfloat out = buf_[slot];
    buf_[slot] = s;
    while (!blank_.empty() && blank_.front().second < outIdx) blank_.erase(blank_.begin());
    if (!blank_.empty() && outIdx >= blank_.front().first) out = cfloat{};
    x[i] = out;
  }
}

}  // namespace cw
