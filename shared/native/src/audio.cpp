#include "fh/audio.hpp"

#include <algorithm>
#include <cmath>

namespace fh {

namespace {

double follow(double current, double target, double up = 0.5, double down = 0.08) {
  return current + (target - current) * (target > current ? up : down);
}
double hear(double v) { return std::min(1.0, v * sensitivity); }

}  // namespace

BandEnergies band_energies(std::span<const std::uint8_t> bytes, double sample_rate) {
  if (bytes.empty() || !(sample_rate > 0)) {
    return {};
  }
  const auto n = static_cast<double>(bytes.size());
  const double bin_hz = (sample_rate / 2) / n;
  const auto mean = [&](double from_hz, double to_hz) {
    const double a = std::max(0.0, std::floor(from_hz / bin_hz));
    const double b = std::min(n, std::max(a + 1, std::ceil(to_hz / bin_hz)));
    double sum = 0;
    for (double k = a; k < b; k++) {
      sum += bytes[static_cast<std::size_t>(k)];
    }
    return sum / (b - a) / 255;
  };
  BandEnergies e{};
  e.bass = mean(20, 250);
  e.mid = mean(250, 2500);
  e.treb = mean(2500, 12000);
  e.level = (e.bass * 2 + e.mid + e.treb) / 4;
  return e;
}

void AudioFeatures::update(std::span<const std::uint8_t> freq, std::span<const std::uint8_t> time,
                           double sample_rate, double now_ms) {
  if (!(sample_rate > 0)) {
    sample_rate = 44100;
  }
  if (!freq.empty()) {
    const BandEnergies e = band_energies(freq, sample_rate);
    bass = follow(bass, hear(e.bass));
    mid = follow(mid, hear(e.mid));
    treb = follow(treb, hear(e.treb));
    level = follow(level, hear(e.level));
    const bool is_beat = e.bass > 0.25 && e.bass > avg_ * 1.35 && now_ms - last_beat_ >= 400;
    avg_ += (e.bass - avg_) * 0.08;
    if (is_beat) {
      last_beat_ = now_ms;
      beat = 1;
    } else {
      beat *= 0.9;
    }
    const auto len = static_cast<double>(freq.size());
    const double bin_hz = sample_rate / 2 / len;
    const double lo = 40;
    const double hi = 16000;
    for (int k = 0; k < spec_size; k++) {
      const double f0 = lo * std::pow(hi / lo, k / 64.0);
      const double f1 = lo * std::pow(hi / lo, (k + 1) / 64.0);
      const double a = std::min(len - 1, std::floor(f0 / bin_hz));
      const double b = std::min(len, std::max(a + 1, std::ceil(f1 / bin_hz)));
      double s = 0;
      for (double i = a; i < b; i++) {
        s += freq[static_cast<std::size_t>(i)];
      }
      spec[static_cast<std::size_t>(k)] = static_cast<float>(
          follow(spec[static_cast<std::size_t>(k)], hear(s / (b - a) / 255), 0.6, 0.15));
    }
  } else {
    bass *= 0.95;
    mid *= 0.95;
    treb *= 0.95;
    level *= 0.95;
    beat *= 0.9;
    for (float& v : spec) {
      v = static_cast<float>(v * 0.9);
    }
  }
  bass_att += (bass - bass_att) * 0.04;
  mid_att += (mid - mid_att) * 0.04;
  treb_att += (treb - treb_att) * 0.04;
  if (!time.empty()) {
    for (std::size_t i = 0; i < wave.size(); i++) {
      const double sample = time[i * time.size() / wave.size()];
      wave[i] = static_cast<float>(std::clamp((sample - 128) / 128 * sensitivity, -1.0, 1.0));
    }
  } else {
    for (float& v : wave) {
      v = static_cast<float>(v * 0.9);
    }
  }
}

}  // namespace fh
