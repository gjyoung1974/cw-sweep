#pragma once
// Callsign extraction from decoded text.
//
// A word is spotted when it is a syntactically valid callsign AND either the
// identical call has been copied twice on the same channel, or it follows
// CQ / DE / TEST / QRZ and appears in a known-calls database (MASTER.SCP).
// A single copy is never trusted on its own: at low SNR a decoder produces
// plausible-looking "busted" calls, which is what pollutes spotting networks.

#include <deque>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace cw {

bool isCallsign(std::string_view word);

// Known-calls database, e.g. MASTER.SCP (one call per line, '#' comments).
class KnownCalls {
 public:
  bool load(const std::string& path, std::string& err);
  bool contains(const std::string& call) const;
  size_t size() const { return calls_.size(); }

 private:
  std::unordered_set<std::string> calls_;
};

struct CallEvent {
  std::string call;
  bool cq;  // station is calling CQ / running
};

class CallTracker {
 public:
  explicit CallTracker(const KnownCalls* known = nullptr) : known_(known) {}
  // Feed decoded characters. `now` is stream time in seconds.
  void feed(std::string_view text, double now, std::vector<CallEvent>& out);
  void flush(double now, std::vector<CallEvent>& out);

 private:
  void word(std::string w, double now, std::vector<CallEvent>& out);
  const KnownCalls* known_;
  std::string cur_;
  std::deque<std::string> prev_;
  struct Seen { std::string call; double t; bool cq; };
  std::deque<Seen> seen_;
};

}  // namespace cw
