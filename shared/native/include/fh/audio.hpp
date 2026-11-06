// Port of bandEnergies and AudioFeatures in js/falsehorizon-core.js (the reference).
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace fh {

// Heard 20% louder than the analyser says (capped at 1); the beat detector uses the true level.
constexpr double sensitivity = 1.2;

struct BandEnergies {
  double bass, mid, treb, level;
};

// Average strength (0..1) of 20-250 Hz, 250-2500 Hz and 2500-12000 Hz in a spectrum covering 0 ..
// sample_rate/2. Nothing is heard (all zero) if there is no spectrum or the rate is not positive.
BandEnergies band_energies(std::span<const std::uint8_t> bytes, double sample_rate);

class AudioFeatures {
 public:
  static constexpr int wave_size = 256, spec_size = 64;

  double bass = 0, mid = 0, treb = 0, level = 0;
  double bass_att = 0, mid_att = 0, treb_att = 0;
  double beat = 0;
  std::array<float, wave_size>
      wave{};  // floats on purpose: the JavaScript keeps these in a Float32Array
  std::array<float, spec_size> spec{};

  // Call once per frame. An empty spectrum or waveform means "no analyser": everything fades
  // towards calm instead of freezing. now_ms is any steadily increasing clock in milliseconds.
  void update(std::span<const std::uint8_t> freq, std::span<const std::uint8_t> time,
              double sample_rate, double now_ms);

 private:
  double avg_ = 0, last_beat_ = -1e9;
};

}  // namespace fh
