#pragma once
// Soft-decision Morse decoder: joint segmentation and decoding.
//
// Instead of first deciding key-up/key-down per frame and then classifying
// run lengths (which cannot recover once noise has split a dah or merged two
// dits), this decoder runs a beam search directly on the per-frame
// log-likelihood ratios. Each hypothesis carries:
//
//   - the key state (up/down) and how long it has been in that state,
//   - the Morse pattern of the character in progress,
//   - the decoded text not yet committed.
//
// It is an explicit-duration (semi-Markov) HMM: when a run ends, its length is
// scored against the dit/dah or element/character/word-gap duration model of
// the current timing estimate, plus the character language model. Frames in
// the key-down state add their LLR. Text is committed once all surviving
// hypotheses agree on it.

#include <string>
#include <vector>

#include "decoder.hpp"
#include "lm.hpp"
#include "morse.hpp"

namespace cw {

class SoftDecoder {
 public:
  explicit SoftDecoder(const CharLM& lm = CharLM::instance(), double lmWeight = 0.6, int beam = 64,
                       double startPenalty = 8.0);

  void setTiming(const Timing& t);
  void push(double llr);
  void end();
  std::string take();

 private:
  struct Hyp {
    float score;
    uint8_t state;     // 0 key up, 1 key down
    uint8_t emitted;   // in a gap: 0 nothing yet, 2 char+word already emitted
    bool started;      // seen at least one mark
    uint16_t dur;      // frames in current state
    Pattern pat;
    char prev;
    std::string text;
  };

  void emitChar(Hyp& h, bool word) const;
  double markLL(double len, int dah) const;
  double gapLL(double len, int cls) const;
  void prune();
  void commit(bool force);

  const CharLM* lm_;
  double lmWeight_;
  int beamWidth_;
  double startPenalty_;  // prior cost of keying starting after a long silence
  Timing timing_;
  double wordForce_ = 70;
  std::vector<Hyp> beam_, next_;
  std::string out_;
  bool lastOutSpace_ = true;
};

}  // namespace cw
