// Tests for the sound chain (dsp.hpp) with made-up signals: filters, the whole chain, the clip
// guard's guarantee and damaged input.
#include "check.hpp"
#include "dva/dsp.hpp"
#include "dva/stretch.hpp"
#include "tests.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <random>
#include <string>

using namespace dva;
using test::check;
static double db(double gain) { return 20 * std::log10(gain); }
constexpr double pi = std::numbers::pi;

// A stereo sine (left and right with their own amplitudes), as interleaved floats.
static std::vector<float> tone(double hz, double amp_l, double amp_r, int frames, double fs,
                               double phase = 0) {
  std::vector<float> x(static_cast<std::size_t>(frames) * 2);
  for (int i = 0; i < frames; i++) {
    const double s = std::sin(2 * pi * hz * i / fs + phase);
    x[static_cast<std::size_t>(i) * 2] = static_cast<float>(amp_l * s);
    x[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(amp_r * s);
  }
  return x;
}
// Peak of one channel over the last `count` frames.
static double peak_of(const std::vector<float>& x, int ch, int count) {
  double p = 0;
  for (std::size_t i = x.size() / 2 - static_cast<std::size_t>(count); i < x.size() / 2; i++) {
    p = std::max(p, static_cast<double>(std::fabs(x[i * 2 + static_cast<std::size_t>(ch)])));
  }
  return p;
}
static Settings flat_settings() {
  Settings s;
  s.volume_db = 0;
  s.limiter = false;
  return s;
}
static void run(Chain& c, std::vector<float>& x) {
  // in uneven pieces, as an audio device would hand them over
  std::size_t at = 0;
  const std::size_t frames = x.size() / 2;
  const int sizes[] = {480, 441, 1024, 17, 256};
  int k = 0;
  while (at < frames) {
    const std::size_t n =
        std::min<std::size_t>(static_cast<std::size_t>(sizes[k++ % 5]), frames - at);
    c.process(&x[at * 2], static_cast<int>(n));
    at += n;
  }
}

void dsp_tests() {
  test::section("dsp");
  for (const double fs : {44100.0, 48000.0, 96000.0}) {
    bool ok = true;
    std::string why;
    for (const double gain : {-12.0, -6.0, -1.5, 0.0, 3.0, 9.0, 12.0}) {
      const auto bell = design_biquad(FilterSpec::Type::Peaking, 1000, 1.4, gain, fs);
      if (std::fabs(db(biquad_magnitude(bell, 1000, fs)) - gain) > 1e-6) {
        ok = false;
        why = "bell centre";
      }
      if (std::fabs(db(biquad_magnitude(bell, 10, fs))) > 0.5 ||
          std::fabs(db(biquad_magnitude(bell, 20000, fs))) > 1.5) {
        ok = false;
        why = "bell far away";
      }
      const auto low = design_biquad(FilterSpec::Type::LowShelf, 60, 1.4, gain, fs);
      if (std::fabs(db(biquad_magnitude(low, 1, fs)) - gain) > 1e-3 ||
          std::fabs(db(biquad_magnitude(low, 10000, fs))) > 0.05) {
        ok = false;
        why = "low shelf";
      }
      const auto high = design_biquad(FilterSpec::Type::HighShelf, 16000, 1.4, gain, fs);
      if (std::fabs(db(biquad_magnitude(high, fs / 2, fs)) - gain) > 1e-3 ||
          std::fabs(db(biquad_magnitude(high, 200, fs))) > 0.05) {
        ok = false;
        why = "high shelf";
      }
      if (gain == 0) {
        for (const double f : {20.0, 500.0, 5000.0, 18000.0}) {
          if (std::fabs(biquad_magnitude(bell, f, fs) - 1) > 1e-9 ||
              std::fabs(biquad_magnitude(low, f, fs) - 1) > 1e-9) {
            ok = false;
            why = "zero gain is not flat";
          }
        }
      }
    }
    check(
        "filters at " + std::to_string(static_cast<int>(fs)) +
            " Hz: the gain is where it should be (bell centre, shelf ends), and zero gain is flat",
        ok, why);
  }
  {
    bool finite = true;
    for (const double fs : {8000.0, 22050.0, 192000.0}) {
      for (std::size_t i = 0; i < band_count; i++) {
        const auto s = filter_specs()[i];
        const auto c = design_biquad(s.type, s.frequency, s.q, 12, fs);
        finite = finite && std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
                 std::isfinite(c.a1) && std::isfinite(c.a2);
      }
    }
    check("filters: a band above half the sample rate (8 kHz, 22 kHz audio) stays finite", finite);
    check("equalizer: a flat curve is 0 dB everywhere, a boosted 1 kHz band shows up at 1 kHz",
          std::fabs(equalizer_response_db(Bands{}, 3000, 44100)) < 1e-9 &&
              equalizer_response_db(Bands{0, 0, 0, 0, 6, 0, 0, 0, 0, 0}, 1000, 44100) > 5.9);
  }

  const double fs = 44100;
  {
    Chain c(fs);
    c.configure(flat_settings());
    auto x = tone(1000, 0.5, 0.25, 44100, fs);
    auto original = x;
    run(c, x);
    const int l = c.latency_frames();
    double worst = 0;
    for (int i = l + 4000; i < 44100; i++) {
      for (int ch = 0; ch < 2; ch++) {
        worst = std::max(
            worst,
            static_cast<double>(std::fabs(
                x[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(ch)] -
                original[static_cast<std::size_t>(i - l) * 2 + static_cast<std::size_t>(ch)])));
      }
    }
    check(
        "a flat chain at 0 dB with no limiter passes the sound through, delayed by the look-ahead",
        worst < 1e-5,
        "delay " + std::to_string(l) + " frames, worst difference " + std::to_string(worst));
  }
  {
    Settings s = flat_settings();
    s.volume_db = -20;
    Chain c(fs);
    c.configure(s);
    auto x = tone(440, 0.8, 0.8, 22050, fs);
    run(c, x);
    check("chain: volume -20 dB is a tenth of the level (after it glides there)",
          std::fabs(peak_of(x, 0, 2000) - 0.08) < 0.002, std::to_string(peak_of(x, 0, 2000)));
  }
  {
    Settings s = flat_settings();
    s.volume_db = -60;
    Chain c(fs);
    c.configure(s);
    auto x = tone(440, 0.8, 0.8, 44100, fs);
    run(c, x);
    check("chain: volume at -60 dB is silence", peak_of(x, 0, 2000) < 1e-9);
  }
  {
    Settings s = flat_settings();
    s.preamp = -6;
    Chain c(fs);
    c.configure(s);
    auto x = tone(440, 0.5, 0.5, 22050, fs);
    run(c, x);
    check("chain: preamp -6 dB halves the level",
          std::fabs(peak_of(x, 0, 2000) - 0.5 * std::pow(10, -6.0 / 20)) < 0.002);
  }
  {
    // a boosted band: the chain's output level at the band's centre matches the response worked out
    // from the coefficients
    Settings s = flat_settings();
    s.bands[5] = 9;  // 3 kHz
    Chain c(fs);
    c.configure(s);
    auto x = tone(3000, 0.1, 0.1, 44100, fs);
    run(c, x);
    const double want = 0.1 * std::pow(10, equalizer_response_db(s.bands, 3000, fs) / 20);
    check("chain: a +9 dB band at 3 kHz boosts a 3 kHz tone by what the filter formulas say",
          std::fabs(peak_of(x, 0, 2000) - want) < 0.002 && want > 0.25,
          std::to_string(peak_of(x, 0, 2000)) + " vs " + std::to_string(want));
    Settings off = s;
    off.eq_on = false;
    Chain c2(fs);
    c2.configure(off);
    auto y = tone(3000, 0.1, 0.1, 44100, fs);
    run(c2, y);
    check("chain: with the equalizer switched off the same tone is not boosted",
          std::fabs(peak_of(y, 0, 2000) - 0.1) < 0.002);
    Settings low = flat_settings();
    low.bands[0] = -12;
    Chain c3(fs);
    c3.configure(low);
    auto z = tone(30, 0.5, 0.5, 44100, fs);
    run(c3, z);
    check("chain: a -12 dB low shelf cuts a 30 Hz tone to about a quarter",
          peak_of(z, 0, 4000) < 0.5 * 0.3 && peak_of(z, 0, 4000) > 0.5 * 0.2,
          std::to_string(peak_of(z, 0, 4000)));
  }
  {
    Settings s = flat_settings();
    s.balance = -100;
    Chain c(fs);
    c.configure(s);
    auto x = tone(440, 0.4, 0.3, 22050, fs);
    run(c, x);
    check(
        "chain: balance all the way left puts everything on the left (L + R), nothing on the right",
        peak_of(x, 1, 2000) < 1e-6 && std::fabs(peak_of(x, 0, 2000) - 0.7) < 0.003);
    s.balance = 100;
    Chain d(fs);
    d.configure(s);
    auto y = tone(440, 0.4, 0.3, 22050, fs);
    run(d, y);
    check("chain: all the way right puts everything on the right",
          peak_of(y, 0, 2000) < 1e-6 && std::fabs(peak_of(y, 1, 2000) - 0.7) < 0.003);
    s.balance = 0;
    Chain e(fs);
    e.configure(s);
    auto z = tone(440, 0.4, 0.3, 22050, fs);
    run(e, z);
    check("chain: balance in the middle changes nothing",
          std::fabs(peak_of(z, 0, 2000) - 0.4) < 0.002 &&
              std::fabs(peak_of(z, 1, 2000) - 0.3) < 0.002);
  }
  {
    Settings s = flat_settings();
    s.mono = true;
    Chain c(fs);
    c.configure(s);
    auto x = tone(440, 0.8, 0.0, 22050, fs);
    run(c, x);
    check("chain: mono folds left-only sound to both sides at half level",
          std::fabs(peak_of(x, 0, 2000) - 0.4) < 0.003 &&
              std::fabs(peak_of(x, 1, 2000) - 0.4) < 0.003);
  }

  const double ceiling = std::pow(10.0, -2.0 / 20);
  {
    Settings s = flat_settings();
    s.limiter = true;
    Chain c(fs);
    c.configure(s);
    auto x = tone(200, 3.0, 3.0, 44100, fs);
    run(c, x);
    double worst = 0;
    for (const float v : x) {
      worst = std::max(worst, static_cast<double>(std::fabs(v)));
    }
    check("limiter: a sine three times too loud never goes above the -2 dB ceiling",
          worst <= ceiling + 1e-6, std::to_string(worst) + " vs " + std::to_string(ceiling));
    check("limiter: a loud sine is held close under the ceiling, not squashed far below it",
          peak_of(x, 0, 4000) > ceiling * 0.95);
  }
  {
    Settings s = flat_settings();
    s.limiter = true;
    Chain c(fs);
    c.configure(s);
    auto x = tone(440, 0.5, 0.5, 44100, fs);
    run(c, x);
    check("limiter: a quiet sound passes untouched",
          std::fabs(peak_of(x, 0, 4000) - 0.5) < 0.001 && c.gain_reduction_db() < 0.01);
  }
  {
    Settings s = flat_settings();
    s.limiter = true;
    Chain c(fs);
    c.configure(s);
    // a quiet tone, a loud burst, a quiet tone again: the guard lets go slowly (about 200 ms) and
    // then the sound is back to normal
    std::vector<float> x = tone(440, 0.3, 0.3, 22050, fs);
    auto loud = tone(440, 2.0, 2.0, 4410, fs);
    auto quiet = tone(440, 0.3, 0.3, 44100 * 2, fs);
    x.insert(x.end(), loud.begin(), loud.end());
    x.insert(x.end(), quiet.begin(), quiet.end());
    run(c, x);
    double worst = 0;
    for (const float v : x) {
      worst = std::max(worst, static_cast<double>(std::fabs(v)));
    }
    check(
        "limiter: after a loud burst the sound comes back to full level (release), and nothing in "
        "between was above the ceiling",
        worst <= ceiling + 1e-6 && std::fabs(peak_of(x, 0, 4000) - 0.3) < 0.003,
        std::to_string(peak_of(x, 0, 4000)));
  }
  {
    const Settings s = flat_settings();
    Chain c(fs);
    c.configure(s);
    auto x = tone(200, 2.0, 2.0, 22050, fs);
    run(c, x);
    check(
        "limiter: switched off, loud sound is not touched (the output device clips it, as it would "
        "in the browser)",
        peak_of(x, 0, 2000) > 1.99);
  }
  {
    // an impulse far above the ceiling after silence: the reduction has to be in place before it
    // arrives
    Settings s = flat_settings();
    s.limiter = true;
    Chain c(fs);
    c.configure(s);
    std::vector<float> x(2 * 20000, 0.0F);
    x[2 * 10000] = 5.0F;
    x[2 * 10000 + 1] = -5.0F;
    run(c, x);
    double worst = 0;
    for (const float v : x) {
      worst = std::max(worst, static_cast<double>(std::fabs(v)));
    }
    check("limiter: a single huge click is caught too", worst <= ceiling + 1e-6 && worst > 0.5,
          std::to_string(worst));
  }

  {
    std::mt19937 gen(99);
    std::uniform_real_distribution<double> u(-1, 1);
    bool finite = true;
    bool inside = true;
    for (int round = 0; round < 40; round++) {
      Settings s;
      s.volume_db = u(gen) * 60;
      s.balance = static_cast<int>(u(gen) * 100);
      s.mono = gen() % 2;
      s.eq_on = gen() % 4 != 0;
      s.preamp = u(gen) * 12;
      s.limiter = true;
      for (double& b : s.bands) {
        b = u(gen) * 12;
      }
      const double rate = round % 3 == 0 ? 8000 : round % 3 == 1 ? 48000 : 192000;
      Chain c(rate);
      c.configure(s);
      std::vector<float> x(2 * 6000);
      for (float& v : x) {
        v = static_cast<float>(u(gen) * (round % 4 == 0 ? 100 : 1.5));
      }
      if (round % 5 == 0) {
        x[100] = NAN;
        x[201] = INFINITY;
        x[300] = -INFINITY;
      }
      run(c, x);
      for (const float v : x) {
        if (!std::isfinite(v)) {
          finite = false;
        }
        if (std::fabs(v) > ceiling + 1e-5) {
          inside = false;
        }
      }
    }
    check(
        "chain: random settings, sample rates and loud or damaged (NaN, infinite) input always "
        "give finite sound inside the ceiling",
        finite && inside);
  }
  {
    Chain c(fs);
    Settings s = flat_settings();
    s.bands[3] = 12;
    c.configure(s);
    auto x = tone(600, 0.2, 0.2, 8000, fs);
    run(c, x);
    c.reset();
    std::vector<float> silence(2 * 4000, 0.0F);
    run(c, silence);
    double p = 0;
    for (const float v : silence) {
      p = std::max(p, static_cast<double>(std::fabs(v)));
    }
    check("chain: after reset (a new song, a seek) nothing of the old sound is left in it", p == 0);
    float newest[64];
    Chain d(fs);
    d.configure(flat_settings());
    auto y = tone(1000, 0.5, 0.5, 4000, fs);
    run(d, y);
    d.latest(newest, 64);
    double m = 0;
    for (const float v : newest) {
      m = std::max(m, static_cast<double>(std::fabs(v)));
    }
    check("chain: the tap for the spectrum display has the latest sound", m > 0.3);
  }
  for (const double speed : {0.5, 0.75, 1.25, 1.5, 2.0}) {
    TimeStretcher st(speed);
    const int in_frames = 48000 * 4;
    auto x = tone(440, 0.5, 0.5, in_frames, 48000);
    std::vector<float> out;
    std::vector<float> buf(2 * 600);
    for (int at = 0; at < in_frames; at += 480) {
      st.push(&x[static_cast<std::size_t>(at) * 2], std::min(480, in_frames - at));
      for (int n = 0; (n = st.pop(buf.data(), 600)) > 0;) {
        out.insert(out.end(), buf.begin(), buf.begin() + n * 2);
      }
    }
    st.flush();
    for (int n = 0; (n = st.pop(buf.data(), 600)) > 0;) {
      out.insert(out.end(), buf.begin(), buf.begin() + n * 2);
    }
    const double out_seconds = static_cast<double>(out.size() / 2) / 48000;
    const double want = 4.0 / speed;
    // the pitch, from rising zero crossings in the middle; the level, from the peaks of 20 ms
    // blocks; the smoothness, from the biggest step between neighbours
    const std::size_t a = out.size() / 2 / 4;
    const std::size_t b = out.size() / 2 * 3 / 4;
    int crossings = 0;
    double worst_step = 0;
    double min_peak = 1e9;
    double max_peak = 0;
    for (std::size_t i = a + 1; i < b; i++) {
      if (out[(i - 1) * 2] < 0 && out[i * 2] >= 0) {
        crossings++;
      }
      worst_step =
          std::max(worst_step, static_cast<double>(std::fabs(out[i * 2] - out[(i - 1) * 2])));
    }
    for (std::size_t i = a; i + 960 < b; i += 960) {
      double p = 0;
      for (std::size_t k = i; k < i + 960; k++) {
        p = std::max(p, static_cast<double>(std::fabs(out[k * 2])));
      }
      min_peak = std::min(min_peak, p);
      max_peak = std::max(max_peak, p);
    }
    const double hz = crossings * 48000.0 / static_cast<double>(b - a);
    const std::string label = "stretch at " + std::to_string(speed).substr(0, 4) + "x";
    check(label + ": the song is " + std::to_string(want).substr(0, 5) + " s long",
          std::fabs(out_seconds - want) < 0.22, std::to_string(out_seconds).substr(0, 5) + " s");
    check(label + ": the pitch stays at 440 Hz", std::fabs(hz - 440) < 6,
          std::to_string(hz).substr(0, 6) + " Hz");
    check(label + ": the level stays even and there are no clicks",
          min_peak > 0.45 && max_peak < 0.52 && worst_step < 0.1,
          "level " + std::to_string(min_peak).substr(0, 5) + " .. " +
              std::to_string(max_peak).substr(0, 5) + ", step " + std::to_string(worst_step));
  }
  {
    // music-like sound: low notes, a high one and some noise; the stretch must stay finite, inside
    // the input's range, and keep the level
    std::mt19937 gen(3);
    std::uniform_real_distribution<double> u(-1, 1);
    const int in_frames = 48000 * 3;
    std::vector<float> x(static_cast<std::size_t>(in_frames) * 2);
    for (int i = 0; i < in_frames; i++) {
      const double t = i / 48000.0;
      const auto v = static_cast<float>(0.3 * std::sin(2 * pi * 110 * t) +
                                        0.2 * std::sin(2 * pi * 330 * t + 1) +
                                        0.1 * std::sin(2 * pi * 3000 * t) + 0.05 * u(gen));
      x[static_cast<std::size_t>(i) * 2] = v;
      x[static_cast<std::size_t>(i) * 2 + 1] = -v;
    }
    bool good = true;
    for (const double speed : {0.5, 1.3, 2.0}) {
      TimeStretcher st(speed);
      st.push(x.data(), in_frames);
      st.flush();
      std::vector<float> out(static_cast<std::size_t>(st.available()) * 2);
      st.pop(out.data(), st.available());
      double peak_in = 0;
      double peak_out = 0;
      for (const float v : x) {
        peak_in = std::max(peak_in, static_cast<double>(std::fabs(v)));
      }
      for (const float v : out) {
        if (!std::isfinite(v)) {
          good = false;
        }
        peak_out = std::max(peak_out, static_cast<double>(std::fabs(v)));
      }
      if (peak_out > peak_in * 1.15 || out.size() < 2000) {
        good = false;
      }
    }
    check("stretch: complex sound stays finite and never gets louder than what went in", good);
    TimeStretcher tiny(1.5);
    float one[2] = {0.5F, 0.5F};
    tiny.push(one, 1);
    check(
        "stretch: a single frame gives nothing yet (it waits for more) and flushing it does not "
        "crash",
        tiny.available() == 0 && (tiny.flush(), tiny.available() >= 0));
    const TimeStretcher odd(NAN);
    check("stretch: an invalid speed falls back to normal", odd.speed() == 1.0 &&
                                                                TimeStretcher(9).speed() == 2.0 &&
                                                                TimeStretcher(0.01).speed() == 0.5);
  }
}
