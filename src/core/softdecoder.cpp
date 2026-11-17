#include "softdecoder.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace cw {
namespace {
constexpr double kLmBaseline = -3.0;
constexpr double kInvalidPenalty = 10.0;
constexpr double kPrune = 25.0;
const double kLogElem = std::log(0.6), kLogChar = std::log(0.3), kLogWord = std::log(0.1);
constexpr size_t kMaxPending = 6;
constexpr size_t kKeep = 3;
constexpr float kCommitMargin = 10.0f;

inline double sq(double x) { return x * x; }
inline double logNorm(double x, double mu, double sig) { return -0.5 * sq((x - mu) / sig) - std::log(sig); }

uint64_t hypKey(uint8_t state, uint8_t emitted, bool started, uint16_t dur, Pattern pat, const std::string& text) {
  uint64_t h = std::hash<std::string>{}(text);
  h ^= (static_cast<uint64_t>(state) << 1) ^ (static_cast<uint64_t>(emitted) << 3) ^
       (static_cast<uint64_t>(started) << 5) ^ (static_cast<uint64_t>(dur) << 8) ^
       (static_cast<uint64_t>(pat) << 24);
  return h * 0x9E3779B97F4A7C15ull;
}
}  // namespace

SoftDecoder::SoftDecoder(const CharLM& lm, double lmWeight, int beam, double startPenalty)
    : lm_(&lm), lmWeight_(lmWeight), beamWidth_(beam), startPenalty_(startPenalty) {
  beam_.push_back({0.0f, 0, 0, false, 1, kEmptyPattern, ' ', {}});
  setTiming(timing_);
}

void SoftDecoder::setTiming(const Timing& t) {
  timing_ = t;
  wordForce_ = timing_.gapMean(2);
}

double SoftDecoder::markLL(double len, int dah) const {
  double mu = timing_.markMean(dah);
  return logNorm(len, mu, timing_.sigma * mu + 0.8) + std::log(0.5);
}

double SoftDecoder::gapLL(double len, int cls) const {
  double mu = timing_.gapMean(cls);
  if (cls == 2) {
    double s = 0.35 * mu + 0.8;
    return len >= mu ? -std::log(s) : logNorm(len, mu, s);
  }
  return logNorm(len, mu, timing_.sigma * mu + 0.8);
}

void SoftDecoder::emitChar(Hyp& h, bool word) const {
  if (h.pat != kEmptyPattern) {
    char c = h.pat == kInvalidPattern ? 0 : decodePattern(h.pat);
    if (c == 0) {
      c = '*';
      h.score -= static_cast<float>(kInvalidPenalty);
    } else {
      h.score += static_cast<float>(lmWeight_ * (lm_->logp(h.prev, c) - kLmBaseline));
    }
    h.text.push_back(c);
    h.prev = c;
    h.pat = kEmptyPattern;
  }
  if (word && h.prev != ' ') {
    h.score += static_cast<float>(lmWeight_ * (lm_->logp(h.prev, ' ') - kLmBaseline));
    h.text.push_back(' ');
    h.prev = ' ';
  }
}

void SoftDecoder::push(double llr) {
  next_.clear();
  const float fl = static_cast<float>(llr);
  for (const Hyp& h : beam_) {
    // 1. Stay in the current state.
    Hyp c = h;
    if (c.dur < 65535) ++c.dur;
    if (c.state == 1) c.score += fl;
    if (c.state == 0 && c.started && c.emitted == 0 && c.dur >= wordForce_) {
      emitChar(c, true);  // long silence: the character and word are complete
      c.emitted = 2;
    }
    next_.push_back(std::move(c));

    // 2. End the current run here and switch state.
    const double d = h.dur;
    if (h.state == 1) {
      double ll[2] = {markLL(d, 0), markLL(d, 1)};
      bool any = false;
      if (h.pat != kInvalidPattern) {
        for (int dah = 0; dah < 2; ++dah) {
          Pattern np = extendPattern(h.pat, dah);
          if (!isPrefix(np)) continue;
          Hyp m = h;
          m.state = 0;
          m.dur = 1;
          m.pat = np;
          m.emitted = 0;
          m.score += static_cast<float>(ll[dah]);
          next_.push_back(std::move(m));
          any = true;
        }
      }
      if (!any) {
        Hyp m = h;
        m.state = 0;
        m.dur = 1;
        m.pat = kInvalidPattern;
        m.emitted = 0;
        m.score += static_cast<float>(std::max(ll[0], ll[1]) - (h.pat == kInvalidPattern ? 0 : kInvalidPenalty));
        next_.push_back(std::move(m));
      }
    } else {
      auto startMark = [&](Hyp m, double add) {
        m.state = 1;
        m.dur = 1;
        m.started = true;
        m.emitted = 0;
        m.score += static_cast<float>(add + llr);
        next_.push_back(std::move(m));
      };
      // Isolated blips after a long silence are far more often noise than a
      // new transmission, which pays this cost only once.
      if (!h.started) {
        startMark(h, -startPenalty_);
      } else if (h.emitted == 2) {
        startMark(h, gapLL(d, 2) + kLogWord - (d > 1.5 * wordForce_ ? startPenalty_ : 0.0));
      } else {
        if (h.pat != kEmptyPattern) startMark(h, gapLL(d, 0) + kLogElem);
        Hyp c2 = h;
        emitChar(c2, false);
        startMark(std::move(c2), gapLL(d, 1) + kLogChar);
        Hyp w = h;
        emitChar(w, true);
        startMark(std::move(w), gapLL(d, 2) + kLogWord);
      }
    }
  }
  beam_.swap(next_);
  prune();
  // A pause in sending: everything decoded so far is final.
  const Hyp& best = beam_.front();
  commit(best.state == 0 && best.emitted == 2 && best.dur > 1.5 * wordForce_ && !best.text.empty());
}

