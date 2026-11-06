// Turns a song file into stereo 32-bit float at the requested sample rate. MP3, FLAC and WAV use
// miniaudio's decoders, Ogg Vorbis stb_vorbis, M4A/Opus/WMA/AAC/Matroska ffmpeg when built with it
// (see src/decoder_ffmpeg.hpp). The kind of file comes from its first bytes, not its name. Files
// are untrusted: a damaged one gives an error or a short song, never a crash.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "dva/tags.hpp"

namespace dva {

class Decoder {
 public:
  Decoder() = default;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;
  virtual ~Decoder() = default;
  // Up to `frames` stereo frames (L R L R ...) into `out`. Returns how many; 0 means the song is
  // over (or cannot go on).
  virtual long long read(float* out, long long frames) = 0;
  // Moves to `frame` (counted at the output sample rate). False if the file cannot do it.
  virtual bool seek(long long frame) = 0;
  [[nodiscard]] virtual long long total_frames()
      const = 0;  // at the output rate; -1 if the file does not say
  [[nodiscard]] virtual double sample_rate() const = 0;  // the output rate
  [[nodiscard]] virtual Tags tags() const { return {}; }
  [[nodiscard]] virtual std::string format() const = 0;  // "MP3", "FLAC", "WAV", "Ogg Vorbis"
};

struct OpenResult {
  std::unique_ptr<Decoder> decoder;
  std::string error;  // a sentence for the person, when decoder is null
};

OpenResult open_decoder(const std::filesystem::path& file, double output_rate);

// Does the file name look like a song this program plays? (mp3 flac wav ogg oga, any case; with
// ffmpeg also m4a mp4 aac opus wma mka webm)
bool looks_like_song(const std::filesystem::path& file);
// True if this build can play M4A/AAC/Opus/WMA (it was built with ffmpeg's libraries).
bool ffmpeg_supported();

}  // namespace dva
