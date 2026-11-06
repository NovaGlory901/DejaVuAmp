// Mimics the browser's AnalyserNode (Blackman window, FFT, time smoothing, dB mapped onto 0..255)
// so the visuals behave as in a browser. Defaults match the browser's: 2048, 0.7, -100..-30 dB.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fh {

// In-place FFT of a power-of-two sized array. Throws std::invalid_argument for any other size.
void fft(std::span<std::complex<double>> data);

class Analyser {
 public:
  explicit Analyser(std::size_t fft_size = 2048, double smoothing = 0.7, double min_db = -100,
                    double max_db = -30);

  [[nodiscard]] std::size_t fft_size() const { return fft_size_; }
  [[nodiscard]] std::size_t bin_count() const { return fft_size_ / 2; }

  // Adds samples (-1..1, one channel) to the end of what has been heard. Only the newest fft_size
  // are kept.
  void push(std::span<const float> samples);

  void reset();

  // The spectrum (bin_count bytes, 0 Hz up to half the sample rate) and the waveform (fft_size
  // bytes) of what was heard last. Call once per frame: each call moves the smoothing on one step.
  void read(std::vector<std::uint8_t>& spectrum, std::vector<std::uint8_t>& waveform);

 private:
  std::size_t fft_size_;
  double smoothing_, min_db_, max_db_;
  std::vector<float> ring_;  // the newest fft_size samples, oldest first once full
  std::size_t head_ = 0;
  std::vector<double> window_, smoothed_;
};

}  // namespace fh
