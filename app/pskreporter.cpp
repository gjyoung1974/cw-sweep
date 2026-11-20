#include "pskreporter.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace cwapp {

namespace {

const size_t kMaxPerDatagram = 50;   // about 1300 bytes: under any path MTU
const size_t kMaxPending = 2000;
const std::time_t kDedupSec = 300;   // one report per call and band in this time
const std::time_t kStaleSec = 1800;  // unsent reports older than this are dropped
const std::time_t kTemplateSec = 3000;  // descriptors are re-sent this often

// Record descriptors. 0x9992 and 0x9993 link them to the records; fields
// with the top bit set belong to PSK Reporter's enterprise number, 30351
// (0x768F); a length of 0xFFFF is a length-prefixed string.
const uint8_t kReceiverTemplate[] = {
    0x00, 0x03, 0x00, 0x24, 0x99, 0x92, 0x00, 0x03, 0x00, 0x01,
    0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // receiverCallsign
    0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // receiverLocator
    0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // decodingSoftware
    0x00, 0x00};
const uint8_t kReceiverTemplateAntenna[] = {
    0x00, 0x03, 0x00, 0x2C, 0x99, 0x92, 0x00, 0x04, 0x00, 0x01,
    0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // receiverCallsign
    0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // receiverLocator
    0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // decodingSoftware
    0x80, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // antennaInformation
    0x00, 0x00};
const uint8_t kSenderTemplate[] = {
    0x00, 0x02, 0x00, 0x34, 0x99, 0x93, 0x00, 0x06,
    0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // senderCallsign
    0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8F,  // frequency, Hz
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,  // sNR, dB
    0x80, 0x0A, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,  // mode
    0x80, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,  // informationSource
    0x00, 0x96, 0x00, 0x04};                         // flowStartSeconds

using Bytes = std::vector<uint8_t>;

void put16(Bytes& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}
void put32(Bytes& b, uint32_t v) {
  put16(b, v >> 16);
  put16(b, v & 0xFFFF);
}
void putString(Bytes& b, const std::string& s) {
  size_t n = std::min<size_t>(s.size(), 254);
  b.push_back(static_cast<uint8_t>(n));
  b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}
// Pads the set that starts at `start` to a multiple of 4 and fills in its length.
void closeSet(Bytes& b, size_t start) {
  while ((b.size() - start) % 4) b.push_back(0);
  size_t len = b.size() - start;
  b[start + 2] = static_cast<uint8_t>(len >> 8);
  b[start + 3] = static_cast<uint8_t>(len);
}

void logMsg(const std::string& s) { std::fprintf(stderr, "cwsweep: pskreporter: %s\n", s.c_str()); }

}  // namespace

bool normaliseLocator(std::string& loc) {
  if (loc.size() != 4 && loc.size() != 6) return false;
  for (size_t i = 0; i < loc.size(); ++i) {
    auto c = static_cast<unsigned char>(loc[i]);
    if (i < 2) {
      c = static_cast<unsigned char>(std::toupper(c));
      if (c < 'A' || c > 'R') return false;
    } else if (i < 4) {
      if (!std::isdigit(c)) return false;
    } else {
      c = static_cast<unsigned char>(std::tolower(c));
      if (c < 'a' || c > 'x') return false;
    }
    loc[i] = static_cast<char>(c);
  }
  return true;
}

void parseHostPort(const std::string& s, std::string& host, int& port) {
  size_t colon = s.rfind(':');
  if (colon == std::string::npos) { host = s; return; }
  host = s.substr(0, colon);
  port = std::stoi(s.substr(colon + 1));
}

std::vector<uint8_t> buildPskDatagram(const PskReporterOptions& o, const std::vector<PskReport>& reports,
                                      bool templates, uint32_t sequence, uint32_t sessionId,
                                      std::time_t now) {
  Bytes b;
  put16(b, 0x000A);  // IPFIX version
  put16(b, 0);       // total length, filled in below
  put32(b, static_cast<uint32_t>(now));
  put32(b, sequence);
  put32(b, sessionId);

  const bool antenna = !o.antenna.empty();
  if (templates) {
    if (antenna) b.insert(b.end(), std::begin(kReceiverTemplateAntenna), std::end(kReceiverTemplateAntenna));
    else b.insert(b.end(), std::begin(kReceiverTemplate), std::end(kReceiverTemplate));
    b.insert(b.end(), std::begin(kSenderTemplate), std::end(kSenderTemplate));
  }

  size_t start = b.size();
  put16(b, 0x9992);
  put16(b, 0);
  putString(b, o.call);
  putString(b, o.locator);
  putString(b, o.software);
  if (antenna) putString(b, o.antenna);
  closeSet(b, start);

  if (!reports.empty()) {
    start = b.size();
    put16(b, 0x9993);
    put16(b, 0);
    for (const PskReport& r : reports) {
      putString(b, r.call);
      put32(b, r.freqHz);
      b.push_back(static_cast<uint8_t>(r.snrDb));
      putString(b, "CW");
      b.push_back(1);  // informationSource: decoded automatically
      put32(b, static_cast<uint32_t>(r.time));
    }
    closeSet(b, start);
  }

  b[2] = static_cast<uint8_t>(b.size() >> 8);
  b[3] = static_cast<uint8_t>(b.size());
  return b;
}

