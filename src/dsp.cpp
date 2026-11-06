#include "dva/dsp.hpp"

#include <math.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace dva {

namespace {
constexpr double pi = std::numbers::pi;
constexpr double ceiling_db = -2, release_seconds = 0.2, lookahead_seconds = 0.005,
                 glide_seconds = 0.02;
constexpr int block_frames = 32;  // parameters are moved and filters redesigned this often
}  // namespace

BiquadCoefficients design_biquad(FilterSpec::Type type, double frequency, double q, double gain_db,
                                 double fs) {
  const double nyquist = fs / 2;
  const double f = std::clamp(std::isfinite(frequency) ? frequency : 0.0, 0.0, nyquist);
  const double a = std::pow(10.0, gain_db / 40);
  const double w0 = 2 * pi * f / fs;
  const double cw = std::cos(w0);
  const double sw = std::sin(w0);
  double b0 = NAN;
  double b1 = NAN;
  double b2 = NAN;
  double a0 = NAN;
  double a1 = NAN;
  double a2 = NAN;
  if (type == FilterSpec::Type::Peaking) {
    const double alpha = sw / (2 * std::max(q, 1e-6));
    b0 = 1 + alpha * a;
    b1 = -2 * cw;
    b2 = 1 - alpha * a;
    a0 = 1 + alpha / a;
    a1 = -2 * cw;
    a2 = 1 - alpha / a;
  } else {
    const double alpha =
        sw / 2 * std::numbers::sqrt2;  // shelf slope S = 1, as in the Web Audio specification
    const double k = 2 * std::sqrt(a) * alpha;
    if (type == FilterSpec::Type::LowShelf) {
      b0 = a * ((a + 1) - (a - 1) * cw + k);
      b1 = 2 * a * ((a - 1) - (a + 1) * cw);
      b2 = a * ((a + 1) - (a - 1) * cw - k);
      a0 = (a + 1) + (a - 1) * cw + k;
      a1 = -2 * ((a - 1) + (a + 1) * cw);
      a2 = (a + 1) + (a - 1) * cw - k;
    } else {
      b0 = a * ((a + 1) + (a - 1) * cw + k);
      b1 = -2 * a * ((a - 1) + (a + 1) * cw);
      b2 = a * ((a + 1) + (a - 1) * cw - k);
      a0 = (a + 1) - (a - 1) * cw + k;
      a1 = 2 * ((a - 1) - (a + 1) * cw);
      a2 = (a + 1) - (a - 1) * cw - k;
    }
  }
  BiquadCoefficients c;
  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;
  c.a2 = a2 / a0;
  return c;
}

double biquad_magnitude(const BiquadCoefficients& c, double frequency, double fs) {
  const double w = 2 * pi * frequency / fs;
  const std::complex<double> z1 = std::polar(1.0, -w);
  const std::complex<double> z2 = std::polar(1.0, -2 * w);
  return std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
}

double equalizer_response_db(const Bands& gains, double frequency, double fs) {
  double product = 1;
  const auto specs = filter_specs();
  for (std::size_t i = 0; i < band_count; i++) {
    product *= biquad_magnitude(
        design_biquad(specs[i].type, specs[i].frequency, specs[i].q, gains[i], fs), frequency, fs);
  }
  return 20 * std::log10(product);
}

Chain::Chain(double sample_rate) { set_sample_rate(sample_rate); }

void Chain::set_sample_rate(double sample_rate) {
  fs_ = sample_rate > 1000 ? sample_rate : 44100;
  lookahead_ = std::max(1, static_cast<int>(std::lround(lookahead_seconds * fs_)));
  delay_.assign(static_cast<std::size_t>(lookahead_) * 2, 0.0F);
  req_ring_.assign(static_cast<std::size_t>(lookahead_) + 1, 1.0);
  env_ring_.assign(static_cast<std::size_t>(lookahead_) + 1, 1.0);
  delay_at_ = ring_at_ = 0;
  env_ = 1;
  env_sum_ = static_cast<double>(lookahead_ + 1);
  last_gain_.store(1);
  design_all();
  reset();
}

void Chain::design_all() {
  const auto specs = filter_specs();
  for (std::size_t i = 0; i < band_count; i++) {
    bands_[i].c =
        design_biquad(specs[i].type, specs[i].frequency, specs[i].q, bands_[i].gain_db, fs_);
  }
}

void Chain::reset() {
  for (Band& b : bands_) {
    b.z1[0] = b.z1[1] = b.z2[0] = b.z2[1] = 0;
  }
  std::ranges::fill(delay_, 0.0F);
  std::ranges::fill(req_ring_, 1.0);
  std::ranges::fill(env_ring_, 1.0);
  env_ = 1;
  env_sum_ = static_cast<double>(lookahead_ + 1);
  last_gain_.store(1);
  tap_.fill(0.0F);
}

