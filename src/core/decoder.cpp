#include "decoder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cw {
namespace {
constexpr size_t kHistory = 48;
constexpr double kObsCap = 10.0;
constexpr double kLmBaseline = -3.0;   // typical log P per character
constexpr double kInvalidPenalty = 10.0;
constexpr double kPriorElem = 0.6, kPriorChar = 0.3, kPriorWord = 0.1;

inline double sq(double x) { return x * x; }
}  // namespace

// ---------------------------------------------------------------------------
// Timing estimation
// ---------------------------------------------------------------------------

void TimingEstimator::add(bool mark, double len) {
  hist_.push_back({mark, len});
  while (hist_.size() > kHistory) hist_.pop_front();
}

double TimingEstimator::cost(double dit, double bias) const {
  // Approximately -2 log-likelihood: squared normalised duration error plus
  // -2 log prior of the element class. Real Morse has more element gaps than
  // character gaps; without these priors "five dits" and "five Ts" (all marks
  // dahs, all gaps character gaps) fit equally well.
  constexpr double sr = 0.2, sa = 0.7;
  constexpr double pDit = 1.2, pDah = 1.6, pElem = 1.2, pChar = 2.2, pWord = 4.2;
  double total = 0;
  int elemGaps = 0, charGaps = 0;
  for (const Obs& o : hist_) {
    double e;
    if (o.mark) {
      double m1 = dit + bias, m3 = 3 * dit + bias;
      e = std::min(sq((o.len - m1) / (sr * m1 + sa)) + pDit, sq((o.len - m3) / (sr * m3 + sa)) + pDah);
    } else {
      double g1 = std::max(0.5, dit - bias), g3 = std::max(0.5, 3 * dit - bias);
      double g7 = std::max(0.5, 7 * dit - bias);
      double e1 = sq((o.len - g1) / (sr * g1 + sa)) + pElem;
      double e3 = sq((o.len - g3) / (sr * g3 + sa)) + pChar;
      double e7 = (o.len >= g7 ? 0.0 : sq((o.len - g7) / (0.35 * g7 + sa))) + pWord;
      e = std::min({e1, e3, e7});
      if (e == e1) ++elemGaps;
      else if (e == e3) ++charGaps;
    }
    total += std::min(e, kObsCap);
  }
  int gaps = elemGaps + charGaps;
  if (gaps >= 4) {
    double fe = static_cast<double>(elemGaps) / gaps;
    if (fe < 0.3) total += 3.0 * gaps * (0.3 - fe) / 0.3;
  }
  // Prior: keying weight is usually modest.
  total += 1.0 * static_cast<double>(hist_.size()) * sq(bias / dit);
  return total;
}

double TimingEstimator::spread(double dit, double bias) const {
  // Robust relative spread of durations around their assigned class means
  // (word gaps excluded: their length is not well defined).
  std::vector<double> r;
  r.reserve(hist_.size());
  for (const Obs& o : hist_) {
    double best = 1e9;
    if (o.mark) {
      for (double mu : {dit + bias, 3 * dit + bias})
        if (std::fabs(o.len - mu) / mu < std::fabs(best)) best = (o.len - mu) / mu;
    } else {
      for (double mu : {std::max(0.5, dit - bias), std::max(0.5, 3 * dit - bias)})
        if (std::fabs(o.len - mu) / mu < std::fabs(best)) best = (o.len - mu) / mu;
      if (o.len > 5 * dit) continue;
    }
    r.push_back(std::fabs(best));
  }
  if (r.size() < 8) return 0.22;
  std::nth_element(r.begin(), r.begin() + static_cast<long>(r.size() / 2), r.end());
  double mad = r[r.size() / 2];
  // Median absolute deviation -> std-dev, plus a floor for frame quantisation.
  double sigma = std::sqrt(std::pow(1.4826 * mad, 2) + std::pow(0.7 / dit, 2));
  return std::clamp(sigma, 0.12, 0.45);
}

