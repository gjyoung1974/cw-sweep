#pragma once
// Synthetic CW band generator for tests and benchmarks.
//
// Models multiple stations with different pitches/speeds, human fist timing
// jitter, keying weight, Rayleigh QSB, QRN impulses and AWGN. SNR is signal
// (key-down) power relative to noise power in a 2500 Hz reference bandwidth.

#include <complex>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace cw {

struct Fist {
  double jitter = 0.0;    // relative std-dev of every element/gap
  double dahRatio = 3.0;
  double weight = 0.0;    // fraction of a dit added to marks / removed from gaps
  double charGap = 3.0;   // dits
  double wordGap = 7.0;
};

struct Station {
  double freq = 700;      // Hz (audio, or signed baseband for I/Q)
  double wpm = 25;
  std::string text;
  double snrDb = 10;      // in 2500 Hz
  double start = 0.5;     // s
  Fist fist;
  double qsbRate = 0;     // Hz (fading bandwidth), 0 = off
  double qsbDepth = 0;    // 0..1
  double riseMs = 5;
  double chirpHz = 0;
};

struct SynthParams {
  double sampleRate = 8000;
  double duration = 30;   // s
  bool iq = false;
  bool noise = true;
  double qrnRate = 0;     // impulses per second
  double qrnAmp = 30;     // relative to noise RMS
  uint64_t seed = 1;
};

// Returns samples (imag part is zero when !iq).
std::vector<std::complex<float>> synthesize(const std::vector<Station>& stations,
                                            const SynthParams& p);
// Duration in seconds of `text` at `wpm` with standard timing.
double messageDuration(const std::string& text, double wpm);

std::string randomCallsign(std::mt19937_64& rng);
std::string randomMessage(std::mt19937_64& rng, const std::string& call);
std::string randomText(std::mt19937_64& rng, int words);

}  // namespace cw
