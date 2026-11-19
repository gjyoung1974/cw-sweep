#pragma once
// Sample sources: sound files, raw pipes (rtl_sdr, rx_sdr, arecord, ...),
// PulseAudio/PipeWire capture and SoapySDR devices.

#include <complex>
#include <memory>
#include <string>
#include <vector>

namespace cwapp {

using cfloat = std::complex<float>;

struct SourceOptions {
  std::string spec;          // FILE | - | raw:PATH | pulse[:DEVICE] | soapy[:ARGS]
  double rate = 0;           // required for raw; default for pulse/soapy
  bool iq = false;           // input is I/Q (two channels / complex)
  bool swapIq = false;
  std::string format = "s16";  // raw: s16 | f32 | u8
  double freq = 0;           // soapy tuning frequency
  double gain = 0;           // soapy / hpsdr gain, dB
  bool gainSet = false;      // otherwise: soapy automatic, hpsdr its default
};

class Source {
 public:
  virtual ~Source() = default;
  virtual double rate() const = 0;
  virtual bool iq() const = 0;
  virtual bool live() const = 0;
  // Read up to `max` samples into `out` (resized). Returns false at end/error.
  virtual bool read(std::vector<cfloat>& out, size_t max) = 0;
  virtual std::string describe() const = 0;
  // The radio behind this source is transmitting (what it receives is our own signal).
  virtual bool transmitting() const { return false; }
  // Problems since the last call (lost packets, ADC overload); empty if none.
  virtual std::string health() { return {}; }
};

std::unique_ptr<Source> openSource(const SourceOptions& o, std::string& err);

// Writes the raw input (for later replay) as a WAV file.
class Recorder {
 public:
  Recorder(const std::string& path, double rate, bool iq, std::string& err);
  ~Recorder();
  void write(const std::vector<cfloat>& x);
  bool ok() const { return f_ != nullptr; }

 private:
  void* f_ = nullptr;
  bool iq_;
  std::vector<float> buf_;
};

}  // namespace cwapp
