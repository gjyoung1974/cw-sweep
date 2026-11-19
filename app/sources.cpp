#include "sources.hpp"

#include "hpsdr.hpp"

#include <sndfile.h>

#include <cstdio>
#include <cstring>
#include <utility>

#ifdef CWSWEEP_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif
#ifdef CWSWEEP_HAVE_SOAPY
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#endif

namespace cwapp {
namespace {

cfloat makeSample(float a, float b, bool iq, bool swap, int channels) {
  if (iq) return swap ? cfloat(b, a) : cfloat(a, b);
  return channels >= 2 ? cfloat(0.5f * (a + b), 0.0f) : cfloat(a, 0.0f);
}

// ---------------------------------------------------------------------------
class SndfileSource : public Source {
 public:
  SndfileSource(const SourceOptions& o, std::string& err) : o_(o) {
    f_ = sf_open(o.spec.c_str(), SFM_READ, &info_);
    if (!f_) { err = "cannot open '" + o.spec + "': " + sf_strerror(nullptr); return; }
    if (o.iq && info_.channels < 2) { err = "--iq needs a 2-channel file"; sf_close(f_); f_ = nullptr; }
  }
  ~SndfileSource() override { if (f_) sf_close(f_); }
  bool good() const { return f_ != nullptr; }
  double rate() const override { return info_.samplerate; }
  bool iq() const override { return o_.iq; }
  bool live() const override { return false; }
  std::string describe() const override {
    return o_.spec + " (" + std::to_string(info_.samplerate) + " Hz, " +
           std::to_string(info_.channels) + " ch" + (o_.iq ? ", I/Q" : "") + ")";
  }
  bool read(std::vector<cfloat>& out, size_t max) override {
    buf_.resize(max * static_cast<size_t>(info_.channels));
    sf_count_t n = sf_readf_float(f_, buf_.data(), static_cast<sf_count_t>(max));
    out.resize(static_cast<size_t>(std::max<sf_count_t>(n, 0)));
    for (size_t i = 0; i < out.size(); ++i) {
      const float* fr = &buf_[i * static_cast<size_t>(info_.channels)];
      out[i] = makeSample(fr[0], info_.channels > 1 ? fr[1] : 0.0f, o_.iq, o_.swapIq, info_.channels);
    }
    return n > 0;
  }

 private:
  SourceOptions o_;
  SF_INFO info_{};
  SNDFILE* f_ = nullptr;
  std::vector<float> buf_;
};

// ---------------------------------------------------------------------------
class RawSource : public Source {
 public:
  RawSource(const SourceOptions& o, std::string path, std::string& err) : o_(o), path_(std::move(path)) {
    if (o.rate <= 0) { err = "raw input needs --rate"; return; }
    if (o.format == "s16") width_ = 2;
    else if (o.format == "f32") width_ = 4;
    else if (o.format == "u8") width_ = 1;
    else { err = "unknown --format '" + o.format + "' (s16, f32, u8)"; return; }
    f_ = path_ == "-" ? stdin : std::fopen(path_.c_str(), "rb");
    if (!f_) err = "cannot open '" + path_ + "': " + std::strerror(errno);
  }
  ~RawSource() override { if (f_ && f_ != stdin) std::fclose(f_); }
  bool good() const { return f_ != nullptr; }
  double rate() const override { return o_.rate; }
  bool iq() const override { return o_.iq; }
  bool live() const override { return path_ == "-"; }
  std::string describe() const override {
    return "raw " + path_ + " (" + o_.format + ", " + std::to_string(static_cast<long>(o_.rate)) +
           " Hz" + (o_.iq ? ", I/Q" : "") + ")";
  }
  bool read(std::vector<cfloat>& out, size_t max) override {
    const int ch = o_.iq ? 2 : 1;
    const size_t frameBytes = static_cast<size_t>(width_ * ch);
    buf_.resize(max * frameBytes);
    size_t got = std::fread(buf_.data() + carry_, 1, buf_.size() - carry_, f_) + carry_;
    size_t frames = got / frameBytes;
    carry_ = got - frames * frameBytes;
    out.resize(frames);
    auto sample = [&](size_t idx) -> float {
      const unsigned char* p = buf_.data() + idx * static_cast<size_t>(width_);
      switch (width_) {
        case 1: return (static_cast<float>(*p) - 127.5f) / 127.5f;
        case 2: { int16_t v; std::memcpy(&v, p, 2); return v / 32768.0f; }
        default: { float v; std::memcpy(&v, p, 4); return v; }
      }
    };
    for (size_t i = 0; i < frames; ++i)
      out[i] = makeSample(sample(i * ch), ch > 1 ? sample(i * ch + 1) : 0.0f, o_.iq, o_.swapIq, ch);
    if (carry_) std::memmove(buf_.data(), buf_.data() + frames * frameBytes, carry_);
    return frames > 0 || !std::feof(f_);
  }