PskReporter::PskReporter(PskReporterOptions o) : o_(std::move(o)) {
  std::random_device rd;
  sessionId_ = (static_cast<uint32_t>(rd()) << 16) ^ static_cast<uint32_t>(rd());
}

PskReporter::~PskReporter() { stop(); }

void PskReporter::start() {
  std::lock_guard lk(mu_);
  if (running_) return;
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void PskReporter::stop() {
  {
    std::lock_guard lk(mu_);
    if (!running_) return;
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  flush();
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

void PskReporter::add(const std::string& call, double rfHz, double snrDb) {
  if (call.empty() || rfHz < 1 || rfHz > 4.0e9) return;
  std::time_t now = std::time(nullptr);
  std::string key = call + "/" + std::to_string(static_cast<long>(rfHz / 1e6));
  bool full = false;
  {
    std::lock_guard lk(mu_);
    auto it = lastReported_.find(key);
    if (it != lastReported_.end() && now - it->second < kDedupSec) return;
    if (lastReported_.size() > 5000)
      std::erase_if(lastReported_, [&](const auto& kv) { return now - kv.second >= kDedupSec; });
    lastReported_[key] = now;
    if (pending_.size() >= kMaxPending) pending_.erase(pending_.begin());
    PskReport r;
    r.call = call;
    r.freqHz = static_cast<uint32_t>(std::llround(rfHz));
    r.snrDb = static_cast<int8_t>(std::clamp(std::lround(snrDb), -128L, 127L));
    r.time = now;
    pending_.push_back(std::move(r));
    full = pending_.size() >= kMaxPerDatagram && !failing_;
  }
  if (full) cv_.notify_all();
}

void PskReporter::run() {
  // The timer runs from program start, so that uploads aren't synchronised
  // to the clock across stations. A full datagram is sent at once.
  using Clock = std::chrono::steady_clock;
  const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o_.intervalSec));
  auto next = Clock::now() + interval;
  std::unique_lock lk(mu_);
  while (running_) {
    cv_.wait_until(lk, next, [&] { return !running_ || (pending_.size() >= kMaxPerDatagram && !failing_); });
    if (!running_) break;
    lk.unlock();
    flush();
    lk.lock();
    while (next <= Clock::now()) next += interval;
  }
}

bool PskReporter::openSocket(sockaddr_storage& to, unsigned& toLen) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = fd_ >= 0 ? family_ : AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  int rc = ::getaddrinfo(o_.host.c_str(), std::to_string(o_.port).c_str(), &hints, &res);
  if (rc != 0 || !res) {
    if (!failing_) logMsg(o_.host + ": " + gai_strerror(rc) + "; will retry");
    return false;
  }
  bool ok = false;
  for (addrinfo* a = res; a && !ok; a = a->ai_next) {
    // One socket for the whole session: the server recognises a station's
    // descriptors by its source address and port.
    if (fd_ < 0) {
      fd_ = ::socket(a->ai_family, SOCK_DGRAM, 0);
      if (fd_ < 0) continue;
      family_ = a->ai_family;
    }
    std::memcpy(&to, a->ai_addr, a->ai_addrlen);
    toLen = static_cast<unsigned>(a->ai_addrlen);
    ok = true;
  }
  ::freeaddrinfo(res);
  if (!ok && !failing_) logMsg(std::string("socket: ") + std::strerror(errno));
  return ok;
}

void PskReporter::flush() {
  std::vector<PskReport> batch;
  {
    std::lock_guard lk(mu_);
    batch.swap(pending_);
  }
  std::time_t now = std::time(nullptr);
  std::erase_if(batch, [&](const PskReport& r) { return now - r.time > kStaleSec; });
  if (batch.empty()) return;

  sockaddr_storage to{};
  unsigned toLen = 0;
  size_t sent = 0;
  bool ok = openSocket(to, toLen);
  while (ok && sent < batch.size()) {
    size_t n = std::min(kMaxPerDatagram, batch.size() - sent);
    std::vector<PskReport> part(batch.begin() + static_cast<std::ptrdiff_t>(sent),
                                batch.begin() + static_cast<std::ptrdiff_t>(sent + n));
    bool templates = datagramsSent_ < 3 || now - lastTemplates_ >= kTemplateSec;
    std::vector<uint8_t> d = buildPskDatagram(o_, part, templates, sequence_, sessionId_, now);
    if (::sendto(fd_, d.data(), d.size(), 0, reinterpret_cast<sockaddr*>(&to), toLen) < 0) {
      if (!failing_) logMsg(std::string("send: ") + std::strerror(errno) + "; will retry");
      ok = false;
      break;
    }
    sequence_ += static_cast<uint32_t>(n) + 1;  // the reports and the receiver record
    ++datagramsSent_;
    if (templates) lastTemplates_ = now;
    sent += n;
  }

  if (ok && !announced_) {
    announced_ = true;
    logMsg("reporting to " + o_.host + ":" + std::to_string(o_.port) + " as " + o_.call + " in " + o_.locator);
  }
  std::lock_guard lk(mu_);
  if (ok && failing_) logMsg("sending again");
  failing_ = !ok;
  // What couldn't be sent goes back to the front of the queue.
  if (sent < batch.size()) pending_.insert(pending_.begin(), batch.begin() + static_cast<std::ptrdiff_t>(sent), batch.end());
}

}  // namespace cwapp