bool TimingEstimator::fit(Timing& t, bool force) const {
  if (hist_.empty()) return false;
  double bestD = t.dit, bestB = t.bias;
  double best = std::numeric_limits<double>::infinity();
  for (double d = 1.5; d <= 120.0; d *= 1.04) {
    for (int bi = -4; bi <= 6; ++bi) {
      double b = 0.1 * bi * d;
      double c = cost(d, b);
      if (c < best) { best = c; bestD = d; bestB = b; }
    }
  }
  double d0 = bestD, b0 = bestB;
  for (int di = -6; di <= 6; ++di) {
    for (int bi = -4; bi <= 4; ++bi) {
      double d = d0 * (1.0 + 0.005 * di);
      double b = b0 + 0.02 * bi * d0;
      double c = cost(d, b);
      if (c < best) { best = c; bestD = d; bestB = b; }
    }
  }
  bool changed = true;
  if (!force) {
    double cur = cost(t.dit, t.bias);
    double jump = std::fabs(std::log(bestD / t.dit));
    double margin = jump > 0.2 ? 4.0 : 0.3;
    changed = best < cur - margin;
  }
  if (changed) {
    t.dit = bestD;
    t.bias = bestB;
  }
  double sg = spread(t.dit, t.bias);
  if (std::fabs(sg - t.sigma) > 0.02) { t.sigma = sg; changed = true; }
  return changed;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

Decoder::Decoder(const CharLM& lm, DecoderParams p) : lm_(&lm), p_(p) {
  beam_.push_back({0.0, {}, kEmptyPattern, ' '});
}

double Decoder::minRun() const {
  if (!locked_) return 1.5;
  return std::max(1.0, 0.4 * std::min(timing_.markMean(0), timing_.gapMean(0)));
}

double Decoder::logNorm(double x, double mu, double sig) const {
  return -0.5 * sq((x - mu) / sig) - std::log(sig);
}

double Decoder::markLL(double len, int dah) const {
  double mu = timing_.markMean(dah);
  return logNorm(len, mu, timing_.sigma * mu + p_.sigmaAbs) + std::log(0.5);
}

double Decoder::gapLL(double len, int cls) const {
  double mu = timing_.gapMean(cls);
  if (cls == 2) {
    double s = 0.35 * mu + p_.sigmaAbs;
    return len >= mu ? -std::log(s) : logNorm(len, mu, s);
  }
  return logNorm(len, mu, timing_.sigma * mu + p_.sigmaAbs);
}

double Decoder::charThreshold() const {
  return 1.1 * std::sqrt(timing_.gapMean(0) * timing_.gapMean(1));
}

double Decoder::wordThreshold() const {
  return 1.1 * std::sqrt(timing_.gapMean(1) * timing_.gapMean(2));
}

void Decoder::mark(double len) {
  seenMark_ = true;
  est_.add(true, len);
  if (!locked_) {
    pending_.push_back({true, len});
    if (est_.size() >= p_.lockRuns) lockAndReplay();
    return;
  }
  doMark(len);
  if (++sinceFit_ >= 4) { sinceFit_ = 0; est_.fit(timing_, false); }
}

void Decoder::space(double len, bool final) {
  if (!seenMark_) return;
  if (!locked_) {
    if (final) {
      est_.add(false, len);
      pending_.push_back({false, len});
      if (est_.size() >= p_.lockRuns) lockAndReplay();
      return;
    }
    // A long silence after a short burst: decode what we have.
    double longestMark = 0;
    for (const Event& e : pending_) if (e.mark) longestMark = std::max(longestMark, e.len);
    if (est_.size() >= 3 && len > 3.0 * longestMark) {
      lockAndReplay();
    } else {
      return;
    }
  }
  if (final) {
    doFinalSpace(len);
    est_.add(false, len);
    if (++sinceFit_ >= 4) { sinceFit_ = 0; est_.fit(timing_, false); }
  } else {
    doProgress(len);
  }
}

void Decoder::end() {
  if (!seenMark_) return;
  if (!locked_) lockAndReplay();
  doProgress(1e9);
  commitBest();
}

std::string Decoder::take() {
  std::string s;
  s.swap(out_);
  return s;
}

void Decoder::lockAndReplay() {
  est_.fit(timing_, true);
  locked_ = true;
  beam_.assign(1, {0.0, {}, kEmptyPattern, ' '});
  std::vector<Event> ev;
  ev.swap(pending_);
  for (const Event& e : ev) {
    if (e.mark) doMark(e.len);
    else doFinalSpace(e.len);
  }
}

void Decoder::doMark(double len) {
  spaceState_ = 0;
  std::vector<Hyp> next;
  next.reserve(beam_.size() * 3);
  double ll[2] = {markLL(len, 0), markLL(len, 1)};
  for (const Hyp& h : beam_) {
    bool anyValid = false;
    if (h.pat != kInvalidPattern) {
      for (int dah = 0; dah < 2; ++dah) {
        Pattern np = extendPattern(h.pat, dah);
        if (isPrefix(np)) {
          next.push_back({h.score + ll[dah], h.text, np, h.prev});
          anyValid = true;
        }
      }
    }
    // Escape hatch so the beam never empties on garbage: an undecodable char.
    double pen = h.pat == kInvalidPattern ? 0.0 : kInvalidPenalty;
    if (!anyValid || h.pat == kInvalidPattern || next.size() < 2)
      next.push_back({h.score + std::max(ll[0], ll[1]) - pen, h.text, kInvalidPattern, h.prev});
  }
  beam_.swap(next);
  prune();
}

void Decoder::emitChar(Hyp& h, bool word) const {
  if (h.pat != kEmptyPattern) {
    char c = h.pat == kInvalidPattern ? 0 : decodePattern(h.pat);
    if (c == 0) {
      c = '*';
      h.score -= kInvalidPenalty;
    } else {
      h.score += p_.lmWeight * (lm_->logp(h.prev, c) - kLmBaseline);
    }
    h.text.push_back(c);
    h.prev = c;
    h.pat = kEmptyPattern;
  }
  if (word && h.prev != ' ') {
    h.score += p_.lmWeight * (lm_->logp(h.prev, ' ') - kLmBaseline);
    h.text.push_back(' ');
    h.prev = ' ';
  }
}

void Decoder::doFinalSpace(double len) {
  if (spaceState_ != 0) {
    // Character (and maybe word) already emitted while the gap was running.
    if (spaceState_ == 1 && len >= wordThreshold()) {
      for (Hyp& h : beam_) emitChar(h, true);
      commitBest();
    }
    spaceState_ = 0;
    return;
  }
  std::vector<Hyp> next;
  next.reserve(beam_.size() * 3);
  double le = gapLL(len, 0) + std::log(kPriorElem);
  double lc = gapLL(len, 1) + std::log(kPriorChar);
  double lw = gapLL(len, 2) + std::log(kPriorWord);
  for (const Hyp& h : beam_) {
    if (h.pat != kEmptyPattern) next.push_back({h.score + le, h.text, h.pat, h.prev});
    Hyp c = h;
    c.score += lc;
    emitChar(c, false);
    next.push_back(std::move(c));
    Hyp w = h;
    w.score += lw;
    emitChar(w, true);
    next.push_back(std::move(w));
  }
  beam_.swap(next);
  prune();
  commitCommonPrefix();
}

void Decoder::doProgress(double len) {
  if (spaceState_ == 0 && len >= charThreshold()) {
    for (Hyp& h : beam_) emitChar(h, false);
    prune();
    commitBest();
    spaceState_ = 1;
  }
  if (spaceState_ == 1 && len >= wordThreshold()) {
    for (Hyp& h : beam_) emitChar(h, true);
    commitBest();
    spaceState_ = 2;
  }
}

void Decoder::prune() {
  std::sort(beam_.begin(), beam_.end(), [](const Hyp& a, const Hyp& b) { return a.score > b.score; });
  std::vector<Hyp> kept;
  kept.reserve(p_.beamWidth);
  double best = beam_.empty() ? 0.0 : beam_.front().score;
  for (Hyp& h : beam_) {
    if (h.score < best - 30.0) break;
    bool dup = false;
    for (const Hyp& k : kept)
      if (k.pat == h.pat && k.text == h.text) { dup = true; break; }
    if (dup) continue;
    h.score -= best;
    kept.push_back(std::move(h));
    if (kept.size() >= static_cast<size_t>(p_.beamWidth)) break;
  }
  beam_.swap(kept);
}

void Decoder::commitCommonPrefix() {
  if (beam_.empty()) return;
  size_t n = beam_[0].text.size();
  for (const Hyp& h : beam_) {
    size_t i = 0;
    while (i < n && i < h.text.size() && h.text[i] == beam_[0].text[i]) ++i;
    n = i;
  }
  if (n == 0) return;
  std::string s = beam_[0].text.substr(0, n);
  for (Hyp& h : beam_) h.text.erase(0, n);
  for (char c : s) {
    if (c == ' ' && (out_.empty() ? lastOutSpace_ : out_.back() == ' ')) continue;
    out_.push_back(c);
  }
  if (!s.empty()) lastOutSpace_ = s.back() == ' ';
}

void Decoder::commitBest() {
  if (beam_.empty()) return;
  auto it = std::max_element(beam_.begin(), beam_.end(),
                             [](const Hyp& a, const Hyp& b) { return a.score < b.score; });
  Hyp best = std::move(*it);
  best.score = 0;
  beam_.assign(1, std::move(best));
  commitCommonPrefix();
}

}  // namespace cw
