// The sound device: asks the Player for sound over and over, through miniaudio
// (PulseAudio/PipeWire/ALSA on Linux, WASAPI on Windows), opened at the player's sample rate.
#pragma once

#include <memory>
#include <string>

#include "dva/player.hpp"

namespace dva {

class AudioOutput {
 public:
  AudioOutput();
  ~AudioOutput();
  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;
  AudioOutput(AudioOutput&&) = delete;
  AudioOutput& operator=(AudioOutput&&) = delete;
  // Opens the default output (or one whose name contains `hint`) and starts it. False, with a
  // reason, if the computer has no usable sound output.
  bool start(Player& player, const std::string& hint, std::string& error);
  void stop();
  [[nodiscard]] std::string device_name() const;

  struct Impl;  // (public only so the device's callback can see it)

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace dva
