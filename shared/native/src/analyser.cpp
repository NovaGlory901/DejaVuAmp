#include "fh/analyser.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace fh {

void fft(std::span<std::complex<double>> a) {
  const std::size_t n = a.size();
  if (n == 0 || (n & (n - 1)) != 0) {
    throw std::invalid_argument("fft size must be a power of two");
  }
  for (std::size_t i = 1, j = 0; i < n; i++) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(a[i], a[j]);
    }
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * std::numbers::pi / static_cast<double>(len);
    const std::complex<double> step(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1, 0);
      for (std::size_t k = 0; k < len / 2; k++) {
        const std::complex<double> u = a[i + k];
        const std::complex<double> v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= step;
      }
    }
  }
}

Analyser::Analyser(std::size_t fft_size, double smoothing, double min_db, double max_db)
    : fft_size_(fft_size),
      smoothing_(std::clamp(smoothing, 0.0, 1.0)),
      min_db_(min_db),
      max_db_(max_db) {
  if (fft_size < 32 || (fft_size & (fft_size - 1)) != 0) {
    throw std::invalid_argument("fftSize must be a power of two, at least 32");
  }
  if (!(max_db > min_db)) {
    throw std::invalid_argument("maxDb must be above minDb");
  }
  ring_.assign(fft_size_, 0.0F);
  smoothed_.assign(fft_size_ / 2, 0.0);
  window_.resize(fft_size_);
  const double alpha = 0.16;  // Blackman window, as the Web Audio specification says
  for (std::size_t i = 0; i < fft_size_; i++) {
    const double x = static_cast<double>(i) / static_cast<double>(fft_size_);
    window_[i] = (1 - alpha) / 2 - 0.5 * std::cos(2 * std::numbers::pi * x) +
                 alpha / 2 * std::cos(4 * std::numbers::pi * x);
  }
}

void Analyser::push(std::span<const float> samples) {
  if (samples.size() >= fft_size_) {  // more than we can keep: only the newest matter
    std::copy(samples.end() - static_cast<std::ptrdiff_t>(fft_size_), samples.end(), ring_.begin());
    head_ = 0;
    return;
  }
  for (const float s : samples) {
    ring_[head_] = s;
    head_ = (head_ + 1) % fft_size_;
  }
}

void Analyser::reset() {
  std::ranges::fill(ring_, 0.0F);
  std::ranges::fill(smoothed_, 0.0);
  head_ = 0;
}

void Analyser::read(std::vector<std::uint8_t>& spectrum, std::vector<std::uint8_t>& waveform) {
  std::vector<std::complex<double>> x(fft_size_);
  waveform.resize(fft_size_);
  for (std::size_t i = 0; i < fft_size_; i++) {
    const float s = ring_[(head_ + i) % fft_size_];
    const double v = std::isfinite(s) ? s : 0.0;
    x[i] = v * window_[i];
    waveform[i] = static_cast<std::uint8_t>(std::clamp(std::floor(128.0 * (1.0 + v)), 0.0, 255.0));
  }
  fft(x);
  spectrum.resize(bin_count());
  for (std::size_t k = 0; k < bin_count(); k++) {
    const double mag = std::abs(x[k]) / static_cast<double>(fft_size_);
    smoothed_[k] = smoothing_ * smoothed_[k] + (1 - smoothing_) * mag;
    const double db = smoothed_[k] > 0 ? 20 * std::log10(smoothed_[k]) : -1000.0;
    spectrum[k] = static_cast<std::uint8_t>(
        std::clamp(std::floor(255.0 / (max_db_ - min_db_) * (db - min_db_)), 0.0, 255.0));
  }
}

}  // namespace fh
