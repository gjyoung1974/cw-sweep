// Minimal self-contained test runner (no external framework needed).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "callsign.hpp"
#include "decoder.hpp"
#include "engine.hpp"
#include "morse.hpp"
#include "synth.hpp"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++failures;                                                            \
    }                                                                        \
  } while (0)
#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    auto _a = (a);                                                           \
    auto _b = (b);                                                           \
    if (!(_a == _b)) {                                                       \
      std::printf("  FAIL %s:%d: %s == %s\n    got:      '%s'\n    expected: '%s'\n", \
                  __FILE__, __LINE__, #a, #b, std::string(_a).c_str(),       \
                  std::string(_b).c_str());                                  \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

std::string trim(std::string s) {
  while (!s.empty() && s.back() == ' ') s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(0, 1);
  return s;
}

void testMorse() {
  for (char c : std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/?=")) {
    auto code = cw::codeFor(c);
    CHECK(!code.empty());
    cw::Pattern p = cw::kEmptyPattern;
    for (char e : code) {
      p = cw::extendPattern(p, e == '-');
      CHECK(cw::isPrefix(p));
    }
    CHECK(cw::decodePattern(p) == c);
  }
  CHECK(!cw::isPrefix(cw::toPattern("........")));
  CHECK_EQ(cw::normalizeText("  cq  de k1abc "), std::string("CQ DE K1ABC"));
}

void testCallsigns() {
  for (const char* c : {"K1ABC", "DL1ABC", "9A1AA", "S51A", "W1AW", "KH6/K1ABC", "K1ABC/P",
                        "VK2ABC", "JA1XYZ", "4X4AA", "UA9ABC", "N0X"})
    CHECK(cw::isCallsign(c));
  for (const char* c : {"5NN", "TEST", "599", "CQ", "73", "TU", "K1", "ABCDEF", "1ABC", "K1ABCDE"})
    CHECK(!cw::isCallsign(c));

  cw::CallTracker t;
  std::vector<cw::CallEvent> ev;
  t.feed("CQ CQ DE K1ABC K1ABC K ", 0, ev);  // two copies -> spotted
  CHECK(ev.size() == 1 && ev[0].call == "K1ABC" && ev[0].cq);
  ev.clear();
  t.feed("CQ DE W2XYZ K ", 1, ev);  // single copy, unknown call -> not spotted
  CHECK(ev.empty());
  t.feed("W2XYZ 5NN ", 2, ev);  // second copy -> spotted (engine dedupes repeats)
  CHECK(ev.size() == 1 && ev[0].call == "W2XYZ" && ev[0].cq);
}

// Feed ideal durations straight into the decoder.
std::string decodeIdeal(const std::string& text, double dit, double weight) {
  cw::Decoder d;
  std::string norm = cw::normalizeText(text);
  for (size_t i = 0; i < norm.size(); ++i) {
    if (norm[i] == ' ') continue;
    auto code = cw::codeFor(norm[i]);
    for (size_t e = 0; e < code.size(); ++e) {
      d.mark((code[e] == '-' ? 3 : 1) * dit + weight);
      if (e + 1 < code.size()) d.space(dit - weight, true);
    }
    if (i + 1 < norm.size()) {
      double g = (norm[i + 1] == ' ' ? 7 : 3) * dit - weight;
      for (double l = 1; l < g; l += 1) d.space(l, false);
      d.space(g, true);
    }
  }
  d.end();
  return trim(d.take());
}

void testDecoderIdeal() {
  const std::string msg = "CQ CQ DE K1ABC K1ABC K";
  CHECK_EQ(decodeIdeal(msg, 10, 0), msg);
  CHECK_EQ(decodeIdeal(msg, 4, 0), msg);
  CHECK_EQ(decodeIdeal(msg, 30, 0), msg);
  CHECK_EQ(decodeIdeal(msg, 10, 3), msg);   // heavy keying weight
  CHECK_EQ(decodeIdeal("5NN TU EEE TTT 73", 8, 0), std::string("5NN TU EEE TTT 73"));
}

std::string decodeSynth(const std::string& msg, double snr, double wpm, std::vector<std::string>* calls) {
  cw::Station st;
  st.freq = 700;
  st.wpm = wpm;
  st.text = msg;
  st.snrDb = snr;
  st.start = 1.0;
  cw::SynthParams sp;
  sp.duration = cw::messageDuration(msg, wpm) + 3.0;
  sp.seed = 7;
  auto x = cw::synthesize({st}, sp);
  cw::EngineConfig cfg;
  cw::Engine eng(cfg);
  std::string text;
  eng.onText = [&](const cw::ChannelInfo& ci, const std::string& t) {
    if (std::abs(ci.offsetHz - 700) < 40) text += t;
  };
  eng.onSpot = [&](const cw::Spot& s) { if (calls) calls->push_back(s.call); };
  eng.process(x.data(), x.size());
  eng.finish();
  return trim(text);
}

void testEndToEnd() {
  const std::string msg = "CQ CQ DE K1ABC K1ABC K";
  std::vector<std::string> calls;
  CHECK_EQ(decodeSynth(msg, 10, 25, &calls), msg);
  CHECK(calls.size() == 1 && calls[0] == "K1ABC");
  CHECK_EQ(decodeSynth(msg, 0, 18, nullptr), msg);
  CHECK_EQ(decodeSynth(msg, 5, 40, nullptr), msg);
}

// Decode a multi-station band and return text per station (nearest channel).
std::vector<std::string> decodeBand(std::vector<cw::Station> st, const cw::SynthParams& sp,
                                    std::vector<std::string>* spots = nullptr) {
  cw::SynthParams p = sp;
  for (const auto& s : st)  // make sure every transmission completes
    p.duration = std::max(p.duration, s.start + cw::messageDuration(s.text, s.wpm) + 2.0);
  auto x = cw::synthesize(st, p);
  cw::Engine eng(cw::EngineConfig{});
  std::vector<std::pair<int, std::string>> byId;
  std::vector<std::pair<int, double>> freqs;
  eng.onText = [&](const cw::ChannelInfo& ci, const std::string& t) {
    for (auto& [id, txt] : byId) if (id == ci.id) { txt += t; return; }
    byId.push_back({ci.id, t});
    freqs.push_back({ci.id, ci.offsetHz});
  };
  eng.onSpot = [&](const cw::Spot& s) { if (spots) spots->push_back(s.call); };
  eng.process(x.data(), x.size());
  eng.finish();
  std::vector<std::string> out;
  for (const auto& s : st) {
    std::string best;
    for (size_t i = 0; i < freqs.size(); ++i)
      if (std::abs(freqs[i].second - s.freq) < 40) best = trim(byId[i].second);
    out.push_back(best);
  }
  return out;
}

cw::Station station(double f, double wpm, double snr, const std::string& text, double start = 1.0) {
  cw::Station s;
  s.freq = f;
  s.wpm = wpm;
  s.snrDb = snr;
  s.text = text;
  s.start = start;
  return s;
}

void testCrowdedBand() {
  // A weak station between two much stronger ones 200 Hz away.
  std::vector<cw::Station> st = {
      station(800, 28, 25, "CQ TEST DL1AA DL1AA", 0.7),
      station(1000, 24, 5, "CQ CQ DE K1ABC K1ABC K", 1.0),
      station(1200, 32, 25, "CQ TEST OH2BB OH2BB", 1.3),
  };
  cw::SynthParams sp;
  sp.duration = 12;
  sp.seed = 3;
  auto out = decodeBand(st, sp);
  CHECK_EQ(out[0], std::string("CQ TEST DL1AA DL1AA"));
  CHECK_EQ(out[1], std::string("CQ CQ DE K1ABC K1ABC K"));
  CHECK_EQ(out[2], std::string("CQ TEST OH2BB OH2BB"));
}

void testImpulseNoise() {
  std::vector<cw::Station> st = {station(700, 25, 0, "CQ CQ DE K1ABC K1ABC K")};
  cw::SynthParams sp;
  sp.duration = 12;
  sp.seed = 4;
  sp.qrnRate = 20;
  CHECK_EQ(decodeBand(st, sp)[0], std::string("CQ CQ DE K1ABC K1ABC K"));
}

void testFading() {
  auto s = station(700, 22, 10, "CQ CQ DE K1ABC K1ABC K");
  s.qsbRate = 0.3;
  s.qsbDepth = 0.8;
  cw::SynthParams sp;
  sp.duration = 13;
  sp.seed = 5;
  CHECK_EQ(decodeBand({s}, sp)[0], std::string("CQ CQ DE K1ABC K1ABC K"));
}

void testNoiseOnlyNoSpots() {
  cw::SynthParams sp;
  sp.duration = 60;
  sp.seed = 6;
  std::vector<std::string> spots;
  decodeBand({station(1000, 20, -40, "E")}, sp, &spots);
  CHECK(spots.empty());
}

}  // namespace

int main() {
  struct T { const char* name; std::function<void()> fn; };
  std::vector<T> tests = {
      {"morse", testMorse},
      {"callsigns", testCallsigns},
      {"decoder_ideal", testDecoderIdeal},
      {"end_to_end", testEndToEnd},
      {"crowded_band", testCrowdedBand},
      {"impulse_noise", testImpulseNoise},
      {"fading", testFading},
      {"noise_no_spots", testNoiseOnlyNoSpots},
  };
  for (auto& t : tests) {
    int before = failures;
    t.fn();
    std::printf("%-16s %s\n", t.name, failures == before ? "ok" : "FAILED");
  }
  return failures == 0 ? 0 : 1;
}
