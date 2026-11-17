#include "synth.hpp"

#include <algorithm>
#include <cmath>

#include "morse.hpp"

namespace cw {
namespace {

struct Seg { bool down; double dur; };

std::vector<Seg> keying(const std::string& text, double wpm, const Fist& f, std::mt19937_64& rng) {
  const double dit = 1.2 / wpm;
  std::normal_distribution<double> nd(0.0, 1.0);
  auto dur = [&](double units, bool mark) {
    double base = units * dit + (mark ? 1 : -1) * f.weight * dit;
    if (f.jitter > 0) base *= std::max(0.3, 1.0 + f.jitter * nd(rng));
    return std::max(base, 0.25 * dit);
  };
  std::vector<Seg> segs;
  std::string norm = normalizeText(text);
  for (size_t i = 0; i < norm.size(); ++i) {
    char c = norm[i];
    if (c == ' ') continue;
    std::string_view code = codeFor(c);
    for (size_t e = 0; e < code.size(); ++e) {
      segs.push_back({true, dur(code[e] == '.' ? 1.0 : f.dahRatio, true)});
      if (e + 1 < code.size()) segs.push_back({false, dur(1.0, false)});
    }
    if (i + 1 < norm.size()) {
      bool word = norm[i + 1] == ' ';
      segs.push_back({false, dur(word ? f.wordGap : f.charGap, false)});
    }
  }
  return segs;
}

std::vector<float> envelope(const std::vector<Seg>& segs, double fs, double start, size_t n,
                            double riseMs) {
  std::vector<float> env(n, 0.0f);
  const double r = std::max(riseMs * 1e-3, 1.0 / fs);
  double t = start;
  for (const Seg& s : segs) {
    if (s.down) {
      double t0 = t, t1 = t + s.dur;
      auto i0 = static_cast<long>(std::floor(t0 * fs));
      auto i1 = static_cast<long>(std::ceil((t1 + r) * fs));
      for (long i = std::max(0L, i0); i < std::min<long>(static_cast<long>(n), i1); ++i) {
        double ti = i / fs;
        double up = std::clamp((ti - t0) / r, 0.0, 1.0);
        double dn = std::clamp((t1 + r - ti) / r, 0.0, 1.0);
        double v = 0.5 - 0.5 * std::cos(M_PI * std::min(up, dn));
        env[static_cast<size_t>(i)] = std::max(env[static_cast<size_t>(i)], static_cast<float>(v));
      }
    }
    t += s.dur;
  }
  return env;
}

std::vector<float> rayleigh(size_t n, double fs, double rate, std::mt19937_64& rng) {
  size_t decim = std::max<size_t>(1, static_cast<size_t>(fs / (20.0 * rate)));
  size_t m = n / decim + 4;
  std::normal_distribution<double> nd(0.0, 1.0);
  double alpha = std::exp(-2.0 * M_PI * rate * static_cast<double>(decim) / fs);
  std::vector<double> mag(m);
  std::complex<double> y{};
  // Warm the filter up so the start is stationary.
  for (int i = 0; i < 200; ++i) y = alpha * y + (1 - alpha) * std::complex<double>(nd(rng), nd(rng));
  double pw = 0;
  for (size_t i = 0; i < m; ++i) {
    y = alpha * y + (1 - alpha) * std::complex<double>(nd(rng), nd(rng));
    mag[i] = std::abs(y);
    pw += mag[i] * mag[i];
  }
  double norm = std::sqrt(pw / static_cast<double>(m));
  std::vector<float> out(n);
  for (size_t i = 0; i < n; ++i) {
    double x = static_cast<double>(i) / static_cast<double>(decim);
    size_t k = static_cast<size_t>(x);
    double fr = x - static_cast<double>(k);
    out[i] = static_cast<float>(((1 - fr) * mag[k] + fr * mag[k + 1]) / norm);
  }
  return out;
}

const char* kPfx[] = {"K", "W", "N", "AA", "KB", "KC", "WA", "VE", "G", "M", "F", "DL", "DK",
                      "I", "IK", "EA", "OH", "SM", "LA", "OZ", "PA", "ON", "OK", "SP", "HA",
                      "YO", "UA", "RA", "UR", "JA", "JH", "VK", "ZL", "ZS", "PY", "LU", "CE",
                      "XE", "HB", "OE", "S5", "9A", "EI", "4X", "ES", "LY", "HL", "UT", "RN"};
const char* kNames[] = {"BOB", "JIM", "TOM", "BILL", "JOHN", "ANN", "MIKE", "DAVE", "PETE",
                        "HANS", "KURT", "JOSE", "IVAN", "YURI", "KEN", "ED", "AL", "JOE"};
const char* kQth[] = {"NY", "CA", "TX", "OHIO", "BERLIN", "PARIS", "MOSCOW", "TOKYO",
                      "MADRID", "ROME", "LONDON", "WARSAW", "OSLO", "MA", "FL"};

template <size_t N>
const char* pick(const char* const (&arr)[N], std::mt19937_64& rng) {
  return arr[std::uniform_int_distribution<size_t>(0, N - 1)(rng)];
}

}  // namespace

double messageDuration(const std::string& text, double wpm) {
  std::mt19937_64 rng(0);
  double t = 0;
  for (const Seg& s : keying(text, wpm, Fist{}, rng)) t += s.dur;
  return t;
}

std::string randomCallsign(std::mt19937_64& rng) {
  std::string c = pick(kPfx, rng);
  if (!(c.back() >= '0' && c.back() <= '9'))
    c.push_back(static_cast<char>('0' + std::uniform_int_distribution<int>(0, 9)(rng)));
  int n = std::uniform_int_distribution<int>(1, 3)(rng);
  for (int i = 0; i < n; ++i) c.push_back(static_cast<char>('A' + std::uniform_int_distribution<int>(0, 25)(rng)));
  return c;
}

std::string randomMessage(std::mt19937_64& rng, const std::string& call) {
  std::string other = randomCallsign(rng);
  double k = std::uniform_real_distribution<double>(0, 1)(rng);
  int nr = std::uniform_int_distribution<int>(1, 40)(rng);
  if (k < 0.35) return "CQ CQ DE " + call + " " + call + " K";
  if (k < 0.6) return "CQ TEST " + call + " " + call;
  if (k < 0.8) return other + " 5NN " + std::to_string(nr) + " TU " + call;
  return other + " DE " + call + " R TNX FER CALL UR RST 599 NAME " + pick(kNames, rng) +
         " QTH " + pick(kQth, rng) + " HW? " + other + " DE " + call + " K";
}

std::string randomText(std::mt19937_64& rng, int words) {
  static const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::string out;
  for (int w = 0; w < words; ++w) {
    if (w) out.push_back(' ');
    int len = std::uniform_int_distribution<int>(2, 6)(rng);
    for (int i = 0; i < len; ++i) out.push_back(kAlpha[std::uniform_int_distribution<int>(0, 35)(rng)]);
  }
  return out;
}

std::vector<std::complex<float>> synthesize(const std::vector<Station>& stations,
                                            const SynthParams& p) {
  std::mt19937_64 rng(p.seed);
  const double fs = p.sampleRate;
  const size_t n = static_cast<size_t>(p.duration * fs);
  std::vector<std::complex<float>> out(n);
  std::uniform_real_distribution<double> ud(0.0, 2.0 * M_PI);

  for (const Station& st : stations) {
    auto env = envelope(keying(st.text, st.wpm, st.fist, rng), fs, st.start, n, st.riseMs);
    if (st.qsbRate > 0 && st.qsbDepth > 0) {
      auto fade = rayleigh(n, fs, st.qsbRate, rng);
      for (size_t i = 0; i < n; ++i)
        env[i] *= static_cast<float>((1.0 - st.qsbDepth) + st.qsbDepth * fade[i]);
    }
    const double amp = std::sqrt(std::pow(10.0, st.snrDb / 10.0) * (p.iq ? 1.0 : 2.0));
    double phase = ud(rng);
    for (size_t i = 0; i < n; ++i) {
      phase += 2.0 * M_PI * (st.freq + st.chirpHz * env[i]) / fs;
      if (phase > 2 * M_PI) phase -= 2 * M_PI;
      if (env[i] == 0.0f) continue;
      double a = amp * env[i];
      if (p.iq) out[i] += std::complex<float>(static_cast<float>(a * std::cos(phase)),
                                              static_cast<float>(a * std::sin(phase)));
      else out[i] += static_cast<float>(a * std::cos(phase));
    }
  }

  std::normal_distribution<double> nd(0.0, 1.0);
  if (p.noise) {
    if (p.iq) {
      double s = std::sqrt(fs / 2500.0 / 2.0);
      for (auto& x : out) x += std::complex<float>(static_cast<float>(s * nd(rng)), static_cast<float>(s * nd(rng)));
    } else {
      double s = std::sqrt(fs / 2.0 / 2500.0);
      for (auto& x : out) x += static_cast<float>(s * nd(rng));
    }
  }

  if (p.qrnRate > 0) {
    std::poisson_distribution<long> pd(p.qrnRate * p.duration);
    std::uniform_int_distribution<size_t> pos(0, n - 1);
    std::exponential_distribution<double> ed(1.0);
    long count = pd(rng);
    size_t width = std::max<size_t>(1, static_cast<size_t>(0.0005 * fs));
    double scale = p.qrnAmp * std::sqrt(fs / 2500.0);
    for (long k = 0; k < count; ++k) {
      size_t p0 = pos(rng);
      double a = scale * ed(rng);
      for (size_t i = p0; i < std::min(n, p0 + width); ++i) {
        double b = a * nd(rng);
        out[i] += p.iq ? std::complex<float>(static_cast<float>(b / M_SQRT2), static_cast<float>(b / M_SQRT2))
                       : std::complex<float>(static_cast<float>(b), 0.0f);
      }
    }
  }
  return out;
}

}  // namespace cw