void SoftDecoder::prune() {
  auto better = [](const Hyp& a, const Hyp& b) { return a.score > b.score; };
  // Keep a margin over the beam width for duplicates, then sort only those.
  const size_t keep = static_cast<size_t>(beamWidth_) * 2;
  if (beam_.size() > keep) {
    std::nth_element(beam_.begin(), beam_.begin() + static_cast<long>(keep), beam_.end(), better);
    beam_.resize(keep);
  }
  std::sort(beam_.begin(), beam_.end(), better);
  const float best = beam_.empty() ? 0.0f : beam_.front().score;
  std::unordered_set<uint64_t> seen;
  seen.reserve(beam_.size() * 2);
  size_t k = 0;
  for (size_t i = 0; i < beam_.size() && k < static_cast<size_t>(beamWidth_); ++i) {
    Hyp& h = beam_[i];
    if (h.score < best - kPrune) break;
    if (!seen.insert(hypKey(h.state, h.emitted, h.started, h.dur, h.pat, h.text)).second) continue;
    h.score -= best;
    if (k != i) beam_[k] = std::move(h);
    ++k;
  }
  beam_.resize(k);
}

void SoftDecoder::commit(bool force) {
  if (beam_.empty()) return;
  const std::string& ref = beam_.front().text;  // best hypothesis (beam is sorted)
  size_t n = ref.size();
  for (const Hyp& h : beam_) {
    // Hypotheses this far behind the best are effectively dead: don't let
    // them hold back committing text (they are dropped below if they disagree).
    if (!force && h.score < beam_.front().score - kCommitMargin) break;
    size_t i = 0;
    while (i < n && i < h.text.size() && h.text[i] == ref[i]) ++i;
    n = i;
  }
  // Fixed-lag decision: hypotheses that differ only in an old decision can
  // evolve in lockstep forever. Once the best one is more than kMaxPending
  // characters ahead of the agreed prefix, commit all but its last kKeep.
  if (!force && ref.size() > n + kMaxPending) n = ref.size() - kKeep;
  if (force) n = ref.size();
  if (n == 0) return;
  std::string s = ref.substr(0, n);
  std::vector<Hyp> kept;
  for (Hyp& h : beam_) {
    if (h.text.compare(0, n, s) != 0) continue;
    h.text.erase(0, n);
    kept.push_back(std::move(h));
  }
  beam_.swap(kept);
  for (char c : s) {
    if (c == ' ' && (out_.empty() ? lastOutSpace_ : out_.back() == ' ')) continue;
    out_.push_back(c);
  }
  lastOutSpace_ = s.back() == ' ';
}

void SoftDecoder::end() {
  for (Hyp& h : beam_) {
    if (h.state == 1) {
      double ll0 = markLL(h.dur, 0), ll1 = markLL(h.dur, 1);
      Pattern np = extendPattern(h.pat, ll1 > ll0 ? 1 : 0);
      h.pat = isPrefix(np) ? np : kInvalidPattern;
      h.score += static_cast<float>(std::max(ll0, ll1));
      h.state = 0;
    }
    if (h.started) emitChar(h, true);
  }
  prune();
  commit(true);
}

std::string SoftDecoder::take() {
  std::string s;
  s.swap(out_);
  return s;
}

}  // namespace cw