 private:
  SourceOptions o_;
  std::string path_;
  FILE* f_ = nullptr;
  int width_ = 2;
  std::vector<unsigned char> buf_;
  size_t carry_ = 0;
};

// ---------------------------------------------------------------------------
#ifdef CWSWEEP_HAVE_PULSE
class PulseSource : public Source {
 public:
  PulseSource(const SourceOptions& o, std::string device, std::string& err) : o_(o), dev_(std::move(device)) {
    if (o_.rate <= 0) o_.rate = 48000;
    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = static_cast<uint32_t>(o_.rate);
    ss.channels = o_.iq ? 2 : 1;
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.fragsize = static_cast<uint32_t>(o_.rate * 0.02) * ss.channels * 4;
    int e = 0;
    s_ = pa_simple_new(nullptr, "cwsweep", PA_STREAM_RECORD, dev_.empty() ? nullptr : dev_.c_str(),
                       "cwsweep input", &ss, nullptr, &attr, &e);
    if (!s_) err = std::string("PulseAudio: ") + pa_strerror(e);
  }
  ~PulseSource() override { if (s_) pa_simple_free(s_); }
  bool good() const { return s_ != nullptr; }
  double rate() const override { return o_.rate; }
  bool iq() const override { return o_.iq; }
  bool live() const override { return true; }
  std::string describe() const override {
    return "PulseAudio " + (dev_.empty() ? std::string("default source") : dev_) + " (" +
           std::to_string(static_cast<long>(o_.rate)) + " Hz" + (o_.iq ? ", I/Q" : "") + ")";
  }
  bool read(std::vector<cfloat>& out, size_t max) override {
    const int ch = o_.iq ? 2 : 1;
    size_t n = std::min<size_t>(max, static_cast<size_t>(o_.rate * 0.02));
    buf_.resize(n * ch);
    int e = 0;
    if (pa_simple_read(s_, buf_.data(), buf_.size() * sizeof(float), &e) < 0) return false;
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
      out[i] = makeSample(buf_[i * ch], ch > 1 ? buf_[i * ch + 1] : 0.0f, o_.iq, o_.swapIq, ch);
    return true;
  }

