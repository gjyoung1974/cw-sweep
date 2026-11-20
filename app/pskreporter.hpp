#pragma once
// Uploads spots to PSK Reporter (pskreporter.info) as reception reports:
// IPFIX-style UDP datagrams, as described at pskreporter.info/pskdev.html.
// Reports are collected and sent in one datagram every five minutes.

#include <sys/socket.h>

#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cwapp {

struct PskReporterOptions {
  std::string host = "report.pskreporter.info";
  int port = 4739;
  std::string call;        // receiver (your) callsign
  std::string locator;     // receiver Maidenhead locator
  std::string antenna;     // optional free text, shown on the map
  std::string software;    // name and version of the decoder
  double intervalSec = 300;
};

struct PskReport {
  std::string call;
  uint32_t freqHz = 0;
  int8_t snrDb = 0;
  std::time_t time = 0;
};

// Checks a 4- or 6-character Maidenhead locator and normalises its case
// ("fn42HN" -> "FN42hn").
bool normaliseLocator(std::string& loc);

// Splits "HOST[:PORT]"; the port is left alone when none is given.
void parseHostPort(const std::string& s, std::string& host, int& port);

// One datagram: header, the record descriptors if `templates`, the receiver
// record, then the reports.
std::vector<uint8_t> buildPskDatagram(const PskReporterOptions& o, const std::vector<PskReport>& reports,
                                      bool templates, uint32_t sequence, uint32_t sessionId,
                                      std::time_t now);

class PskReporter {
 public:
  explicit PskReporter(PskReporterOptions o);
  ~PskReporter();
  void start();
  void stop();  // sends what is waiting, then returns
  // Queues a report. A call already reported on the same band within the
  // last five minutes is dropped.
  void add(const std::string& call, double rfHz, double snrDb);

 private:
  void run();
  void flush();
  bool openSocket(sockaddr_storage& to, unsigned& toLen);

  PskReporterOptions o_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool running_ = false;
  bool failing_ = false;
  std::vector<PskReport> pending_;
  std::map<std::string, std::time_t> lastReported_;  // "CALL/MHz" -> time
  int fd_ = -1;
  int family_ = 0;
  uint32_t sessionId_ = 0;
  uint32_t sequence_ = 1;
  int datagramsSent_ = 0;
  std::time_t lastTemplates_ = 0;
  bool announced_ = false;
};

}  // namespace cwapp
