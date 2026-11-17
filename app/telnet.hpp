#pragma once
// Minimal DX-cluster compatible telnet server. Loggers (N1MM+, DXLog,
// CC-User, ...) connect, send a callsign at the login prompt, then receive
// "DX de ..." spot lines.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cwapp {

class TelnetServer {
 public:
  explicit TelnetServer(std::string nodeName) : node_(std::move(nodeName)) {}
  ~TelnetServer();
  bool start(int port, std::string& err);
  void stop();
  void broadcast(const std::string& line);
  int clientCount() const;

 private:
  struct Client {
    int fd;
    bool ready = false;
    std::string call, in;
    int iacSkip = 0;
  };
  void run();
  void sendTo(Client& c, const std::string& s);
  void onLine(Client& c, const std::string& line);

  std::string node_;
  int listenFd_ = -1;
  int wake_[2] = {-1, -1};
  std::thread thread_;
  std::atomic<bool> running_{false};
  mutable std::mutex mu_;
  std::vector<Client> clients_;
};

}  // namespace cwapp
