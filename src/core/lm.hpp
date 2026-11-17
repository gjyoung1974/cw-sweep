#pragma once
// Character bigram language model for amateur-radio CW traffic.
//
// Used as a weak prior inside the Bayesian beam search: it breaks ties between
// acoustically ambiguous hypotheses ("is that 5NN or 5NA?") without being
// strong enough to hallucinate text that was not sent.

#include <array>
#include <string_view>

namespace cw {

class CharLM {
 public:
  static constexpr int kSymbols = 38;  // space, A-Z, 0-9, other

  CharLM();
  // Train on additional text (appended to the built-in corpus counts).
  void train(std::string_view text);
  // log P(c | prev). prev == ' ' at word starts.
  float logp(char prev, char c) const { return table_[index(prev)][index(c)]; }

  static int index(char c);
  static const CharLM& instance();

 private:
  void rebuild();
  std::array<std::array<double, kSymbols>, kSymbols> counts_{};
  std::array<std::array<float, kSymbols>, kSymbols> table_{};
};

}  // namespace cw
