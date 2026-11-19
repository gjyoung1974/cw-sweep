#pragma once
// Bayesian Morse decoder operating on mark/space durations.
//
// Timing model: a dit of `dit` frames plus a keying-weight bias `bias` that
// lengthens marks and shortens gaps equally (this absorbs both the sender's
// weighting and the smear of the STFT window). It is re-estimated by robust
// grid search over the recent runs, with hysteresis so it only changes when
// the data clearly favour a new speed.
//
// Decoding: beam search over hypotheses (dit|dah for every mark,
// element|character|word gap for every space). Each hypothesis is scored by
// the duration likelihoods under the timing model, constrained to valid
// Morse prefixes, plus a weighted character-bigram language-model prior.

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "lm.hpp"
#include "morse.hpp"

namespace cw {

struct Timing {
  double dit = 10.0;   // frames
  double bias = 0.0;   // frames added to marks, removed from gaps
  double sigma = 0.22; // relative std-dev of element durations (sender's fist)
  // Letter and word gaps in dits. Standard Morse is 3 and 7, but many
  // operators (and all Farnsworth-style sending) leave much longer gaps, so
  // both are fitted per station; they never go below the standard values.
  double charUnits = 3.0;
  double wordUnits = 7.0;
  double charSigma = 0.22;  // relative std-dev of the letter gap
  double markMean(int dah) const { return (dah ? 3.0 : 1.0) * dit + bias; }
  double gapUnits(int cls) const { return cls == 0 ? 1.0 : cls == 1 ? charUnits : wordUnits; }
  double gapMean(int cls) const { return std::max(0.5, gapUnits(cls) * dit - bias); }
  // Relative std-dev of a gap of class `cls` (0 element, 1 letter).
  double gapSigma(int cls) const { return cls == 1 ? std::max(sigma, charSigma) : sigma; }
};

class TimingEstimator {
 public:
  void add(bool mark, double len);
  size_t size() const { return hist_.size(); }
  // Fit timing to history. If `force` is false the current timing is kept
  // unless a new fit is clearly better. Returns true if timing changed.
  bool fit(Timing& t, bool force);
  void clear() { hist_.clear(); }
  // Gaps in the history that are clearly longer than an element gap.
  int longGaps(const Timing& t) const;

 private:
  double cost(double dit, double bias) const;
  double spread(const Timing& t) const;
  bool fitSpacing(Timing& t) const;
  struct Obs { bool mark; double len; };
  std::deque<Obs> hist_;
  std::deque<double> gaps_;  // longer memory of gap lengths, for the spacing fit
  double gapsDit_ = 0;       // dit length when gaps_ was started
};

struct DecoderParams {
  int beamWidth = 16;
  double lmWeight = 0.6;
  double sigmaAbs = 0.8;      // absolute duration std-dev (frames); relative part is in Timing
  size_t lockRuns = 10;       // runs observed before decoding starts
};

class Decoder {
 public:
  explicit Decoder(const CharLM& lm = CharLM::instance(), DecoderParams p = {});

  void mark(double len);
  // final=true: the space is complete (a mark follows).
  // final=false: the space is still in progress and has lasted `len` frames.
  void space(double len, bool final);
  void end();

  // Return and clear newly decoded text.
  std::string take();
  const Timing& timing() const { return timing_; }
  bool locked() const { return locked_; }
  // Locked, and enough letter/word gaps seen to know the station's spacing
  // (or the transmission has paused, so there is nothing more to wait for).
  bool spacingReady() const;
  // Smallest run considered a real element rather than a glitch.
  double minRun() const;

 private:
  struct Hyp {
    double score;
    std::string text;
    Pattern pat;
    char prev;
  };
  struct Event { bool mark; double len; };

  void lockAndReplay();
  void doMark(double len);
  void doFinalSpace(double len);
  void doProgress(double len);
  void emitChar(Hyp& h, bool word) const;
  void prune();
  void commitCommonPrefix();
  void commitBest();
  double logNorm(double x, double mu, double sig) const;
  double markLL(double len, int dah) const;
  double gapLL(double len, int cls) const;
  double charThreshold() const;
  double wordThreshold() const;

  const CharLM* lm_;
  DecoderParams p_;
  Timing timing_;
  TimingEstimator est_;
  bool locked_ = false;
  bool seenMark_ = false;
  bool silent_ = false;  // in a long pause
  bool spacingFitted_ = false;
  int sinceFit_ = 0;
  int spaceState_ = 0;  // 0: nothing emitted for current gap, 1: char, 2: char+word
  std::vector<Event> pending_;
  std::vector<Hyp> beam_;
  std::string out_;
  bool lastOutSpace_ = true;
};

}  // namespace cw
