#include "rigctl.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cwapp {

RigCtl::RigCtl(const std::string& target) {
  size_t colon = target.rfind(':');
  if (colon == std::string::npos) {
    host_ = target;
    port_ = "4532";
  } else {
    host_ = target.substr(0, colon);
    port_ = target.substr(colon + 1);
  }
  if (host_.empty()) host_ = "localhost";
}

RigCtl::~RigCtl() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void RigCtl::start() {
  thread_ = std::thread([this] { run(); });
}

RigState RigCtl::state() const {
  std::lock_guard lk(mu_);
  return st_;
}

void RigCtl::run() {
  bool complained = false;
  while (!stop_) {
    bool wasUp = session();
    {
      std::lock_guard lk(mu_);
      st_.connected = false;
      st_.ptt = false;
    }
    if (stop_) break;
    if (wasUp || !complained) {
      std::fprintf(stderr, "cwsweep: rigctld at %s: %s; retrying\n", target().c_str(),
                   wasUp ? "connection lost" : "cannot connect");
      complained = true;
    }
    for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// Sends one command and reads `lines` reply lines. rigctld answers a failed
// command with a single "RPRT <negative code>" line instead.
bool RigCtl::command(int fd, const char* cmd, int lines, std::string* reply) {
  std::string out = std::string(cmd) + "\n";
  if (::send(fd, out.data(), out.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(out.size())) return false;
  for (int got = 0; got < lines;) {
    size_t nl = in_.find('\n');
    if (nl == std::string::npos) {
      char buf[256];
      ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) return false;  // closed, or no answer within the timeout
      in_.append(buf, static_cast<size_t>(n));
      continue;
    }
    std::string line = in_.substr(0, nl);
    in_.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("RPRT", 0) == 0) {
      reply[0] = line;
      return true;
    }
    reply[got++] = line;
  }
  return true;
}

bool RigCtl::session() {
  addrinfo hints{}, *res = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res) != 0) return false;
  int fd = -1;
  for (addrinfo* a = res; a; a = a->ai_next) {
    fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return false;

  in_.clear();
  bool up = false;
  while (!stop_) {
    std::string r[2];
    RigState s;
    s.connected = true;
    if (!command(fd, "f", 1, r)) break;
    s.freqHz = r[0].rfind("RPRT", 0) == 0 ? 0 : std::strtod(r[0].c_str(), nullptr);
    if (!command(fd, "m", 2, r)) break;
    const std::string& m = r[0];
    if (m == "USB" || m == "PKTUSB") s.mode = RigState::USB;
    else if (m == "LSB" || m == "PKTLSB") s.mode = RigState::LSB;
    else if (m == "CW") s.mode = RigState::CW;
    else if (m == "CWR") s.mode = RigState::CWR;
    // A radio that can't report its transmit state answers with an error;
    // ask it again only now and then.
    if (pttSkip_ > 0) {
      --pttSkip_;
    } else {
      if (!command(fd, "t", 1, r)) break;
      if (r[0].rfind("RPRT", 0) == 0) pttSkip_ = 50;
      else s.ptt = std::atoi(r[0].c_str()) != 0;
    }
    {
      std::lock_guard lk(mu_);
      st_ = s;
    }
    if (!up) {
      up = true;
      std::fprintf(stderr, "cwsweep: following the radio through rigctld at %s\n", target().c_str());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ::close(fd);
  return up;
}

}  // namespace cwapp
