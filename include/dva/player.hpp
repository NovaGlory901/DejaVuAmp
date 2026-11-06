// The player engine: one song at a time through the sound chain (dsp.hpp). A decoder thread fills a
// ring buffer of about a second; render(), which tests and the device wrapper both drive, takes
// from it. render() never reads a file or waits: if the decoder is behind it plays silence for that
// moment (an underrun, counted). The two sides share the ring under one lock held only to copy
// sound.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dva/core.hpp"
#include "dva/decoder.hpp"
#include "dva/dsp.hpp"
#include "dva/stretch.hpp"

namespace dva {

class Player {
 public:
  explicit Player(double sample_rate = 48000);
  ~Player();
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  Player(Player&&) = delete;
  Player& operator=(Player&&) = delete;

  struct Status {
    bool loaded = false, playing = false;
    unsigned track_serial = 0;  // goes up each time a different song starts being heard (a load, or
                                // the next song coming in)
    bool next_queued = false;
    double position = 0;  // seconds into the song, as heard (follows the speed)
    double duration = -1;  // seconds; -1 if the file does not say
    Tags tags;
    std::string path, format;
    long long underruns = 0;  // times the sound ran dry since the song was loaded
    double gain_reduction_db = 0;  // what the clip guard is holding down right now
  };

  // Opens a song and gets it ready at the start. Returns false, with a reason, if it cannot be
  // played; the song that was playing goes on.
  bool load(const std::filesystem::path& file, std::string& error, bool autoplay = true);
  // Lines up the song that follows the one playing. When the playing one is near its end (and its
  // length is known) the two are blended over the crossfade time; with no crossfade, or if the
  // length is not known, the next one just starts where the last one stopped (gapless). Opened
  // here, so the answer is immediate; a new load(), or clear_next(), forgets it.
  bool queue_next(const std::filesystem::path& file, std::string& error);
  void clear_next();
  // Skips to another song by blending: the one playing fades out from where it is heard while the
  // new one fades in from its start, over the crossfade time (shortened if either song is short).
  // Falls back to a plain load() (a hard cut) when nothing is playing, there is no crossfade, or
  // the playing song's length is not known. Like load(), returns false with a reason if the file
  // cannot be played.
  bool skip_to(const std::filesystem::path& file, std::string& error);
  void set_crossfade(double seconds);  // 0 (none) .. 12
  void play();
  void pause();
  void toggle();
  void stop();
  void seek(double seconds);
  void set_settings(
      const Settings& s);  // volume, equalizer, balance, mono, clip guard, speed: from any thread
  Status status() const;
  bool take_ended();  // true once when the song has played to its end

  // What the sound device calls: fills `frames` stereo frames (interleaved) with the next sound, or
  // silence if nothing is playing.
  void render(float* out, int frames);
  // The newest sound after the clip guard (mono), for the spectrum display.
  void latest(float* out, std::size_t count) const { chain_.latest(out, count); }
  double sample_rate() const { return rate_; }

 private:
  struct TrackInfo {
    std::string path, format;
    Tags tags;
    double duration = -1;  // seconds
    long long total = -1;  // frames at the output rate
  };
  using InfoPtr = std::shared_ptr<const TrackInfo>;
  struct Marker {
    long long abs;
    double src;
    double ratio;
    InfoPtr info;
  };  // from ring position `abs` on, the sound is song frame `src`, advancing `ratio` song frames
      // per frame heard (the speed)

  void decode_loop();
  void clear_ring_locked(bool reset_chain = true);
  std::size_t capacity() const { return capacity_; }

  const double rate_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::thread thread_;
  bool quit_ = false;

  std::unique_ptr<Decoder> dec_, pending_dec_;  // dec_ belongs to the decoder thread
  InfoPtr pending_info_;
  double active_speed_ =
      1;  // the speed the decoder thread is producing at (it changes when the stream is restarted)
  bool new_track_ = false;
  long long seek_to_ = -1;
  unsigned epoch_ = 0;  // changes whenever the decoder thread must drop what it was reading

  std::vector<float> ring_;
  std::size_t capacity_ = 0;
  long long rd_ = 0, wr_ = 0;  // absolute frame counts; the ring holds wr_ - rd_ frames
  std::deque<Marker> markers_;
  double src_at_read_ = 0,
         src_write_ =
             0;  // song frame at the read point, and of the next frame the decoder will deliver
  std::unique_ptr<Decoder>
      next_dec_;  // lined up, not yet started (claimed by the decoder thread when it is needed)
  InfoPtr next_info_;
  double crossfade_seconds_ = 0;
  std::unique_ptr<Decoder> skip_dec_;  // a skip waiting for the decoder thread: the song to blend
                                       // in, how long, and where the outgoing one is heard
  InfoPtr skip_info_;
  long long skip_len_ = 0, skip_from_ = 0;
  bool skip_ = false;
  InfoPtr shown_;  // the song being heard, as far as the render side knows
  unsigned track_serial_ = 0;
  bool eof_ = false, loaded_ = false, playing_ = false, ended_ = false, reset_chain_ = false;
  double fade_ = 0;  // 0 silent .. 1 full; follows playing_ over 15 ms so pausing does not click
  double duration_ = -1;
  Tags tags_;
  std::string path_, format_;
  long long underruns_ = 0;
  Settings pending_;
  bool settings_dirty_ = true;

  Chain chain_;
};

}  // namespace dva
