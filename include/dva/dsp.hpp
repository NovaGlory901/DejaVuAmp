// DejaVuAmp's sound chain, in the order of the web version's audio graph: preamp -> 10-band
// equalizer -> optional mono fold -> balance -> clip guard -> (tap for the spectrum) -> volume.
// Interleaved stereo float, in place, no audio device, so it can be tested with made-up signals.
//
// Where it follows the web version: biquad filters and the equal-power balance are Web Audio's, and
// parameters glide with a 20 ms time constant so dragging a slider does not click. The clip guard
// is NOT the browser's compressor (which differs between browsers): it is a look-ahead limiter,
// ceiling -2 dB, 5 ms look-ahead, 200 ms release. It always delays the sound by the look-ahead, on
// or off, so switching it does not click.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

#include "dva/core.hpp"

namespace dva {

// One second-order filter section (a "biquad"). Coefficients follow the Web Audio specification's
// formulas.
struct BiquadCoefficients {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // divided by a0
};
// `gain_db` boosts or cuts; `frequency` is clamped to 0..Nyquist. Q is linear (for the bells).
BiquadCoefficients design_biquad(FilterSpec::Type type, double frequency, double q, double gain_db,
                                 double fs);
// The filter's gain (not in dB) at `frequency`, from its coefficients. For tests and for drawing
// the curve.
double biquad_magnitude(const BiquadCoefficients& c, double frequency, double fs);
// The whole equalizer's gain in dB at `frequency` for the given band gains (preamp not included).
double equalizer_response_db(const Bands& gains_db, double frequency, double fs);

class Chain {
 public:
  explicit Chain(double sample_rate = 44100);
  void set_sample_rate(double sample_rate);  // also clears the filters' memory
  // Tells it what the settings are now; it glides there. Call whenever they change (any thread that
  // is not processing; guard it yourself).
  void configure(const Settings& s);
  void reset();  // forget the sound it was holding (a new song, a seek)
  // `frames` stereo frames, interleaved L R L R, changed in place.
  void process(float* x, int frames);
  [[nodiscard]] int latency_frames() const { return lookahead_; }
  // The sound after the clip guard and before the volume, as a mono sample for each frame, kept in
  // a ring of the last tap_size frames. The spectrum display (another thread) reads the newest
  // `count` of them (count <= tap_size); a read racing a write may see a few mixed-up samples,
  // which is harmless for a display and means nothing ever waits.
  static constexpr std::size_t tap_size = 8192;
  void latest(float* out, std::size_t count) const;
  [[nodiscard]] double gain_reduction_db() const;  // how much the clip guard is holding the sound
                                                   // down just now (0 = nothing), for a meter

 private:
  struct Band {
    double gain_db = 0, target_db = 0;
    BiquadCoefficients c;
    double z1[2] = {0, 0}, z2[2] = {0, 0};  // transposed direct form II memory per channel
  };
  void design_all();
  void glide(int frames);

  double fs_{};
  std::array<Band, band_count> bands_;
  double preamp_ = 1, preamp_target_ = 1, volume_ = 1, volume_target_ = 1, pan_ = 0,
         pan_target_ = 0, mono_ = 0, mono_target_ = 0;
  bool limiter_ = true;
  int lookahead_ = 0;
  std::vector<float> delay_;  // lookahead_ frames, interleaved
  std::size_t delay_at_ = 0;
  std::vector<double> req_ring_;  // required gain for the last lookahead_+1 samples
  std::vector<double> env_ring_;  // env history for the box average
  std::size_t ring_at_ = 0;
  double env_ = 1, env_sum_ = 0;
  std::atomic<double> last_gain_{1};  // read by the display thread
  std::array<float, tap_size> tap_{};
  std::atomic<std::size_t> tap_pos_{0};
};

}  // namespace dva
