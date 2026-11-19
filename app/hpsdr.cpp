#include "hpsdr.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// openHPSDR protocol 1 ("Metis"), UDP port 1024.
//
//   discovery   host -> EF FE 02, 60 zero bytes            (63 bytes)
//               radio -> EF FE <02 idle | 03 streaming> MAC[6] version board ...
//   start/stop  host -> EF FE 04 <01 | 00>, 60 zero bytes  (64 bytes)
//   data        1032 bytes each way: EF FE 01 <endpoint> sequence[4], then two
//               512-byte frames of 7F 7F 7F C0..C4 and 504 payload bytes.
//     radio -> host (endpoint 6): per sample, 24-bit big-endian I and Q for
//               each receiver, then a 16-bit microphone sample.
//     host -> radio (endpoint 2): C0 = address << 1 | MOX, C1..C4 = value;
//               the payload is transmit audio and I/Q.
//
// This client only receives. Every command byte it sends has MOX clear, the
// drive level is zero, the power amplifier is off and the payload is zero, so
// it cannot key the transmitter.

namespace cwapp {
namespace {

constexpr int kPort = 1024;
constexpr size_t kPacket = 1032;
constexpr int kSetsPerFrame = 63;  // one receiver: 504 / (6 + 2)

struct Radio {
  sockaddr_in addr{};
  std::array<uint8_t, 6> mac{};
  int version = 0, minor = 0, board = -1, receivers = 0;
  bool busy = false;
};

const char* boardName(int id) {
  switch (id) {
    case 0: return "Metis";
    case 1: return "Hermes";
    case 2: return "Griffin";
    case 4: return "Angelia";
    case 5: return "Orion";
    case 6: return "Hermes Lite";
    case 10: return "Orion 2";
    default: return "openHPSDR radio";
  }
}

std::string describeRadio(const Radio& r) {
  char ip[INET_ADDRSTRLEN] = "";
  inet_ntop(AF_INET, &r.addr.sin_addr, ip, sizeof ip);
  char b[160];
  std::snprintf(b, sizeof b, "%s at %s (%02x:%02x:%02x:%02x:%02x:%02x, gateware %d.%d)", boardName(r.board), ip,
                r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5], r.version, r.minor);
  return b;
}

// Sends a discovery request to `to` (or to every broadcast address if null)
// and collects the answers that arrive within `waitMs`.
std::vector<Radio> discover(const sockaddr_in* to, int waitMs) {
  std::vector<Radio> found;
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return found;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  uint8_t req[63] = {0xEF, 0xFE, 0x02};

  std::vector<sockaddr_in> targets;
  if (to) {
    targets.push_back(*to);
  } else {
    sockaddr_in all{};
    all.sin_family = AF_INET;
    all.sin_port = htons(kPort);
    all.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    targets.push_back(all);
    // The limited broadcast only leaves by the default route; also try the
    // directed broadcast of every interface (a radio on a second network card).
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
      for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) continue;
        sockaddr_in b = *reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr);
        b.sin_port = htons(kPort);
        targets.push_back(b);
      }
      freeifaddrs(ifs);
    }
  }
  for (const sockaddr_in& t : targets)
    ::sendto(fd, req, sizeof req, 0, reinterpret_cast<const sockaddr*>(&t), sizeof t);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
  for (;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) break;
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
    uint8_t buf[kPacket];
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 11 || buf[0] != 0xEF || buf[1] != 0xFE || (buf[2] != 0x02 && buf[2] != 0x03)) continue;
    Radio r;
    r.addr = from;
    std::copy(buf + 3, buf + 9, r.mac.begin());
    r.version = buf[9];
    r.board = buf[10];
    r.busy = buf[2] == 0x03;
    if (n > 21) {
      r.receivers = buf[19];
      r.minor = buf[21];
    }
    bool dup = false;
    for (const Radio& o : found) dup = dup || o.addr.sin_addr.s_addr == r.addr.sin_addr.s_addr;
    if (!dup) found.push_back(r);
    if (to) break;  // asked one radio; it answered
  }
  ::close(fd);
  return found;
}