 private:
  SourceOptions o_;
  std::string dev_;
  pa_simple* s_ = nullptr;
  std::vector<float> buf_;
};
#endif

// ---------------------------------------------------------------------------
#ifdef CWSWEEP_HAVE_SOAPY
class SoapySource : public Source {
 public:
  SoapySource(const SourceOptions& o, std::string args, std::string& err) : o_(o), args_(std::move(args)) {
    o_.iq = true;
    if (o_.rate <= 0) o_.rate = 192000;
    if (o_.freq <= 0) { err = "SoapySDR input needs --freq"; return; }
    dev_ = SoapySDRDevice_makeStrArgs(args_.c_str());
    if (!dev_) { err = std::string("SoapySDR: ") + SoapySDRDevice_lastError(); return; }
    SoapySDRDevice_setSampleRate(dev_, SOAPY_SDR_RX, 0, o_.rate);
    SoapySDRDevice_setFrequency(dev_, SOAPY_SDR_RX, 0, o_.freq, nullptr);
    if (!o_.gainSet) SoapySDRDevice_setGainMode(dev_, SOAPY_SDR_RX, 0, true);
    else SoapySDRDevice_setGain(dev_, SOAPY_SDR_RX, 0, o_.gain);
    o_.rate = SoapySDRDevice_getSampleRate(dev_, SOAPY_SDR_RX, 0);
    stream_ = SoapySDRDevice_setupStream(dev_, SOAPY_SDR_RX, SOAPY_SDR_CF32, nullptr, 0, nullptr);
    if (!stream_ || SoapySDRDevice_activateStream(dev_, stream_, 0, 0, 0) != 0) {
      err = std::string("SoapySDR stream: ") + SoapySDRDevice_lastError();
    }
  }
  ~SoapySource() override {
    if (stream_) {
      SoapySDRDevice_deactivateStream(dev_, stream_, 0, 0);
      SoapySDRDevice_closeStream(dev_, stream_);
    }
    if (dev_) SoapySDRDevice_unmake(dev_);
  }
  bool good() const { return dev_ && stream_; }
  double rate() const override { return o_.rate; }
  bool iq() const override { return true; }
  bool live() const override { return true; }
  std::string describe() const override {
    return "SoapySDR '" + args_ + "' @ " + std::to_string(o_.freq / 1e3) + " kHz, " +
           std::to_string(static_cast<long>(o_.rate)) + " S/s";
  }
  bool read(std::vector<cfloat>& out, size_t max) override {
    out.resize(max);
    void* buffs[] = {out.data()};
    int flags = 0;
    long long t = 0;
    int n = SoapySDRDevice_readStream(dev_, stream_, buffs, max, &flags, &t, 1000000);
    if (n == SOAPY_SDR_OVERFLOW || n == SOAPY_SDR_TIMEOUT) { out.clear(); return true; }
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    if (o_.swapIq) for (auto& v : out) v = cfloat(v.imag(), v.real());
    return true;
  }

 private:
  SourceOptions o_;
  std::string args_;
  SoapySDRDevice* dev_ = nullptr;
  SoapySDRStream* stream_ = nullptr;
};
#endif

template <class T, class... A>
std::unique_ptr<Source> make(std::string& err, A&&... a) {
  auto s = std::make_unique<T>(std::forward<A>(a)..., err);
  if (!err.empty() || !s->good()) {
    if (err.empty()) err = "cannot open source";
    return nullptr;
  }
  return s;
}

}  // namespace

std::unique_ptr<Source> openSource(const SourceOptions& o, std::string& err) {
  const std::string& s = o.spec;
  if (s == "-") return make<RawSource>(err, o, std::string("-"));
  if (s.rfind("raw:", 0) == 0) return make<RawSource>(err, o, s.substr(4));
  if (s == "pulse" || s.rfind("pulse:", 0) == 0) {
#ifdef CWSWEEP_HAVE_PULSE
    return make<PulseSource>(err, o, s.size() > 6 ? s.substr(6) : std::string());
#else
    err = "built without PulseAudio support";
    return nullptr;
#endif
  }
  if (s == "hpsdr" || s.rfind("hpsdr:", 0) == 0) return openHpsdr(o, s.size() > 6 ? s.substr(6) : std::string(), err);
  if (s == "soapy" || s.rfind("soapy:", 0) == 0) {
#ifdef CWSWEEP_HAVE_SOAPY
    return make<SoapySource>(err, o, s.size() > 6 ? s.substr(6) : std::string());
#else
    err = "built without SoapySDR support";
    return nullptr;
#endif
  }
  return make<SndfileSource>(err, o);
}

Recorder::Recorder(const std::string& path, double rate, bool iq, std::string& err) : iq_(iq) {
  SF_INFO info{};
  info.samplerate = static_cast<int>(rate);
  info.channels = iq ? 2 : 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  f_ = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!f_) err = "cannot record to '" + path + "': " + sf_strerror(nullptr);
}

Recorder::~Recorder() { if (f_) sf_close(static_cast<SNDFILE*>(f_)); }

void Recorder::write(const std::vector<cfloat>& x) {
  if (!f_) return;
  if (iq_) {
    sf_writef_float(static_cast<SNDFILE*>(f_), reinterpret_cast<const float*>(x.data()),
                    static_cast<sf_count_t>(x.size()));
  } else {
    buf_.resize(x.size());
    for (size_t i = 0; i < x.size(); ++i) buf_[i] = x[i].real();
    sf_writef_float(static_cast<SNDFILE*>(f_), buf_.data(), static_cast<sf_count_t>(x.size()));
  }
}

}  // namespace cwapp
