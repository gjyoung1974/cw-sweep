#pragma once
// Morse code tables.
//
// A pattern is stored as an integer with a leading sentinel 1-bit:
//   empty = 0b1, each dit appends 0, each dah appends 1.
// This makes extension and lookup O(1) inside the beam search.
//
// Prosigns are mapped to single characters:
//   AR .-.-. '+'   BT -...- '='   KN -.--. '('   SK ...-.- '>'   AS .-... '&'

#include <cstdint>
#include <string>
#include <string_view>

namespace cw {

using Pattern = uint16_t;
constexpr Pattern kEmptyPattern = 1;
constexpr Pattern kInvalidPattern = 0;
constexpr int kMaxPatternLen = 7;

inline int patternLength(Pattern p) {
  int n = 0;
  while (p > 1) { p >>= 1; ++n; }
  return n;
}

inline Pattern extendPattern(Pattern p, int dah) {
  if (p == kInvalidPattern || patternLength(p) >= kMaxPatternLen) return kInvalidPattern;
  return static_cast<Pattern>((p << 1) | (dah ? 1 : 0));
}

// Character for a complete pattern, or 0 if none.
char decodePattern(Pattern p);
// True if p is a prefix of (or equal to) some valid code.
bool isPrefix(Pattern p);
// Dot/dash string for a character ("" if not encodable).
std::string_view codeFor(char c);
Pattern toPattern(std::string_view dotsDashes);

// Upper-case, drop characters that cannot be sent, collapse whitespace.
std::string normalizeText(std::string_view text);
// Replace prosign placeholder characters with <AR>, <SK>, ...
std::string renderText(std::string_view text);

}  // namespace cw