class HpsdrSource : public Source {
 public:
  HpsdrSource(const SourceOptions& o, const std::string& target, std::string& err) : o_(o) {
    if (o_.rate <= 0) o_.rate = 192000;
    if (o_.rate == 48000) rateCode_ = 0;
    else if (o_.rate == 96000) rateCode_ = 1;
    else if (o_.rate == 192000) rateCode_ = 2;
    else if (o_.rate == 384000) rateCode_ = 3;
    else { err = "hpsdr: --rate must be 48k, 96k, 192k or 384k"; return; }
    if (o_.freq <= 0) { err = "hpsdr: needs --band, --segment or --freq"; return; }
    if (o_.freq >= 4294967296.0) { err = "hpsdr: --freq is out of range"; return; }

    std::vector<Radio> found;
    if (target.empty()) {
      found = discover(nullptr, 1000);
      if (found.empty()) {
        err = "hpsdr: no radio answered the discovery broadcast (is it on this network? try hpsdr:ADDRESS)";
        return;
      }
    } else {
      std::string host = target;
      int port = kPort;
      if (size_t c = target.rfind(':'); c != std::string::npos) {
        host = target.substr(0, c);
        port = std::atoi(target.c_str() + c + 1);
      }
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(static_cast<uint16_t>(port));
      if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 || port <= 0) {
        err = "hpsdr: '" + target + "' is not an IPv4 ADDRESS or ADDRESS:PORT";
        return;
      }
      found = discover(&a, 1000);
      if (found.empty()) { err = "hpsdr: no answer from " + target; return; }
    }
    radio_ = found.front();
    if (found.size() > 1)
      std::fprintf(stderr, "cwsweep: %zu radios found, using %s (choose with hpsdr:ADDRESS)\n", found.size(),
                   describeRadio(radio_).c_str());
    // Only one program can own the radio. Taking it over would cut the other
    // one off, so refuse. (A Radioberry always reports idle.)
    if (radio_.busy) {
      err = "hpsdr: " + describeRadio(radio_) + " is in use by another program";
      return;
    }

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { err = std::string("hpsdr: ") + std::strerror(errno); return; }
    int rcv = 1 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    timeval tv{0, 200000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&radio_.addr), sizeof radio_.addr) < 0) {
      err = std::string("hpsdr: ") + std::strerror(errno);
      return;
    }

    // Settings, sent round-robin in the second frame of every packet.
    const uint32_t f = static_cast<uint32_t>(std::llround(o_.freq));
    auto freq = [f](uint8_t c0) { return Cc{c0, uint8_t(f >> 24), uint8_t(f >> 16), uint8_t(f >> 8), uint8_t(f)}; };
    settings_.push_back(freq(0x04));                 // receiver 1 frequency, Hz
    settings_.push_back(freq(0x02));                 // transmit frequency (never used)
    settings_.push_back(Cc{0x12, 0, 0x04, 0, 0});    // drive 0, PA off, T/R relay left in receive
    if (radio_.board == 6) {                         // Hermes Lite: LNA gain -12..+48 dB
      gainDb_ = std::clamp(o_.gainSet ? static_cast<int>(std::lround(o_.gain)) : 20, -12, 48);
      settings_.push_back(Cc{0x14, 0, 0, 0, uint8_t(0x40 | (gainDb_ + 12))});
    }
    // One command packet per ~20 ms of received data: delivers the settings
    // and feeds the Hermes Lite's watchdog.
    sendEvery_ = std::max(1, static_cast<int>(o_.rate / (2 * kSetsPerFrame) / 50));

    // Settings first, then start, repeated until samples arrive.
    for (int attempt = 0; attempt < 10 && !started_; ++attempt) {
      for (size_t i = 0; i < settings_.size(); ++i) sendCommand();
      uint8_t start[64] = {0xEF, 0xFE, 0x04, 0x01};
      ::send(fd_, start, sizeof start, 0);
      uint8_t buf[kPacket];
      for (int tries = 0; tries < 3 && !started_; ++tries) {
        ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n == static_cast<ssize_t>(kPacket) && buf[2] == 0x01 && buf[3] == 0x06) started_ = true;
      }
    }
    if (!started_) { err = "hpsdr: " + describeRadio(radio_) + " did not start streaming"; return; }
    timeval run{1, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &run, sizeof run);
  }

  ~HpsdrSource() override {
    if (fd_ < 0) return;
    if (started_) {
      uint8_t stop[64] = {0xEF, 0xFE, 0x04, 0x00};
      for (int i = 0; i < 2; ++i) ::send(fd_, stop, sizeof stop, 0);
    }
    ::close(fd_);
  }

  bool good() const { return started_; }
  double rate() const override { return o_.rate; }
  bool iq() const override { return true; }
  bool live() const override { return true; }

  std::string describe() const override {
    char b[240];
    int n = std::snprintf(b, sizeof b, "%s @ %.1f kHz, %.0f kS/s", describeRadio(radio_).c_str(), o_.freq / 1e3,
                          o_.rate / 1e3);
    if (radio_.board == 6) std::snprintf(b + n, sizeof b - static_cast<size_t>(n), ", LNA %+d dB", gainDb_);
    return b;
  }

  std::string health() override {
    std::string s;
    if (lost_) s = std::to_string(lost_) + " packets lost";
    if (overload_) s += std::string(s.empty() ? "" : ", ") + "ADC overload (lower --gain)";
    lost_ = overload_ = 0;
    return s.empty() ? s : "hpsdr: " + s;
  }

  bool read(std::vector<cfloat>& out, size_t max) override {
    out.clear();
    int timeouts = 0;
    while (out.size() + 2 * kSetsPerFrame <= std::max<size_t>(max, 2 * kSetsPerFrame)) {
      uint8_t buf[kPacket];
      ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
      if (n < 0) {
        if (errno == EINTR) return true;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && ++timeouts < 3) {
          sendCommand();  // keep asking; the radio may only have paused
          continue;
        }
        std::fprintf(stderr, "cwsweep: hpsdr: the radio stopped sending (%s)\n",
                     timeouts ? "timed out" : std::strerror(errno));
        return false;
      }
      if (n != static_cast<ssize_t>(kPacket) || buf[0] != 0xEF || buf[1] != 0xFE || buf[2] != 0x01 || buf[3] != 0x06)
        continue;
      timeouts = 0;
      uint32_t seq = uint32_t(buf[4]) << 24 | uint32_t(buf[5]) << 16 | uint32_t(buf[6]) << 8 | buf[7];
      // A lower number than expected is a restart or the counter wrapping
      // (the Hermes Lite counts 20 bits), not a loss.
      if (haveSeq_ && seq > nextSeq_) lost_ += seq - nextSeq_;
      nextSeq_ = seq + 1;
      haveSeq_ = true;

      for (int fr = 0; fr < 2; ++fr) {
        const uint8_t* p = buf + 8 + 512 * fr;
        if (p[0] != 0x7F || p[1] != 0x7F || p[2] != 0x7F) continue;
        // Status: address 0 (and not a command acknowledgement) carries the ADC overload bit.
        if (!(p[3] & 0x80) && ((p[3] >> 3) & 0x0F) == 0 && (p[4] & 1)) ++overload_;
        p += 8;
        for (int s = 0; s < kSetsPerFrame; ++s, p += 8) {
          auto s24 = [](const uint8_t* q) {
            return static_cast<float>((int32_t(int8_t(q[0])) << 16) | (q[1] << 8) | q[2]) * (1.0f / 8388608.0f);
          };
          float i = s24(p), q = s24(p + 3);
          out.push_back(o_.swapIq ? cfloat(q, i) : cfloat(i, q));
        }
      }
      if (++sinceSend_ >= sendEvery_) {
        sinceSend_ = 0;
        sendCommand();
      }
    }
    return true;
  }

 private:
  using Cc = std::array<uint8_t, 5>;

  void sendCommand() {
    uint8_t pkt[kPacket] = {0xEF, 0xFE, 0x01, 0x02};
    pkt[4] = uint8_t(txSeq_ >> 24);
    pkt[5] = uint8_t(txSeq_ >> 16);
    pkt[6] = uint8_t(txSeq_ >> 8);
    pkt[7] = uint8_t(txSeq_);
    ++txSeq_;
    // First frame: general settings. Sample rate, duplex (so that the
    // receiver has its own frequency), one receiver.
    const Cc general{0x00, uint8_t(rateCode_), 0, 0, 0x04};
    const Cc& other = settings_[nextSetting_];
    nextSetting_ = (nextSetting_ + 1) % settings_.size();
    const Cc* cc[2] = {&general, &other};
    for (int fr = 0; fr < 2; ++fr) {
      uint8_t* p = pkt + 8 + 512 * fr;
      p[0] = p[1] = p[2] = 0x7F;
      std::copy(cc[fr]->begin(), cc[fr]->end(), p + 3);
    }
    ::send(fd_, pkt, sizeof pkt, 0);
  }

  SourceOptions o_;
  Radio radio_;
  int fd_ = -1;
  int rateCode_ = 2, gainDb_ = 0;
  bool started_ = false;
  std::vector<Cc> settings_;
  size_t nextSetting_ = 0;
  uint32_t txSeq_ = 0, nextSeq_ = 0;
  bool haveSeq_ = false;
  int sendEvery_ = 1, sinceSend_ = 0;
  unsigned long lost_ = 0, overload_ = 0;
};

}  // namespace

std::unique_ptr<Source> openHpsdr(const SourceOptions& o, const std::string& target, std::string& err) {
  auto s = std::make_unique<HpsdrSource>(o, target, err);
  if (!err.empty() || !s->good()) {
    if (err.empty()) err = "hpsdr: cannot open the radio";
    return nullptr;
  }
  return s;
}

bool listHpsdr() {
  std::vector<Radio> found = discover(nullptr, 1500);
  if (found.empty()) {
    std::fprintf(stderr, "cwsweep: no openHPSDR radio answered the discovery broadcast\n");
    return false;
  }
  for (const Radio& r : found) {
    std::printf("%s", describeRadio(r).c_str());
    if (r.receivers > 0) std::printf(", %d receivers", r.receivers);
    std::printf("%s\n", r.busy ? ", in use" : "");
  }
  return true;
}

}  // namespace cwapp
