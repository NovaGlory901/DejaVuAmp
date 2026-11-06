#include "decoder_ffmpeg.hpp"

#include "fh/pathutf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace dva {

namespace {

constexpr int max_consecutive_errors = 2000;  // a stream that keeps failing is treated as over

class FfmpegDecoder final : public Decoder {
 public:
  ~FfmpegDecoder() override {
    if (frame_) {
      av_frame_free(&frame_);
    }
    if (packet_) {
      av_packet_free(&packet_);
    }
    if (swr_) {
      swr_free(&swr_);
    }
    if (codec_) {
      avcodec_free_context(&codec_);
    }
    if (fmt_) {
      avformat_close_input(&fmt_);
    }
  }

  std::string init(const std::filesystem::path& file, double rate) {
    rate_ = rate;
    av_log_set_level(
        AV_LOG_QUIET);  // damaged files make it chatty; what matters is reported through the result
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file",
                0);  // a file on disk, nothing that reaches out
    av_dict_set(&opts, "format_whitelist",
                "mov,mp4,m4a,3gp,3g2,mj2,ogg,matroska,webm,aac,asf,mp3,flac,wav,caf,ac3,eac3,ape,"
                "wv,tta,aiff",
                0);
    av_dict_set(&opts, "probesize", "2000000", 0);
    av_dict_set(&opts, "analyzeduration", "3000000", 0);
    fmt_ = avformat_alloc_context();
    if (!fmt_) {
      av_dict_free(&opts);
      return "Out of memory.";
    }
    const int r = avformat_open_input(&fmt_, fh::path_to_utf8(file).c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) {
      fmt_ = nullptr;
      return "That file could not be opened as a song (" + error_text(r) + ").";
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
      return "That file has no readable audio.";
    }
    const AVCodec* decoder = nullptr;
    stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream_ < 0 || !decoder) {
      return "That file has no audio this player can decode.";
    }
    for (unsigned i = 0; i < fmt_->nb_streams; i++) {
      if (std::cmp_not_equal(i, stream_)) {
        fmt_->streams[i]->discard = AVDISCARD_ALL;
      }
    }
    const AVStream* st = fmt_->streams[stream_];
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_ || avcodec_parameters_to_context(codec_, st->codecpar) < 0) {
      return "That file's audio could not be set up.";
    }
    if (avcodec_open2(codec_, decoder, nullptr) < 0) {
      return "That file's audio format could not be opened.";
    }
    if (codec_->sample_rate < 1000 || codec_->sample_rate > 400000 ||
        codec_->ch_layout.nb_channels < 1 || codec_->ch_layout.nb_channels > 16) {
      return "That file's audio has settings this player cannot use.";
    }
    if (!make_resampler()) {
      return "That file's audio could not be converted to the output format.";
    }
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!packet_ || !frame_) {
      return "Out of memory.";
    }
    time_base_ = av_q2d(st->time_base);
    start_time_ = st->start_time != AV_NOPTS_VALUE
                      ? st->start_time
                      : (fmt_->start_time != AV_NOPTS_VALUE
                             ? av_rescale_q(fmt_->start_time, AV_TIME_BASE_Q, st->time_base)
                             : 0);
    double seconds = -1;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
      seconds = static_cast<double>(st->duration) * time_base_;
    } else if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0) {
      seconds = static_cast<double>(fmt_->duration) / AV_TIME_BASE;
    }
    if (seconds > 0 && seconds < 3600.0 * 48) {
      total_ = static_cast<long long>(std::llround(seconds * rate));
    }
    format_ = codec_name(decoder->name);
    read_tags_from(fmt_->metadata);
    read_tags_from(st->metadata);
    return "";
  }

  long long read(float* out, long long frames) override {
    long long produced = 0;
    while (produced < frames) {
      const long long ready =
          static_cast<long long>(pend_.size() / 2) - static_cast<long long>(pend_pos_);
      if (ready > 0) {
        const long long n = std::min(ready, frames - produced);
        std::memcpy(out + produced * 2, pend_.data() + pend_pos_ * 2,
                    static_cast<std::size_t>(n) * 2 * sizeof(float));
        pend_pos_ += static_cast<std::size_t>(n);
        produced += n;
        continue;
      }
      pend_.clear();
      pend_pos_ = 0;
      if (!decode_more()) {
        break;
      }
    }
    return produced;
  }

  bool seek(long long frame) override {
    const double seconds = std::max(0.0, static_cast<double>(frame) / rate_);
    const int64_t ts = static_cast<int64_t>(std::llround(seconds / time_base_)) + start_time_;
    if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
      if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_ANY) < 0) {
        return false;
      }
    }
    avcodec_flush_buffers(codec_);
    swr_free(&swr_);
    if (!make_resampler()) {
      return false;
    }
    pend_.clear();
    pend_pos_ = 0;
    draining_ = false;
    finished_ = false;
    errors_ = 0;
    seek_target_ = frame;
    return true;
  }

  [[nodiscard]] long long total_frames() const override { return total_; }
  [[nodiscard]] double sample_rate() const override { return rate_; }
  [[nodiscard]] Tags tags() const override { return tags_; }
  [[nodiscard]] std::string format() const override { return format_; }

 private:
  static std::string error_text(int err) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, b, sizeof b);
    return b;
  }

  static std::string codec_name(const char* n) {
    const std::string c = n ? n : "";
    if (c == "aac") {
      return "AAC";
    }
    if (c == "alac") {
      return "ALAC";
    }
    if (c == "opus" || c == "libopus") {
      return "Opus";
    }
    if (c.starts_with("wma")) {
      return "WMA";
    }
    if (c == "vorbis" || c == "libvorbis") {
      return "Ogg Vorbis";
    }
    std::string up = c;
    for (char& ch : up) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return up.empty() ? "audio" : up;
  }

  void read_tags_from(const AVDictionary* d) {
    if (!d) {
      return;
    }
    const auto find = [&](const char* key) {
      const AVDictionaryEntry* e = av_dict_get(d, key, nullptr, 0);
      return e && e->value ? clean_tag_text(e->value) : std::string();
    };
    if (tags_.title.empty()) {
      tags_.title = find("title");
    }
    if (tags_.artist.empty()) {
      tags_.artist = find("artist");
    }
    if (tags_.album.empty()) {
      tags_.album = find("album");
    }
  }

  bool make_resampler() {
    return make_resampler_for(codec_->ch_layout, codec_->sample_fmt, codec_->sample_rate);
  }

  bool make_resampler_for(const AVChannelLayout& in_layout, int sample_fmt, int sample_rate) {
    swr_free(&swr_);
    AVChannelLayout out =
        {};  // (zeroed: av_channel_layout_copy first uninitialises its target, which was random
             // stack bytes before and crashed on some damaged files)
    av_channel_layout_default(&out, 2);
    AVChannelLayout in = {};
    if (in_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
      av_channel_layout_default(&in, in_layout.nb_channels);
    } else {
      av_channel_layout_copy(&in, &in_layout);
    }
    const int r =
        swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_FLT, static_cast<int>(rate_), &in,
                            static_cast<AVSampleFormat>(sample_fmt), sample_rate, 0, nullptr);
    swr_channels_ = in_layout.nb_channels;
    swr_format_ = sample_fmt;
    swr_rate_ = sample_rate;
    av_channel_layout_uninit(&in);
    av_channel_layout_uninit(&out);
    return r >= 0 && swr_init(swr_) >= 0;
  }

  // Converts the frame in frame_ to stereo float at our rate and adds it to pend_ (dropping the
  // part before a seek target).
  void convert_frame() {
    // The resampler was set up from the stream's first description of itself. A damaged or odd
    // stream (AAC in particular) can change sample format, channel count or rate in the middle, and
    // swr_convert then reads planes that are not there (found by media_fuzz_test). So a frame that
    // no longer matches gets a resampler made for what it really is, and a frame that describes
    // something impossible is skipped.
    const int frame_channels = frame_->ch_layout.nb_channels;
    if (frame_->nb_samples <= 0 || !frame_->extended_data || frame_->sample_rate < 1000 ||
        frame_->sample_rate > 400000 || frame_channels < 1 || frame_channels > 16 ||
        av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame_->format)) <= 0) {
      return;
    }
    if (frame_->format != swr_format_ || frame_->sample_rate != swr_rate_ ||
        frame_channels != swr_channels_) {
      if (!make_resampler_for(frame_->ch_layout, frame_->format, frame_->sample_rate)) {
        swr_free(&swr_);
        return;
      }
    }
    if (!swr_) {
      return;
    }
    const int cap = swr_get_out_samples(swr_, frame_->nb_samples);
    if (cap <= 0) {
      return;
    }
    std::vector<float> tmp(static_cast<std::size_t>(cap) * 2);
    auto* outp = reinterpret_cast<uint8_t*>(tmp.data());
    const int got = swr_convert(
        swr_, &outp, cap, const_cast<const uint8_t**>(frame_->extended_data), frame_->nb_samples);
    if (got <= 0) {
      return;
    }
    append_converted(tmp.data(), got, frame_->best_effort_timestamp);
  }

  void append_converted(const float* samples, int count, int64_t pts) {
    long long skip = 0;
    if (seek_target_ >=
        0) {  // after a seek the decoder starts a little early: drop what comes before the target
      if (pts != AV_NOPTS_VALUE) {
        const auto start = static_cast<long long>(
            std::llround(static_cast<double>(pts - start_time_) * time_base_ * rate_));
        skip = std::max<long long>(0, seek_target_ - start);
      }
      if (skip >= count) {
        return;
      }
      seek_target_ = -1;
    }
    pend_.insert(pend_.end(), samples + skip * 2, samples + static_cast<long long>(count) * 2);
  }

  // Decodes until some sound is waiting in pend_. False when the song is over.
  bool decode_more() {
    for (;;) {
      if (finished_) {
        return false;
      }
      int r = avcodec_receive_frame(codec_, frame_);
      if (r == 0) {
        errors_ = 0;
        const std::size_t before = pend_.size();
        convert_frame();
        av_frame_unref(frame_);
        if (pend_.size() > before) {
          return true;
        }
        continue;
      }
      if (r == AVERROR_EOF) {  // everything decoded: let the resampler give back what it holds
        finished_ = true;
        if (swr_) {
          std::vector<float> tmp(8192 * 2);
          auto* outp = reinterpret_cast<uint8_t*>(tmp.data());
          const int got = swr_convert(swr_, &outp, 8192, nullptr, 0);
          if (got > 0) {
            append_converted(tmp.data(), got, AV_NOPTS_VALUE);
            return !pend_.empty();
          }
        }
        return false;
      }
      if (r != AVERROR(EAGAIN)) {
        if (++errors_ > max_consecutive_errors) {
          finished_ = true;
          return false;
        }
        continue;
      }
      if (draining_) {
        finished_ = true;
        return false;
      }
      r = av_read_frame(fmt_, packet_);
      if (r < 0) {  // end of the file (or it is cut short): flush the decoder
        avcodec_send_packet(codec_, nullptr);
        draining_ = true;
        continue;
      }
      if (packet_->stream_index == stream_) {
        if (avcodec_send_packet(codec_, packet_) < 0 && ++errors_ > max_consecutive_errors) {
          av_packet_unref(packet_);
          finished_ = true;
          return false;
        }
      }
      av_packet_unref(packet_);
    }
  }

  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* codec_ = nullptr;
  SwrContext* swr_ = nullptr;
  int swr_channels_ = 0, swr_format_ = -1, swr_rate_ = 0;  // what swr_ was set up to take in
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  int stream_ = -1;
  double rate_ = 0, time_base_ = 1;
  int64_t start_time_ = 0;
  long long total_ = -1, seek_target_ = -1;
  bool draining_ = false, finished_ = false;
  int errors_ = 0;
  std::vector<float> pend_;
  std::size_t pend_pos_ = 0;
  Tags tags_;
  std::string format_;
};

}  // namespace

OpenResult open_ffmpeg_decoder(const std::filesystem::path& file, double output_rate) {
  OpenResult r;
  auto d = std::make_unique<FfmpegDecoder>();
  r.error = d->init(file, output_rate);
  if (r.error.empty()) {
    r.decoder = std::move(d);
  }
  return r;
}

}  // namespace dva
