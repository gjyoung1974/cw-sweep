// cwsend: send CW through an openHPSDR protocol 1 radio (Radioberry, Hermes Lite 2).
#include <poll.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "hpsdr.hpp"
#include "morse.hpp"

namespace {

volatile std::sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }

void usage(FILE* f) {
  std::fprintf(f, R"(usage: cwsend --freq F [options] [TEXT...]

Sends CW through a Radioberry, Hermes Lite 2 or other openHPSDR (protocol 1)
radio on the network. With TEXT, sends it and exits. Without, every line read
from standard input is sent, until the input ends. <AR> <BT> <KN> <SK> <AS> are
prosigns. Input lines that start with a slash are commands:
  /stop      stop sending and forget what is queued
  /wpm N     change the speed
  /freq F    change the transmit frequency

  --freq F              transmit frequency, e.g. 7030k or 7.03M (required)
  --wpm N               speed, 5..60 (20)
  --drive N             drive level, 0..255 (255, full power)
  --radio ADDRESS[:PORT]  the radio; found by broadcast if not given
  --discover            list the openHPSDR radios on the network and exit
  -h, --help

Only one program can use the radio at a time: not while cwsweep or an SDR
program is running on it.
)");
}

double parseFreq(const std::string& s) {
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end && *end) {
    switch (*end) {
      case 'k': case 'K': v *= 1e3; break;
      case 'm': case 'M': v *= 1e6; break;
      default: break;
    }
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  cwapp::HpsdrTx tx;
  std::string target, text;
  bool haveText = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
      return argv[++i];
    };
    try {
      if (a == "-h" || a == "--help") { usage(stdout); return 0; }
      else if (a == "--freq") tx.freq = parseFreq(next());
      else if (a == "--wpm") tx.wpm = std::stod(next());
      else if (a == "--drive") tx.drive = std::stoi(next());
      else if (a == "--radio") target = next();
      else if (a == "--discover") return cwapp::listHpsdr() ? 0 : 1;
      else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
      else {
        text += (haveText ? " " : "") + a;
        haveText = true;
      }
    } catch (const std::exception&) {
      std::fprintf(stderr, "bad value for %s\n", a.c_str());
      return 2;
    }
  }
  if (tx.freq <= 0) { usage(stderr); return 2; }

  std::string err;
  auto radio = cwapp::openHpsdrSender(tx, target, err);
  if (!radio) { std::fprintf(stderr, "cwsend: %s\n", err.c_str()); return 1; }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::fprintf(stderr, "cwsend: %s\n", radio->describe().c_str());

  auto line = [&](std::string l) {
    while (!l.empty() && std::isspace(static_cast<unsigned char>(l.back()))) l.pop_back();
    if (l.empty()) return;
    if (l[0] == '/') {
      const size_t sp = l.find(' ');
      const std::string cmd = l.substr(0, sp), arg = sp == std::string::npos ? "" : l.substr(sp + 1);
      if (cmd == "/stop") {
        radio->stopSending();
        std::fprintf(stderr, "cwsend: stopped sending\n");
      } else if (cmd == "/wpm" && radio->setSendSpeed(std::atof(arg.c_str()))) {
        std::fprintf(stderr, "cwsend: %.0f WPM\n", radio->sendSpeed());
      } else if (cmd == "/freq" && radio->setSendFreq(parseFreq(arg))) {
        std::fprintf(stderr, "cwsend: transmit frequency %.2f kHz\n", radio->sendFreq() / 1e3);
      } else {
        std::fprintf(stderr, "cwsend: commands are /stop, /wpm 5..60 and /freq F; other lines are sent\n");
      }
      return;
    }
    static const std::pair<const char*, char> kProsigns[] = {
        {"<AR>", '+'}, {"<BT>", '='}, {"<KN>", '('}, {"<SK>", '>'}, {"<AS>", '&'}};
    for (auto& ch : l) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    for (const auto& [name, ch] : kProsigns)
      for (size_t at; (at = l.find(name)) != std::string::npos;) l.replace(at, 4, 1, ch);
    const std::string sent = radio->send(l);
    if (!sent.empty())
      std::fprintf(stderr, "cwsend: sending on %.2f kHz at %.0f WPM: %s\n", radio->sendFreq() / 1e3,
                   radio->sendSpeed(), cw::renderText(sent).c_str());
  };

  // Lines from standard input are looked at between blocks of received samples.
  bool keyboard = !haveText;
  std::string typed;
  auto readKeyboard = [&] {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    while (keyboard && ::poll(&p, 1, 0) > 0) {
      char b[512];
      const ssize_t n = ::read(STDIN_FILENO, b, sizeof b);
      if (n <= 0) {  // end of input: what is queued is still sent
        keyboard = false;
        line(typed);
        break;
      }
      typed.append(b, static_cast<size_t>(n));
      for (size_t nl; (nl = typed.find('\n')) != std::string::npos; typed.erase(0, nl + 1))
        line(typed.substr(0, nl));
    }
  };
  if (haveText) line(text);
  else if (isatty(STDIN_FILENO)) std::fprintf(stderr, "cwsend: type a line to send it (/stop, /wpm N, /freq F; Ctrl-D to finish)\n");

  int rc = 0;
  std::vector<cwapp::cfloat> buf;
  while (!gStop) {
    if (!radio->read(buf, 2400)) { rc = 1; break; }  // 50 ms
    readKeyboard();
    if (!keyboard && !radio->transmitting()) break;
  }
  if (std::string h = radio->health(); !h.empty()) std::fprintf(stderr, "cwsend: %s\n", h.c_str());
  return rc;
}
