#pragma once
// Follows a radio through Hamlib's rigctld (the "NET rigctl" protocol):
// dial frequency, mode and transmit state are polled in the background, so
// that spots stay on frequency while the operator tunes, and the decoder can
// be paused while the station transmits.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace cwapp {

struct RigState {
  enum Mode { Other, USB, LSB, CW, CWR };
  bool connected = false;
  double freqHz = 0;  // 0 until the first answer
  Mode mode = Other;
  bool ptt = false;
};

class RigCtl {
 public:
  // `target` is HOST[:PORT]; the port defaults to rigctld's 4532.
  explicit RigCtl(const std::string& target);
  ~RigCtl();
  void start();
  RigState state() const;
  std::string target() const { return host_ + ":" + port_; }

 private:
  void run();
  bool session();  // one connection; returns when it fails
  bool command(int fd, const char* cmd, int lines, std::string* reply);

  std::string host_, port_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mu_;
  RigState st_;
  std::string in_;
  int pttSkip_ = 0;  // polls to wait before asking for the transmit state again
};

}  // namespace cwapp
