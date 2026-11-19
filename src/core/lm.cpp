#include "lm.hpp"

#include <cmath>
#include <cstdint>
#include <string>

namespace cw {
namespace {

// Typical on-air CW traffic: ragchew, contest and DX exchanges, Q-codes and
// abbreviations. Callsign structure is taught separately below.
constexpr const char* kCorpus = R"(
CQ CQ CQ DE K
CQ CQ DE PSE K
CQ TEST CQ TEST
CQ DX CQ DX DE K
QRZ? QRZ DE
TU 5NN 5NN TU
TU UP UP TU QRZ
5NN 05 5NN 14 5NN 15 5NN 33 5NN 25 5NN 3 5NN 4 5NN 27 5NN 28
599 599 599 TU 579 559 589 449
R R TNX FER CALL UR RST 599 599 NAME BOB BOB QTH OHIO OHIO HW?
GM GA GE OM TNX FER QSO HPE CUAGN 73 73 SK
FB OM UR SIG RST 579 HR NAME IS JOHN JOHN QTH NR BOSTON
RIG HR IS FLEX 6600 ES ANT IS DIPOLE PWR 100W
WX HR IS SUNNY ES WARM TEMP 25C
TNX FER NICE QSO 73 ES GUD DX
QSL VIA BURO QSL VIA LOTW EQSL OK
AGN AGN PSE QRS QRM QRN QSB QSY QRL? QRL?
NR? NR? CALL? PSE AGN
BK BK R R
RST 579 NAME TOM QTH TEXAS
UR 599 IN NY NAME JIM
73 ES TU DE
GL ES GUD DX 73 TU
FIRST QSO WID U
CONDX POOR HR QSB DEEP
HAM SINCE 1975 ES AGE 65
MY QTH IS NEAR PARIS
POWER 5W QRP ANT VERTICAL
CPY? SRI QRM
QRU? QRV QRX QTH QSL QSO
DE TEST TEST UP 1 UP 2 LISTENING
CQ WW DX CONTEST 5NN 14 5NN 15 5NN 16 5NN 05
ARRL DX 5NN 100 5NN KW 5NN 1K
CQ POTA CQ SOTA CQ IOTA CQ FD
NAME OP OP OP
THE AND FOR YOU WITH HAVE THIS WILL YOUR FROM GOOD VERY NICE
WEATHER ANTENNA RADIO STATION SIGNAL REPORT
)";

constexpr const char* kPrefixes[] = {
    "K", "W", "N", "AA", "AB", "AC", "AD", "AE", "AF", "AG", "AI", "AJ", "AK", "KA", "KB", "KC",
    "KD", "KE", "KF", "KG", "KI", "KJ", "KK", "KN", "KO", "KQ", "WA", "WB", "WD", "N", "VE",
    "VA", "VY", "G", "M", "F", "DL", "DJ", "DK", "DM", "DO", "I", "IK", "IZ", "EA", "EB",
    "EC", "OH", "SM", "SA", "LA", "OZ", "PA", "PD", "ON", "OK", "OM", "SP", "SQ", "HA",
    "YO", "LZ", "UA", "RA", "RU", "RW", "RN", "UR", "UT", "UX", "JA", "JE", "JF", "JH",
    "JR", "VK", "ZL", "ZS", "PY", "PP", "LU", "CE", "XE", "HB", "OE", "S5", "9A", "YU",
    "EI", "GM", "GW", "GI", "4X", "5B", "TA", "ES", "YL", "LY", "BY", "BG", "HL", "DS",
    "VU", "A6", "A7", "UN", "4L", "ER", "EU", "EW", "CT", "CU", "EA8", "KP4", "KH6", "KL7",
    "VP2", "J3", "9Y", "ZF", "CO", "CM", "HK", "HC", "OA", "YV", "TI", "HP", "V5", "5Z",
};

}  // namespace

int CharLM::index(char c) {
  if (c == ' ') return 0;
  if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
  if (c >= '0' && c <= '9') return 27 + (c - '0');
  return 37;
}

CharLM::CharLM() {
  train(kCorpus);
  // Teach callsign structure with a deterministic set of synthetic calls.
  uint32_t s = 12345;
  auto rnd = [&s](uint32_t n) {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) % n;
  };
  std::string calls;
  constexpr size_t nPfx = sizeof(kPrefixes) / sizeof(kPrefixes[0]);
  for (int i = 0; i < 3000; ++i) {
    std::string c = kPrefixes[rnd(nPfx)];
    if (!(c.back() >= '0' && c.back() <= '9')) c.push_back(static_cast<char>('0' + rnd(10)));
    int n = 1 + static_cast<int>(rnd(3));
    for (int k = 0; k < n; ++k) c.push_back(static_cast<char>('A' + rnd(26)));
    calls += c;
    calls += (i % 7 == 0) ? " DE " : " ";
  }
  train(calls);
}

void CharLM::train(std::string_view text) {
  char prev = ' ';
  for (char c : text) {
    if (c == '\n' || c == '\t') c = ' ';
    if (c == ' ' && prev == ' ') continue;
    counts_[index(prev)][index(c)] += 1.0;
    prev = c;
  }
  counts_[index(prev)][0] += 1.0;
  rebuild();
}

void CharLM::rebuild() {
  // Interpolated bigram/unigram/uniform with add-k smoothing.
  std::array<double, kSymbols> uni{};
  double total = 0;
  for (int i = 0; i < kSymbols; ++i)
    for (int j = 0; j < kSymbols; ++j) { uni[j] += counts_[i][j]; total += counts_[i][j]; }
  for (int i = 0; i < kSymbols; ++i) {
    double rowTotal = 0;
    for (int j = 0; j < kSymbols; ++j) rowTotal += counts_[i][j];
    for (int j = 0; j < kSymbols; ++j) {
      double big = (counts_[i][j] + 0.1) / (rowTotal + 0.1 * kSymbols);
      double un = (uni[j] + 1.0) / (total + kSymbols);
      double p = 0.70 * big + 0.22 * un + 0.08 / kSymbols;
      table_[i][j] = static_cast<float>(std::log(p));
    }
  }
}

const CharLM& CharLM::instance() {
  static const CharLM lm;
  return lm;
}

}  // namespace cw