void Chain::configure(const Settings& s) {
  const Bands eq = effective_bands(s);
  for (std::size_t i = 0; i < band_count; i++) {
    bands_[i].target_db = eq[i];
  }
  preamp_target_ = std::pow(10.0, effective_preamp(s) / 20);
  volume_target_ = db_to_gain(s.volume_db);
  pan_target_ = balance_to_pan(s.balance);
  mono_target_ = s.mono ? 1 : 0;
  limiter_ = s.limiter;
}

void Chain::latest(float* out, std::size_t count) const {
  count = std::min(count, tap_size);
  const std::size_t end = tap_pos_.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; i++) {
    out[i] = tap_[(end + tap_size - count + i) % tap_size];
  }
}

double Chain::gain_reduction_db() const {
  const double g = last_gain_.load(std::memory_order_relaxed);
  return g >= 1 ? 0 : -20 * std::log10(std::max(g, 1e-6));
}

void Chain::glide(int frames) {
  const double k = 1 - std::exp(-frames / (glide_seconds * fs_));
  bool redesign = false;
  for (Band& b : bands_) {
    const double next = std::fabs(b.target_db - b.gain_db) < 1e-4
                            ? b.target_db
                            : b.gain_db + (b.target_db - b.gain_db) * k;
    if (next != b.gain_db) {
      b.gain_db = next;
      redesign = true;
    }
  }
  if (redesign) {
    design_all();
  }
  preamp_ += (preamp_target_ - preamp_) * k;
  volume_ += (volume_target_ - volume_) * k;
  pan_ += (pan_target_ - pan_) * k;
  mono_ += (mono_target_ - mono_) * (1 - std::exp(-frames / (0.005 * fs_)));
}

void Chain::process(float* x, int frames) {
  const double ceiling = std::pow(10.0, ceiling_db / 20);
  const double release_k = 1 - std::exp(-1.0 / (release_seconds * fs_));
  const std::size_t ring_size = static_cast<std::size_t>(lookahead_) + 1;
  int done = 0;
  while (done < frames) {
    const int n = std::min(block_frames, frames - done);
    glide(n);
    const double pan_x = pan_;
    const double pan_radian = pan_x <= 0 ? (pan_x + 1) * pi / 2 : pan_x * pi / 2;
    const double gl = std::cos(pan_radian);
    const double gr = std::sin(pan_radian);
    for (int i = 0; i < n; i++) {
      float* s = x + static_cast<std::size_t>(done + i) * 2;
      const double in_l = std::isfinite(s[0]) ? s[0] : 0.0;
      const double in_r =
          std::isfinite(s[1]) ? s[1] : 0.0;  // a damaged sample must not poison the filters
      double v[2] = {in_l * preamp_, in_r * preamp_};
      for (Band& b : bands_) {
        const BiquadCoefficients& c = b.c;
        for (int ch = 0; ch < 2; ch++) {
          const double in = v[ch];
          const double out = c.b0 * in + b.z1[ch];
          b.z1[ch] = c.b1 * in - c.a1 * out + b.z2[ch];
          b.z2[ch] = c.b2 * in - c.a2 * out;
          v[ch] = out;
        }
      }
      if (mono_ > 1e-6) {  // fold to mono (blended in over 5 ms)
        const double m = 0.5 * (v[0] + v[1]);
        v[0] += (m - v[0]) * mono_;
        v[1] += (m - v[1]) * mono_;
      }
      double l = NAN;
      double r = NAN;
      if (pan_x <= 0) {
        l = v[0] + v[1] * gl;
        r = v[1] * gr;
      } else {
        l = v[0] * gl;
        r = v[1] + v[0] * gr;
      }

      // look-ahead limiter: the gain needed by this sample, the lowest need within the look-ahead,
      // a release, a box average over the look-ahead
      const double peak = std::max(std::fabs(l), std::fabs(r));
      const double req = (limiter_ && peak > ceiling) ? ceiling / peak : 1.0;
      req_ring_[ring_at_] = req;
      double lowest = 1;
      for (const double q : req_ring_) {
        lowest = std::min(lowest, q);
      }
      env_ = lowest <= env_ ? lowest : env_ + (lowest - env_) * release_k;
      env_sum_ += env_ - env_ring_[ring_at_];
      env_ring_[ring_at_] = env_;
      const double gain = std::min(1.0, env_sum_ / static_cast<double>(ring_size));
      last_gain_.store(gain, std::memory_order_relaxed);
      ring_at_ = (ring_at_ + 1) % ring_size;

      float* d = &delay_[delay_at_ * 2];
      const double out_l = d[0] * gain;
      const double out_r = d[1] * gain;
      d[0] = static_cast<float>(l);
      d[1] = static_cast<float>(r);
      delay_at_ = (delay_at_ + 1) % static_cast<std::size_t>(lookahead_);
      const std::size_t at = tap_pos_.load(std::memory_order_relaxed);
      tap_[at % tap_size] = static_cast<float>(0.5 * (out_l + out_r));
      tap_pos_.store(at + 1, std::memory_order_release);
      s[0] = static_cast<float>(out_l * volume_);
      s[1] = static_cast<float>(out_r * volume_);
    }
    done += n;
  }
}

}  // namespace dva
