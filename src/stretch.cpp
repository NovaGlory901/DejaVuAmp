#include "dva/stretch.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dva {

namespace {
constexpr double pi = std::numbers::pi;
}

TimeStretcher::TimeStretcher(double speed)
    : speed_(std::isfinite(speed) ? std::clamp(speed, 0.5, 2.0) : 1.0) {
  window_.resize(hop);
  for (int i = 0; i < hop; i++) {
    window_[static_cast<std::size_t>(i)] =
        static_cast<float>(0.5 - 0.5 * std::cos(pi * (i + 0.5) / hop));
  }
  tail_.assign(static_cast<std::size_t>(hop) * 2, 0.0F);
}

void TimeStretcher::push(const float* stereo, int frames) {
  if (frames <= 0) {
    return;
  }
  in_.insert(in_.end(), stereo, stereo + static_cast<std::size_t>(frames) * 2);
  produce();
}

void TimeStretcher::flush() {
  std::vector<float> silence(static_cast<std::size_t>(hop) * 2 * 4,
                             0.0F);  // enough to push the last real sound through the last pieces
  in_.insert(in_.end(), silence.begin(), silence.end());
  produce();
}

int TimeStretcher::pop(float* stereo, int frames) {
  const int n = std::min(frames, available());
  std::copy_n(out_.begin() + static_cast<std::ptrdiff_t>(out_read_) * 2,
              static_cast<std::size_t>(n) * 2, stereo);
  out_read_ += n;
  if (out_read_ > 8192) {
    out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(out_read_) * 2);
    out_read_ = 0;
  }
  return n;
}

void TimeStretcher::produce() {
  const auto at = [&](long long abs, int ch) {
    return in_[static_cast<std::size_t>(abs - in_start_) * 2 + static_cast<std::size_t>(ch)];
  };
  for (;;) {
    const auto in_frames = static_cast<long long>(in_.size() / 2);
    if (!started_) {
      if (in_frames - 0 < 2LL * hop) {
        return;  // need the first piece (nothing has been dropped yet: in_start_ is 0)
      }
      for (int i = 0; i < hop; i++) {
        out_.push_back(at(i, 0));
        out_.push_back(at(i, 1));
        const float fall = 1.0F - window_[static_cast<std::size_t>(i)];
        tail_[static_cast<std::size_t>(i) * 2] = at(hop + i, 0) * fall;
        tail_[static_cast<std::size_t>(i) * 2 + 1] = at(hop + i, 1) * fall;
      }
      chosen_ = 0;
      ideal_ = speed_ * hop;
      started_ = true;
      continue;
    }
    const auto centre = static_cast<long long>(std::llround(ideal_));
    const long long need =
        centre + search_range + 2LL * hop;  // the latest frame this piece may touch, plus one
    if (need > in_start_ + in_frames) {
      return;
    }
    // the natural continuation of the last piece: what followed its first half in the song. Find
    // the start near `centre` whose first half matches it best.
    const long long natural = chosen_ + hop;
    const long long lo = std::max(in_start_, centre - search_range);
    const long long hi = std::min(centre + search_range, in_start_ + in_frames - hop);
    long long best = std::max<long long>(centre, in_start_);
    double best_score = -1e30;
    if (hi >= lo) {
      // mono copies of what is compared (done once, so the search itself is plain arithmetic on
      // arrays)
      const int span = static_cast<int>(hi - lo) + hop;
      mono_.resize(static_cast<std::size_t>(span));
      for (int i = 0; i < span; i++) {
        mono_[static_cast<std::size_t>(i)] = 0.5F * (at(lo + i, 0) + at(lo + i, 1));
      }
      nat_.resize(hop / 2);
      double nn = 0;
      for (int k = 0; k < hop / 2; k++) {
        nat_[static_cast<std::size_t>(k)] =
            0.5F * (at(natural + 2 * k, 0) + at(natural + 2 * k, 1));
        nn += static_cast<double>(nat_[static_cast<std::size_t>(k)]) *
              nat_[static_cast<std::size_t>(k)];
      }
      for (long long s0 = lo; s0 <= hi; s0++) {
        const float* y = &mono_[static_cast<std::size_t>(s0 - lo)];
        double xy = 0;
        double yy = 0;
        for (int k = 0; k < hop / 2; k++) {
          const double b0 = y[2 * k];
          xy += nat_[static_cast<std::size_t>(k)] * b0;
          yy += b0 * b0;
        }  // every second frame: half the work, plenty for finding the lag
        const double score = xy / std::sqrt(nn * yy + 1e-9);
        if (score > best_score) {
          best_score = score;
          best = s0;
        }
      }
    }
    for (int i = 0; i < hop; i++) {
      const float rise = window_[static_cast<std::size_t>(i)];
      const float fall = 1.0F - rise;
      for (int ch = 0; ch < 2; ch++) {
        out_.push_back(tail_[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(ch)] +
                       at(best + i, ch) * rise);
        tail_[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(ch)] =
            at(best + hop + i, ch) * fall;
      }
    }
    chosen_ = best;
    ideal_ += speed_ * hop;
    const long long keep_from = std::min<long long>(
        chosen_ + hop, static_cast<long long>(std::llround(ideal_)) - search_range - 2);
    if (keep_from - in_start_ > 16384) {
      in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(keep_from - in_start_) * 2);
      in_start_ = keep_from;
    }
  }
}

}  // namespace dva
