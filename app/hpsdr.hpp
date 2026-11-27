#pragma once
// Client for openHPSDR "protocol 1" radios on the network: Hermes Lite 2,
// Radioberry (which emulates a Hermes Lite), Hermes, ... openHpsdr() gives a
// receive-only source (cwsweep); openHpsdrSender() one that can also send CW
// (cwsend).

#include <memory>
#include <string>

#include "sources.hpp"

namespace cwapp {

// `target` is empty (find the radio by broadcast), ADDRESS or ADDRESS:PORT.
std::unique_ptr<Source> openHpsdr(const SourceOptions& o, const std::string& target, std::string& err);

struct HpsdrTx {
  double freq = 0;   // transmit frequency, Hz
  double wpm = 20;   // 5..60
  int drive = 255;   // drive level, 0..255
};

// A radio opened for sending. read() must keep being called: the received
// packets are the clock for the transmitted ones. transmitting() is true
// until everything queued has been sent and the transmitter released.
class HpsdrSender : public Source {
 public:
  // Queues text as CW and returns what will actually be sent.
  virtual std::string send(const std::string& text) = 0;
  // Stops sending and forgets what is queued.
  virtual void stopSending() = 0;
  virtual bool setSendSpeed(double wpm) = 0;
  virtual bool setSendFreq(double hz) = 0;
  virtual double sendSpeed() const = 0;
  virtual double sendFreq() const = 0;
};

std::unique_ptr<HpsdrSender> openHpsdrSender(const HpsdrTx& tx, const std::string& target, std::string& err);

// Prints the radios that answer a discovery broadcast. False if there are none.
bool listHpsdr();

}  // namespace cwapp
