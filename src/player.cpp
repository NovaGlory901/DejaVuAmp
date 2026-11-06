#include "dva/player.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dva {

namespace {
constexpr long long chunk_frames = 2048;
constexpr double ring_seconds = 1.0, fade_seconds = 0.015;
}  // namespace

Player::Player(double sample_rate) : rate_(sample_rate), chain_(sample_rate) {
  capacity_ = static_cast<std::size_t>(rate_ * ring_seconds);
  ring_.assign(capacity_ * 2, 0.0F);
  thread_ = std::thread([this] { decode_loop(); });
}

Player::~Player() {
  {
    const std::scoped_lock lk(m_);
    quit_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Player::clear_ring_locked(bool reset_chain) {
  rd_ = wr_ = 0;
  markers_.clear();
  if (reset_chain) {
    reset_chain_ = true;
  }
}

bool Player::load(const std::filesystem::path& file, std::string& error, bool autoplay) {
  OpenResult r = open_decoder(
      file, rate_);  // opened here, not in the decoder thread, so the answer can be given at once
  if (!r.decoder) {
    error = r.error;
    return false;
  }
  const auto info = std::make_shared<TrackInfo>();
  info->tags = r.decoder->tags();
  if (!info->tags.any()) {
    info->tags = read_tags(file);
  }
  info->total = r.decoder->total_frames();
  info->duration = info->total >= 0 ? static_cast<double>(info->total) / rate_ : -1;
  info->path = file.string();
  info->format = r.decoder->format();
  {
    const std::scoped_lock lk(m_);
    pending_dec_ = std::move(r.decoder);
    pending_info_ = info;
    skip_ = false;
    skip_dec_.reset();
    skip_info_.reset();
    next_dec_.reset();
    next_info_.reset();
    active_speed_ = pending_.speed;
    new_track_ = true;
    epoch_++;
    seek_to_ = -1;
    clear_ring_locked();
    src_at_read_ = src_write_ = 0;
    eof_ = false;
    ended_ = false;
    loaded_ = true;
    playing_ = autoplay;
    fade_ = 0;
    shown_ = info;
    track_serial_++;
    duration_ = info->duration;
    tags_ = info->tags;
    path_ = info->path;
    format_ = info->format;
    underruns_ = 0;
  }
  cv_.notify_all();
  return true;
}

bool Player::queue_next(const std::filesystem::path& file, std::string& error) {
  OpenResult r = open_decoder(file, rate_);
  if (!r.decoder) {
    error = r.error;
    return false;
  }
  const auto info = std::make_shared<TrackInfo>();
  info->tags = r.decoder->tags();
  if (!info->tags.any()) {
    info->tags = read_tags(file);
  }
  info->total = r.decoder->total_frames();
  info->duration = info->total >= 0 ? static_cast<double>(info->total) / rate_ : -1;
  info->path = file.string();
  info->format = r.decoder->format();
  {
    const std::scoped_lock lk(m_);
    if (!loaded_) {
      error = "Nothing is playing to follow.";
      return false;
    }
    next_dec_ = std::move(r.decoder);
    next_info_ = info;
  }
  cv_.notify_all();
  return true;
}

void Player::clear_next() {
  const std::scoped_lock lk(m_);
  next_dec_.reset();
  next_info_.reset();
}

bool Player::skip_to(const std::filesystem::path& file, std::string& error) {
  OpenResult r = open_decoder(file, rate_);
  if (!r.decoder) {
    error = r.error;
    return false;
  }
  const auto info = std::make_shared<TrackInfo>();
  info->tags = r.decoder->tags();
  if (!info->tags.any()) {
    info->tags = read_tags(file);
  }
  info->total = r.decoder->total_frames();
  info->duration = info->total >= 0 ? static_cast<double>(info->total) / rate_ : -1;
  info->path = file.string();
  info->format = r.decoder->format();
  {
    std::unique_lock<std::mutex> lk(m_);
    auto len = static_cast<long long>(std::llround(crossfade_seconds_ * rate_));
    const auto from = static_cast<long long>(std::llround(src_at_read_));
    const long long total = shown_ ? shown_->total : -1;
    if (loaded_ && playing_ && !ended_ && !eof_ && !new_track_ && total > 0 && from < total &&
        info->total != 0 && len > 0) {
      len =
          std::min({len, total - from,
                    total / 2});  // never longer than what is left of the heard song, or half of it
      if (info->total > 0) {
        len = std::min(len, info->total / 2);
      }
      if (len >= 64) {
        skip_dec_ = std::move(r.decoder);
        skip_info_ = info;
        skip_len_ = len;
        skip_from_ = from;
        skip_ = true;
        next_dec_.reset();
        next_info_.reset();
        epoch_++;
        clear_ring_locked(false);  // what was waiting in the ring is up to a second ahead of what
                                   // is heard: drop it
        src_write_ = static_cast<double>(from);
        eof_ = false;
        lk.unlock();
        cv_.notify_all();
        return true;
      }
    }
  }
  return load(file, error, true);
}

void Player::set_crossfade(double seconds) {
  const std::scoped_lock lk(m_);
  crossfade_seconds_ = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 12.0) : 0.0;
}

void Player::play() {
  const std::scoped_lock lk(m_);
  if (loaded_) {
    playing_ = true;
    if (ended_) {
      ended_ = false;
    }
  }
}
void Player::pause() {
  const std::scoped_lock lk(m_);
  playing_ = false;
}
void Player::toggle() {
  const std::scoped_lock lk(m_);
  if (loaded_) {
    playing_ = !playing_;
  }
}

void Player::seek(double seconds) {
  {
    const std::scoped_lock lk(m_);
    if (!loaded_) {
      return;
    }
    double s = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    if (duration_ >= 0) {
      s = std::min(s, duration_);
    }
    seek_to_ = static_cast<long long>(std::llround(s * rate_));
    epoch_++;
    clear_ring_locked();
    src_at_read_ = src_write_ = static_cast<double>(seek_to_);
    eof_ = false;
    ended_ = false;
  }
  cv_.notify_all();
}

void Player::stop() {
  {
    const std::scoped_lock lk(m_);
    playing_ = false;
  }
  seek(0);
}

void Player::set_settings(const Settings& s) {
  {
    const std::scoped_lock lk(m_);
    pending_ = s;
    settings_dirty_ = true;
    if (s.speed != active_speed_) {
      active_speed_ = s.speed;
      if (loaded_) {
        // a new speed: start the stream again from where it is now, so the change is heard at once
        // instead of after the second of sound already waiting
        seek_to_ = static_cast<long long>(std::llround(src_at_read_));
        epoch_++;
        clear_ring_locked(false);
        src_write_ = static_cast<double>(seek_to_);
        eof_ = false;
        fade_ = 0;  // and fade back in, so the join does not click
      }
    }
  }
  cv_.notify_all();
}

Player::Status Player::status() const {
  const std::scoped_lock lk(m_);
  Status st;
  st.loaded = loaded_;
  st.playing = playing_;
  st.track_serial = track_serial_;
  st.next_queued = next_dec_ != nullptr;
  st.position = src_at_read_ / rate_;
  st.duration = duration_;
  st.tags = tags_;
  st.path = path_;
  st.format = format_;
  st.underruns = underruns_;
  st.gain_reduction_db = chain_.gain_reduction_db();
  return st;
}

bool Player::take_ended() {
  const std::scoped_lock lk(m_);
  const bool e = ended_;
  ended_ = false;
  return e;
}

void Player::render(float* out, int frames) {
  std::unique_lock<std::mutex> lk(m_);
  if (settings_dirty_) {
    chain_.configure(pending_);
    settings_dirty_ = false;
  }
  if (reset_chain_) {
    chain_.reset();
    reset_chain_ = false;
  }
  const double target = (playing_ && loaded_) ? 1.0 : 0.0;
  if (target == 0.0 && fade_ <= 0.0) {  // paused and faded out: silence, nothing consumed
    lk.unlock();
    std::memset(out, 0, static_cast<std::size_t>(frames) * 2 * sizeof(float));
    return;
  }
  const long long avail = wr_ - rd_;
  const int n = static_cast<int>(std::min<long long>(avail, frames));
  for (int i = 0; i < n; i++) {
    const std::size_t at =
        static_cast<std::size_t>((rd_ + i) % static_cast<long long>(capacity_)) * 2;
    out[static_cast<std::size_t>(i) * 2] = ring_[at];
    out[static_cast<std::size_t>(i) * 2 + 1] = ring_[at + 1];
  }
  rd_ += n;
  while (markers_.size() > 1 && markers_[1].abs <= rd_) {
    markers_.pop_front();
  }
  if (!markers_.empty()) {
    const Marker& m = markers_.front();
    src_at_read_ = m.src + static_cast<double>(rd_ - m.abs) * m.ratio;
    if (m.info && m.info != shown_) {
      shown_ = m.info;
      duration_ = m.info->duration;
      tags_ = m.info->tags;
      path_ = m.info->path;
      format_ = m.info->format;
      track_serial_++;
      underruns_ = 0;
    }
  }
  if (n < frames) {
    std::memset(out + static_cast<std::size_t>(n) * 2, 0,
                static_cast<std::size_t>(frames - n) * 2 * sizeof(float));
    if (eof_ && rd_ >= wr_) {
      playing_ = false;
      ended_ = true;
    } else if (loaded_ && playing_) {
      underruns_++;
    }
  }
  const double step = 1.0 / (fade_seconds * rate_);
  double f = fade_;
  for (int i = 0; i < frames; i++) {
    f = target > f ? std::min(target, f + step) : std::max(target, f - step);
    out[static_cast<std::size_t>(i) * 2] =
        static_cast<float>(out[static_cast<std::size_t>(i) * 2] * f);
    out[static_cast<std::size_t>(i) * 2 + 1] =
        static_cast<float>(out[static_cast<std::size_t>(i) * 2 + 1] * f);
  }
  fade_ = f;
  lk.unlock();
  cv_.notify_one();
  chain_.process(out, frames);
}

void Player::decode_loop() {
  std::vector<float> buf(static_cast<std::size_t>(chunk_frames) * 2);
  std::vector<float> buf_b(static_cast<std::size_t>(chunk_frames) * 2);
  std::unique_ptr<TimeStretcher> stretch;
  double ratio = 1;
  double out_src =
      0;  // the speed in use, and the song frame the next frame given to the ring comes from
  bool decoder_done = false;
  InfoPtr info;
  long long a_read = 0;
  std::unique_ptr<Decoder> b;
  InfoPtr b_info;
  long long b_read = 0;
  long long fade_pos = 0;
  long long fade_len = 0;
  bool fading = false;
  InfoPtr stream_info;  // the song the frames now going into the ring belong to

  std::unique_lock<std::mutex> lk(m_);
  auto end_fade = [&] {  // the blend is over (or cut short): the incoming song is now the song
    dec_ = std::move(b);
    info = b_info;
    a_read = b_read;
    fading = false;
    b.reset();
  };
  const auto restart =
      [&](double from_frame) {  // the stream starts again (a new song, a seek, a new speed)
        if (fading) {
          end_fade();  // the person is hearing the incoming song: that is the one that is moved
        }
        stream_info = info;
        ratio = active_speed_;
        out_src = from_frame;
        a_read = static_cast<long long>(from_frame);
        stretch = ratio == 1 ? nullptr : std::make_unique<TimeStretcher>(ratio);
        decoder_done = false;
      };
  // Gets up to `n` source frames (n <= chunk_frames) into `out`, doing the crossfade and the
  // hand-over to the next song. Called with the lock held; it lets go of it while it reads. Returns
  // frames (0: the stream is over). `chunk_info`/`chunk_src`: which song they belong to and where
  // they start in it.
  const auto read_source = [&](float* out, long long n, InfoPtr& chunk_info, double& chunk_src,
                               unsigned epoch, bool& stale) -> long long {
    stale = false;
    for (;;) {
      if (!fading && next_dec_ && crossfade_seconds_ > 0 && info && info->total > 0 && next_info_ &&
          next_info_->total != 0) {
        auto len = static_cast<long long>(std::llround(crossfade_seconds_ * rate_));
        len = std::min(len, info->total / 2);
        if (next_info_->total > 0) {
          len = std::min(len, next_info_->total / 2);
        }
        if (len >= 64 && a_read >= info->total - len) {
          b = std::move(next_dec_);
          b_info = next_info_;
          next_info_.reset();
          b_read = 0;
          fade_pos = 0;
          fade_len = len;
          fading = true;
        }
      }
      long long want = n;
      if (fading) {
        want = std::min(want, fade_len - fade_pos);
      } else if (next_dec_ && crossfade_seconds_ > 0 && info && info->total > 0 && next_info_ &&
                 next_info_->total != 0) {
        long long len = std::min(static_cast<long long>(std::llround(crossfade_seconds_ * rate_)),
                                 info->total / 2);
        if (next_info_->total > 0) {
          len = std::min(len, next_info_->total / 2);
        }
        const long long until_fade = info->total - len - a_read;
        if (len >= 64 && until_fade > 0) {
          want = std::min(want, until_fade);  // stop exactly where the blend begins
        }
      }
      if (want <= 0) {
        want = 1;
      }
      Decoder* a = dec_.get();
      Decoder* bd = fading ? b.get() : nullptr;
      lk.unlock();
      const long long got_a = a->read(out, want);
      long long got_b = 0;
      if (bd) {
        got_b = bd->read(buf_b.data(), want);
        for (long long i = got_b; i < want; i++) {
          buf_b[static_cast<std::size_t>(i) * 2] = 0;
          buf_b[static_cast<std::size_t>(i) * 2 + 1] = 0;
        }
      }
      lk.lock();
      if (epoch != epoch_ || new_track_ || seek_to_ >= 0) {
        stale = true;
        return 0;
      }
      if (!fading) {
        if (got_a > 0) {
          chunk_info = info;
          chunk_src = static_cast<double>(a_read);
          a_read += got_a;
          return got_a;
        }
        // the song is over: hand over to the next one at once, if there is one (gapless)
        if (next_dec_) {
          dec_ = std::move(next_dec_);
          info = next_info_;
          next_info_.reset();
          a_read = 0;
          continue;
        }
        return 0;
      }
      // blending: the outgoing song fades out, the incoming one fades in (equal power, so the
      // loudness stays even)
      for (long long i = 0; i < want; i++) {
        if (i >= got_a) {
          out[static_cast<std::size_t>(i) * 2] = 0;
          out[static_cast<std::size_t>(i) * 2 + 1] = 0;
        }
        const double t = static_cast<double>(fade_pos + i + 1) / static_cast<double>(fade_len);
        const auto ga = static_cast<float>(std::cos(t * 1.57079632679489661923));
        const auto gb = static_cast<float>(std::sin(t * 1.57079632679489661923));
        out[static_cast<std::size_t>(i) * 2] =
            out[static_cast<std::size_t>(i) * 2] * ga + buf_b[static_cast<std::size_t>(i) * 2] * gb;
        out[static_cast<std::size_t>(i) * 2 + 1] = out[static_cast<std::size_t>(i) * 2 + 1] * ga +
                                                   buf_b[static_cast<std::size_t>(i) * 2 + 1] * gb;
      }
      a_read += got_a;
      chunk_info = b_info;
      chunk_src = static_cast<double>(b_read);
      b_read += got_b;
      fade_pos += want;
      const bool done = fade_pos >= fade_len || (got_b <= 0 && got_a <= 0);
      if (done) {
        end_fade();
      }
      return want;
    }
  };

  for (;;) {
    cv_.wait(lk, [&] {
      return quit_ || new_track_ || skip_ || seek_to_ >= 0 ||
             (dec_ && !eof_ && static_cast<long long>(capacity_) - (wr_ - rd_) >= chunk_frames);
    });
    if (quit_) {
      return;
    }
    if (new_track_) {
      if (fading) {
        b.reset();
        fading = false;
      }
      dec_ = std::move(pending_dec_);
      info = pending_info_;
      new_track_ = false;
      restart(0);
      stream_info = info;
      continue;
    }
    if (skip_) {
      std::unique_ptr<Decoder> incoming = std::move(skip_dec_);
      const InfoPtr incoming_info = skip_info_;
      const long long len = skip_len_;
      const long long from = skip_from_;
      skip_ = false;
      const unsigned epoch = epoch_;
      if (fading) {
        end_fade();  // a blend was already going: what is heard is the incoming song of that one
      }
      lk.unlock();
      const bool ok = dec_ && dec_->seek(from);  // the outgoing song carries on from where it is
                                                 // heard, not from where the decoder was
      lk.lock();
      if (epoch != epoch_) {
        continue;  // another change came in meanwhile: that one wins
      }
      if (ok) {
        restart(static_cast<double>(from));
        b = std::move(incoming);
        b_info = incoming_info;
        b_read = 0;
        fade_pos = 0;
        fade_len = len;
        fading = true;
      } else {  // cannot seek the heard song: cut to the new one
        dec_ = std::move(incoming);
        info = incoming_info;
        restart(0);
        stream_info = info;
      }
      continue;
    }
    if (seek_to_ >= 0) {
      const long long frame = seek_to_;
      seek_to_ = -1;
      const unsigned epoch = epoch_;
      if (fading) {
        end_fade();
      }
      lk.unlock();
      const bool ok = dec_ && dec_->seek(frame);
      lk.lock();
      if (epoch != epoch_) {
        continue;  // another change came in while seeking: that one wins
      }
      restart(static_cast<double>(frame));
      if (!ok) {
        eof_ = true;
      }
      continue;
    }
    const unsigned epoch = epoch_;
    if (ratio == 1) {
      InfoPtr chunk_info;
      double chunk_src = 0;
      bool stale = false;
      const long long got =
          read_source(buf.data(), chunk_frames, chunk_info, chunk_src, epoch, stale);
      if (stale) {
        continue;
      }
      if (got <= 0) {
        eof_ = true;
        continue;
      }
      markers_.push_back({wr_, chunk_src, 1.0, chunk_info});
      for (long long i = 0; i < got; i++) {
        const std::size_t at =
            static_cast<std::size_t>((wr_ + i) % static_cast<long long>(capacity_)) * 2;
        ring_[at] = buf[static_cast<std::size_t>(i) * 2];
        ring_[at + 1] = buf[static_cast<std::size_t>(i) * 2 + 1];
      }
      wr_ += got;
      out_src = chunk_src + static_cast<double>(got);
      src_write_ = out_src;
      continue;
    }
    if (stretch->available() < chunk_frames && !decoder_done) {
      InfoPtr chunk_info;
      double chunk_src = 0;
      bool stale = false;
      const long long got =
          read_source(buf.data(), chunk_frames, chunk_info, chunk_src, epoch, stale);
      if (stale) {
        continue;
      }
      if (got > 0) {
        if (chunk_info != stream_info) {
          stream_info = chunk_info;
          out_src = chunk_src;
        }  // (the stretcher holds a little sound back, so this is a moment early)
        stretch->push(buf.data(), static_cast<int>(got));
      } else {
        stretch->flush();
        decoder_done = true;
      }
      continue;
    }
    const int n = stretch->pop(buf.data(), static_cast<int>(chunk_frames));
    if (n <= 0) {
      if (decoder_done) {
        eof_ = true;
      }
      continue;
    }
    markers_.push_back({wr_, out_src, ratio, stream_info});
    for (int i = 0; i < n; i++) {
      const std::size_t at =
          static_cast<std::size_t>((wr_ + i) % static_cast<long long>(capacity_)) * 2;
      ring_[at] = buf[static_cast<std::size_t>(i) * 2];
      ring_[at + 1] = buf[static_cast<std::size_t>(i) * 2 + 1];
    }
    wr_ += n;
    out_src += n * ratio;
    src_write_ = out_src;
  }
}

}  // namespace dva
