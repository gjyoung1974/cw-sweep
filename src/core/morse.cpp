#include "morse.hpp"

#include <array>
#include <cctype>

namespace cw {
namespace {

struct Entry { char c; const char* code; };

constexpr Entry kTable[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},
    {'F', "..-."},   {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},
    {'K', "-.-"},    {'L', ".-.."},   {'M', "--"},     {'N', "-."},     {'O', "---"},
    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},   {'Y', "-.--"},
    {'Z', "--.."},
    {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},  {'4', "....-"},
    {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},  {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},  {'=', "-...-"},
    {'+', ".-.-."},  {'(', "-.--."},  {')', "-.--.-"}, {'\'', ".----."}, {'-', "-....-"},
    {':', "---..."}, {'@', ".--.-."}, {'>', "...-.-"}, {'&', ".-..."},  {'"', ".-..-."},
    {'!', "-.-.--"},
};

struct Tables {
  std::array<char, 256> decode{};
  std::array<bool, 256> prefix{};
  std::array<std::string_view, 128> code{};
  Tables() {
    for (const auto& e : kTable) {
      Pattern p = toPattern(e.code);
      decode[p] = e.c;
      code[static_cast<unsigned char>(e.c)] = e.code;
      for (Pattern q = p; q >= 1; q >>= 1) prefix[q] = true;
    }
  }
};

const Tables& tables() {
  static const Tables t;
  return t;
}

}  // namespace

Pattern toPattern(std::string_view s) {
  Pattern p = kEmptyPattern;
  for (char ch : s) p = static_cast<Pattern>((p << 1) | (ch == '-' ? 1 : 0));
  return p;
}

char decodePattern(Pattern p) { return p < 256 ? tables().decode[p] : 0; }

bool isPrefix(Pattern p) { return p != kInvalidPattern && p < 256 && tables().prefix[p]; }

std::string_view codeFor(char c) {
  auto u = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)));
  return u < 128 ? tables().code[u] : std::string_view{};
}

std::string normalizeText(std::string_view text) {
  std::string out;
  for (char c : text) {
    char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (std::isspace(static_cast<unsigned char>(u))) {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
    } else if (!codeFor(u).empty()) {
      out.push_back(u);
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string renderText(std::string_view text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '+': out += "<AR>"; break;
      case '>': out += "<SK>"; break;
      case '&': out += "<AS>"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

}  // namespace cw
