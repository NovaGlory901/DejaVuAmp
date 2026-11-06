// Plays faster or slower without changing the pitch, by WSOLA (waveform similarity overlap-add):
// overlapping 2048-frame pieces are taken every `speed * 1024` frames but laid down every 1024,
// each nudged (up to about 5 ms) to line up with the end of the one before so the joins do not
// warble. Streaming: push what the decoder gives, pop what the player needs. Interleaved stereo
// float.
#pragma once

#include <cstddef>
#include <vector>

namespace dva {

class TimeStretcher {
 public:
  static constexpr int hop = 1024;  // frames laid down per piece
  static constexpr int search_range = 256;  // how far (frames) a piece may be moved either way

  // speed 0.5 .. 2 (anything else is brought into that range); 1 is allowed but the player does not
  // use the stretcher then
  explicit TimeStretcher(double speed);

  void push(const float* stereo, int frames);  // more of the song
  void flush();  // the song is over: let what is still held come out (padded with silence)
  [[nodiscard]] int available() const { return static_cast<int>(out_.size() / 2) - out_read_; }
  int pop(float* stereo,
          int frames);  // up to `frames`; returns how many (fewer if not enough is ready)
  [[nodiscard]] double speed() const { return speed_; }

 private:
  void produce();

  double speed_;
  std::vector<float>
      window_;  // the rising half of a Hann window, hop frames long (falling = 1 - rising)
  std::vector<float> in_;  // input still needed, interleaved
  long long in_start_ = 0;  // absolute frame index of in_[0]
  double ideal_ = 0;  // where the next piece should start (absolute input frame), before nudging
  long long chosen_ = 0;  // where the last piece actually started
  bool started_ = false;
  std::vector<float> tail_;  // the faded-out second half of the last piece, hop frames, interleaved
  std::vector<float> mono_, nat_;  // scratch for the search
  std::vector<float> out_;  // output not yet popped
  int out_read_ = 0;  // frames of out_ already popped (the buffer is compacted now and then)
};

}  // namespace dva
