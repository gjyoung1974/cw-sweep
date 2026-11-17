#include "callsign.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace cw {
namespace {

bool isAlpha(char c) { return c >= 'A' && c <= 'Z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// ITU-style base callsign: prefix (1-2 letters, letter+digit or digit+letter),
// 1-2 digit area number, 1-4 letter suffix.
bool isBaseCall(std::string_view s) {
  if (s.size() < 3 || s.size() > 8) return false;
  for (size_t plen = 1; plen <= 2 && plen < s.size(); ++plen) {
    std::string_view p = s.substr(0, plen);
    bool okPrefix = plen == 1 ? isAlpha(p[0])
                              : (isAlpha(p[0]) && isAlpha(p[1])) ||
                                    (isAlpha(p[0]) && isDigit(p[1])) ||
                                    (isDigit(p[0]) && isAlpha(p[1]));
    if (!okPrefix) continue;
    size_t i = plen;
    size_t nd = 0;
    while (i < s.size() && isDigit(s[i]) && nd < 2) { ++i; ++nd; }
    if (nd == 0) continue;
    size_t ns = 0;
    while (i < s.size() && isAlpha(s[i])) { ++i; ++ns; }
    if (i == s.size() && ns >= 1 && ns <= 4) return true;
  }
  return false;
}

bool isWordIn(const std::string& w, std::initializer_list<const char*> list) {
  for (const char* x : list) if (w == x) return true;
  return false;
}

}  // namespace

bool isCallsign(std::string_view word) {
  if (word.size() < 3 || word.size() > 14) return false;
  // Portable designators: DL/K1ABC, K1ABC/P, K1ABC/MM, KH6/K1ABC
  int bases = 0;
  size_t start = 0;
  while (start <= word.size()) {
    size_t slash = word.find('/', start);
    std::string_view part = word.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                               : slash - start);
    if (part.empty() || part.size() > 8) return false;
    for (char c : part) if (!isAlpha(c) && !isDigit(c)) return false;
    if (isBaseCall(part)) ++bases;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return bases >= 1;
}

bool KnownCalls::load(const std::string& path, std::string& err) {
  std::ifstream f(path);
  if (!f) { err = "cannot read " + path; return false; }
  std::string line;
  while (std::getline(f, line)) {
    std::string c;
    for (char ch : line) {
      if (std::isspace(static_cast<unsigned char>(ch))) { if (!c.empty()) break; continue; }
      c.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (!c.empty() && c[0] != '#') calls_.insert(c);
  }
  return true;
}

bool KnownCalls::contains(const std::string& call) const {
  if (calls_.count(call)) return true;
  // Portable calls: accept if the longest '/'-separated part is known.
  size_t slash = call.find('/');
  if (slash == std::string::npos) return false;
  std::string best;
  size_t start = 0;
  while (start <= call.size()) {
    size_t e = call.find('/', start);
    std::string part = call.substr(start, e == std::string::npos ? std::string::npos : e - start);
    if (part.size() > best.size()) best = part;
    if (e == std::string::npos) break;
    start = e + 1;
  }
  return calls_.count(best) > 0;
}

void CallTracker::feed(std::string_view text, double now, std::vector<CallEvent>& out) {
  for (char c : text) {
    if (c == ' ') {
      if (!cur_.empty()) word(std::move(cur_), now, out);
      cur_.clear();
    } else {
      cur_.push_back(c);
      if (cur_.size() > 20) cur_.erase(0, cur_.size() - 20);
    }
  }
}

void CallTracker::flush(double now, std::vector<CallEvent>& out) {
  if (!cur_.empty()) word(std::move(cur_), now, out);
  cur_.clear();
}

void CallTracker::word(std::string w, double now, std::vector<CallEvent>& out) {
  // Strip trailing punctuation sometimes appended by the sender ("K1ABC?").
  while (!w.empty() && !isAlpha(w.back()) && !isDigit(w.back())) w.pop_back();
  if (isCallsign(w)) {
    bool cq = false, context = false;
    for (const auto& p : prev_) {
      if (isWordIn(p, {"CQ", "TEST", "QRZ", "QRZ?", "CQD"})) cq = context = true;
      if (isWordIn(p, {"DE", "TU"})) context = true;
    }
    while (!seen_.empty() && now - seen_.front().t > 180.0) seen_.pop_front();
    bool repeated = false;
    for (const Seen& s : seen_) {
      if (s.call != w) continue;
      repeated = true;
      cq = cq || s.cq;
    }
    seen_.push_back({w, now, cq});
    bool known = known_ && known_->contains(w);
    if (repeated || (context && known)) out.push_back({w, cq});
  }
  prev_.push_back(std::move(w));
  while (prev_.size() > 2) prev_.pop_front();
}

}  // namespace cw
