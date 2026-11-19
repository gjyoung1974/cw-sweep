#include "keyer.hpp"

#include <algorithm>
#include <cmath>

namespace cw {

double logBesselI0(double z) {
  z = std::fabs(z);
  if (z < 3.75) {
    double t = (z / 3.75) * (z / 3.75);
    double v = 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 +
               t * (0.2659732 + t * (0.0360768 + t * 0.0045813)))));
    return std::log(v);
  }
  double t = 3.75 / z;
  double p = 0.39894228 + t * (0.01328592 + t * (0.00225319 + t * (-0.00157565 +
             t * (0.00916281 + t * (-0.02057706 + t * (0.02635537 +
             t * (-0.01647633 + t * 0.00392377)))))));
  return z - 0.5 * std::log(z) + std::log(p);
}

double keyLlrFading(double P, double N, double S, double minSnr) {
  static constexpr double kScale[] = {0.1, 0.25, 0.5, 1.0, 2.0};
  static constexpr double kWeight[] = {0.1, 0.15, 0.2, 0.35, 0.2};
  double l[5], m = -1e300, wsum = 0;
  int n = 0;
  for (int i = 0; i < 5; ++i) {
    if (kScale[i] < 1.0 && S * kScale[i] < minSnr * N) continue;
    l[n] = keyLlr(P, N, S * kScale[i]) + std::log(kWeight[i]);
    wsum += kWeight[i];
    m = std::max(m, l[n]);
    ++n;
  }
  double acc = 0;
  for (int i = 0; i < n; ++i) acc += std::exp(l[i] - m);
  return m + std::log(acc / wsum);
}

Keyer::Keyer() { setTiming(10.0, 3.0); }

void Keyer::setTiming(double ditFrames, double minRunFrames, double switchRate) {
  double p = std::clamp(switchRate / std::max(ditFrames, 1.0), 0.005, 0.3);
  stay_ = std::log(1.0 - p);
  switch_ = std::log(p);
  lag_ = std::clamp(static_cast<int>(std::lround(2.5 * ditFrames)) + 10, 25, 300);
  minRun_ = std::max(1.0, minRunFrames);
}

void Keyer::push(double llr, std::vector<KeyEvent>& events,
                 std::vector<std::pair<int64_t, int>>& committed) {
  ++t_;
  double a = s_[0] + stay_, b = s_[1] + switch_;
  double c = s_[1] + stay_, d = s_[0] + switch_;
  double n0 = std::max(a, b);
  double n1 = std::max(c, d) + llr;
  uint8_t prev0 = b > a ? 1 : 0;
  uint8_t prev1 = d > c ? 0 : 1;
  bp_[t_ % kRing] = static_cast<uint8_t>(prev0 | (prev1 << 1));
  double m = std::max(n0, n1);
  s_[0] = n0 - m;
  s_[1] = n1 - m;

  int64_t pending = t_ - committedUpTo_;
  if (pending <= lag_) return;
  // Trace back from the best current state through all uncommitted frames.
  scratch_.resize(static_cast<size_t>(pending));
  int s = s_[1] > s_[0] ? 1 : 0;
  for (int64_t k = 0; k < pending; ++k) {
    int64_t idx = t_ - k;
    scratch_[static_cast<size_t>(pending - 1 - k)] = s;
    s = (bp_[idx % kRing] >> s) & 1;
  }
  int64_t lastToCommit = t_ - lag_;
  for (int64_t f = committedUpTo_ + 1, j = 0; f <= lastToCommit; ++f, ++j) {
    int st = scratch_[static_cast<size_t>(j)];
    committed.emplace_back(f, st);
    commit(st, events);
  }
  committedUpTo_ = lastToCommit;
}

void Keyer::flush(std::vector<KeyEvent>& events) {
  int64_t pending = t_ - committedUpTo_;
  if (pending > 0) {
    scratch_.resize(static_cast<size_t>(pending));
    int s = s_[1] > s_[0] ? 1 : 0;
    for (int64_t k = 0; k < pending; ++k) {
      scratch_[static_cast<size_t>(pending - 1 - k)] = s;
      s = (bp_[(t_ - k) % kRing] >> s) & 1;
    }
    for (int st : scratch_) commit(st, events);
    committedUpTo_ = t_;
  }
  if (curState_ == 1 && curLen_ > 0) {
    runComplete(1, curLen_, events);
    curState_ = 0;
    curLen_ = 0;
  }
  for (const Run& r : q_) events.push_back({r.state ? KeyEvent::Mark : KeyEvent::Space, r.len});
  q_.clear();
}

void Keyer::commit(int state, std::vector<KeyEvent>& events) {
  if (state == curState_) {
    curLen_ += 1;
  } else {
    runComplete(curState_, curLen_, events);
    curState_ = state;
    curLen_ = 1;
  }
  if (curState_ == 0 && curLen_ >= minRun_) {
    // An established key-up: a short mark just before it was a noise glitch.
    if (q_.size() >= 2 && q_.back().state == 1 && q_.back().len < minRun_) {
      curLen_ += q_.back().len + q_[q_.size() - 2].len;
      q_.pop_back();
      q_.pop_back();
    }
    for (const Run& r : q_) events.push_back({r.state ? KeyEvent::Mark : KeyEvent::Space, r.len});
    q_.clear();
    events.push_back({KeyEvent::SpaceProgress, curLen_});
  }
}

void Keyer::runComplete(int state, double len, std::vector<KeyEvent>& events) {
  if (len <= 0) return;
  q_.push_back({state, len});
  drainQueue(events);
}

void Keyer::drainQueue(std::vector<KeyEvent>& events) {
  while (q_.size() >= 3) {
    if (q_[1].len < minRun_) {
      // Glitch: absorb it and the following run into the preceding run.
      q_[0].len += q_[1].len + q_[2].len;
      q_.erase(q_.begin() + 1, q_.begin() + 3);
      continue;
    }
    const Run& r = q_.front();
    events.push_back({r.state ? KeyEvent::Mark : KeyEvent::Space, r.len});
    q_.erase(q_.begin());
  }
}

}  // namespace cw
