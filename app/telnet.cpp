#include "telnet.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

namespace cwapp {

TelnetServer::~TelnetServer() { stop(); }

bool TelnetServer::start(int port, std::string& err) {
  listenFd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
  bool v6 = listenFd_ >= 0;
  if (!v6) listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) { err = std::strerror(errno); return false; }
  int one = 1, zero = 0;
  setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  int rc;
  if (v6) {
    setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(static_cast<uint16_t>(port));
    a.sin6_addr = in6addr_any;
    rc = ::bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
  } else {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    rc = ::bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
  }
  if (rc < 0 || ::listen(listenFd_, 16) < 0) {
    err = "telnet port " + std::to_string(port) + ": " + std::strerror(errno);
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  if (::pipe(wake_) < 0) { err = std::strerror(errno); return false; }
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void TelnetServer::stop() {
  if (!running_.exchange(false)) return;
  char b = 0;
  (void)!::write(wake_[1], &b, 1);
  if (thread_.joinable()) thread_.join();
  std::lock_guard lk(mu_);
  for (auto& c : clients_) ::close(c.fd);
  clients_.clear();
  ::close(listenFd_);
  ::close(wake_[0]);
  ::close(wake_[1]);
}

int TelnetServer::clientCount() const {
  std::lock_guard lk(mu_);
  return static_cast<int>(std::count_if(clients_.begin(), clients_.end(), [](auto& c) { return c.ready; }));
}

void TelnetServer::sendTo(Client& c, const std::string& s) {
  if (c.fd < 0) return;
  ssize_t n = ::send(c.fd, s.data(), s.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    ::close(c.fd);
    c.fd = -1;
  }
}

void TelnetServer::broadcast(const std::string& line) {
  std::lock_guard lk(mu_);
  for (auto& c : clients_)
    if (c.ready) sendTo(c, line + "\r\n");
}

void TelnetServer::onLine(Client& c, const std::string& raw) {
  std::string line;
  for (char ch : raw)
    if (std::isprint(static_cast<unsigned char>(ch))) line.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  while (!line.empty() && line.back() == ' ') line.pop_back();
  if (!c.ready) {
    if (line.empty()) { sendTo(c, "Please enter your callsign: "); return; }
    c.call = line;
    c.ready = true;
    sendTo(c, "Hello " + c.call + ", this is " + node_ + " (cwsweep CW decoder).\r\n" + c.call + " de " + node_ + " >\r\n");
    return;
  }
  if (line == "BYE" || line == "QUIT" || line == "EXIT" || line == "Q") {
    sendTo(c, "73 de " + node_ + "\r\n");
    ::close(c.fd);
    c.fd = -1;
    return;
  }
  // Loggers send set/... and sh/... commands; acknowledge with a prompt.
  sendTo(c, c.call + " de " + node_ + " >\r\n");
}

void TelnetServer::run() {
  while (running_) {
    std::vector<pollfd> fds;
    {
      std::lock_guard lk(mu_);
      fds.push_back({wake_[0], POLLIN, 0});
      fds.push_back({listenFd_, POLLIN, 0});
      for (auto& c : clients_) fds.push_back({c.fd, POLLIN, 0});
    }
    if (::poll(fds.data(), fds.size(), 500) < 0 && errno != EINTR) break;
    if (!running_) break;
    std::lock_guard lk(mu_);
    if (fds[1].revents & POLLIN) {
      int fd = ::accept(listenFd_, nullptr, nullptr);
      if (fd >= 0) {
        clients_.push_back(Client{fd, false, {}, {}, 0});
        sendTo(clients_.back(), "\r\nWelcome to " + node_ + " (cwsweep)\r\n\r\nPlease enter your callsign: ");
      }
    }
    for (size_t i = 2; i < fds.size(); ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      auto it = std::find_if(clients_.begin(), clients_.end(), [&](auto& c) { return c.fd == fds[i].fd; });
      if (it == clients_.end()) continue;
      char buf[512];
      ssize_t n = ::recv(it->fd, buf, sizeof buf, 0);
      if (n <= 0) { ::close(it->fd); it->fd = -1; continue; }
      for (ssize_t k = 0; k < n && it->fd >= 0; ++k) {
        auto b = static_cast<unsigned char>(buf[k]);
        if (it->iacSkip > 0) { --it->iacSkip; continue; }
        if (b == 255) { it->iacSkip = 2; continue; }  // telnet IAC negotiation
        if (b == '\n' || b == '\r') {
          if (b == '\n' || !it->in.empty()) onLine(*it, it->in);
          it->in.clear();
          if (b == '\r' && k + 1 < n && buf[k + 1] == '\n') ++k;
        } else if (it->in.size() < 256) {
          it->in.push_back(static_cast<char>(b));
        }
      }
    }
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](auto& c) { return c.fd < 0; }),
                   clients_.end());
  }
}

}  // namespace cwapp
