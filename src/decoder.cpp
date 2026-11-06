#include "dva/decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dva/ma.hpp"
#include "fh/pathutf8.hpp"

#ifdef DVA_HAVE_FFMPEG
#include "decoder_ffmpeg.hpp"
#endif

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
}

namespace dva {

namespace fs = std::filesystem;

namespace {

// A file opened by its path. On Windows the narrow fopen / ma_decoder_init_file read the name in
// the ANSI code page and cannot open a Japanese or Chinese file name, so the wide versions are used
// there.
FILE* open_for_reading(const fs::path& file) {
#ifdef _WIN32
  return _wfopen(file.c_str(), L"rb");
#else
  return std::fopen(file.c_str(), "rb");
#endif
}

std::string extension_of(const fs::path& p) {
  std::string e = fh::path_to_utf8(p.extension());
  for (char& c : e) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return e;
}

class MiniaudioDecoder final : public Decoder {
 public:
  MiniaudioDecoder() = default;
  MiniaudioDecoder(const MiniaudioDecoder&) = delete;
  MiniaudioDecoder& operator=(const MiniaudioDecoder&) = delete;
  MiniaudioDecoder(MiniaudioDecoder&&) = delete;
  MiniaudioDecoder& operator=(MiniaudioDecoder&&) = delete;
  ~MiniaudioDecoder() override {
    if (open_) {
      ma_decoder_uninit(&dec_);
    }
  }
  std::string init(const fs::path& file, double rate) {
    const ma_decoder_config cfg =
        ma_decoder_config_init(ma_format_f32, 2, static_cast<ma_uint32>(rate));
#ifdef _WIN32
    const ma_result r = ma_decoder_init_file_w(file.c_str(), &cfg, &dec_);
#else
    const ma_result r = ma_decoder_init_file(file.c_str(), &cfg, &dec_);
#endif
    if (r != MA_SUCCESS) {
      return "That file could not be opened as a song (" + std::string(ma_result_description(r)) +
             ").";
    }
    open_ = true;
    rate_ = rate;
    ma_uint64 len = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec_, &len) == MA_SUCCESS && len > 0) {
      total_ = static_cast<long long>(len);
    }
    ma_format fmt = ma_format_unknown;
    ma_uint32 ch = 0;
    ma_uint32 sr = 0;
    ma_decoder_get_data_format(&dec_, &fmt, &ch, &sr, nullptr, 0);
    // A damaged header can claim 1 Hz (a few kilobytes then become minutes of upsampled hum); real
    // songs are nowhere near. The file's own rate (not the output's) gets the same limits as the
    // Ogg and ffmpeg paths.
    ma_uint32 native_rate = 0;
    ma_uint32 native_channels = 0;
    if (dec_.pBackend &&
        ma_data_source_get_data_format(dec_.pBackend, &fmt, &native_channels, &native_rate, nullptr,
                                       0) == MA_SUCCESS &&
        (native_rate < 1000 || native_rate > 400000)) {
      return "That file has a sample rate this player cannot use.";
    }
    const std::string ext = extension_of(file);
    format_ = ext == ".mp3" ? "MP3" : ext == ".flac" ? "FLAC" : ext == ".wav" ? "WAV" : "audio";
    return "";
  }
  long long read(float* out, long long frames) override {
    if (broken_) {
      return 0;
    }
    ma_uint64 got = 0;
    const ma_result r =
        ma_decoder_read_pcm_frames(&dec_, out, static_cast<ma_uint64>(frames), &got);
    if (r != MA_SUCCESS && r != MA_AT_END && got == 0) {
      return 0;
    }
    return static_cast<long long>(got);
  }
  // After a read that failed inside a damaged FLAC, dr_flac is half-way between two frames (a frame
  // header read, its samples not). A seek from there can "succeed" and leave pointers unset, and
  // the next read then walks through them and crashes (found by media_fuzz_test). Seeking to the
  // start first resets all of that state (cheap: one jump to the first frame), and the seek proper
  // starts from a clean decoder. A seek that fails goes back to the start too; if even that fails
  // the song is over and read() gives nothing more.
  bool seek(long long frame) override {
    if (broken_) {
      return false;
    }
    const ma_uint64 target = static_cast<ma_uint64>(std::max(0LL, frame));
    if (ma_decoder_seek_to_pcm_frame(&dec_, 0) != MA_SUCCESS) {
      broken_ = true;
      return false;
    }
    if (target == 0 || ma_decoder_seek_to_pcm_frame(&dec_, target) == MA_SUCCESS) {
      return true;
    }
    if (ma_decoder_seek_to_pcm_frame(&dec_, 0) != MA_SUCCESS) {
      broken_ = true;
    }
    return false;
  }
  [[nodiscard]] long long total_frames() const override { return total_; }
  [[nodiscard]] double sample_rate() const override { return rate_; }
  [[nodiscard]] std::string format() const override { return format_; }

 private:
  bool broken_ = false;
  ma_decoder dec_{};
  bool open_ = false;
  double rate_ = 0;
  long long total_ = -1;
  std::string format_;
};

// stb_vorbis (v1.22, the newest) trusts the sizes inside the Vorbis comment header: a vendor or
// comment length near 2^31 or 2^32 makes its allocation size wrap (it is an int, plus one), so it
// gets a block far too small and then writes the whole claimed length into it. Found by
// media_fuzz_test (heap overflow on a damaged Ogg). The library is left as it is; before it sees a
// file this reads the Ogg pages, joins the first two packets (the identification header and the
// comment header) and refuses the file unless every length in the comment header fits inside the
// packet. A real file always passes: a length that cannot fit in the packet is not a comment.
bool ogg_comment_header_sane(FILE* f) {
  constexpr std::size_t max_packet = 8U
                                     << 20;  // a comment header with cover art can be big; beyond
                                             // this the file is refused (the arena is 24 MB anyway)
  std::vector<unsigned char> packet;
  int packet_no = 0;
  std::uint32_t serial = 0;
  bool first = true;
  for (int pages = 0; pages < 4096; ++pages) {
    unsigned char h[27];
    if (std::fread(h, 1, 27, f) != 27 || std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) {
      return packet_no >= 2;
    }
    const std::uint32_t id =
        static_cast<std::uint32_t>(h[14]) | (static_cast<std::uint32_t>(h[15]) << 8) |
        (static_cast<std::uint32_t>(h[16]) << 16) | (static_cast<std::uint32_t>(h[17]) << 24);
    if (first) {
      serial = id;
      first = false;
    }
    unsigned char lacing[255];
    const int n = h[26];
    if (n && std::fread(lacing, 1, static_cast<std::size_t>(n), f) != static_cast<std::size_t>(n)) {
      return false;
    }
    std::size_t body = 0;
    for (int i = 0; i < n; ++i) {
      body += lacing[i];
    }
    std::vector<unsigned char> data(body);
    if (body && std::fread(data.data(), 1, body, f) != body) {
      return false;
    }
    if (id != serial) {
      continue;  // another logical stream: not ours
    }
    std::size_t at = 0;
    for (int i = 0; i < n; ++i) {
      packet.insert(packet.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                    data.begin() + static_cast<std::ptrdiff_t>(at + lacing[i]));
      at += lacing[i];
      if (packet.size() > max_packet) {
        return false;
      }
      if (lacing[i] < 255) {  // the packet is complete
        if (packet_no == 1) {
          const std::size_t size = packet.size();
          const unsigned char* p = packet.data();
          const auto u32 = [&](std::size_t o) {
            return static_cast<std::uint64_t>(p[o]) | (static_cast<std::uint64_t>(p[o + 1]) << 8) |
                   (static_cast<std::uint64_t>(p[o + 2]) << 16) |
                   (static_cast<std::uint64_t>(p[o + 3]) << 24);
          };
          if (size < 11 || p[0] != 3 || std::memcmp(p + 1, "vorbis", 6) != 0) {
            return false;
          }
          std::size_t pos = 7;
          std::uint64_t vendor = u32(pos);
          pos += 4;
          if (vendor > size - pos) {
            return false;
          }
          pos += static_cast<std::size_t>(vendor);
          if (pos + 4 > size) {
            return false;
          }
          const std::uint64_t count = u32(pos);
          pos += 4;
          if (count > (size - pos) / 4) {
            return false;  // every comment needs at least its own 4-byte length
          }
          for (std::uint64_t k = 0; k < count; ++k) {
            if (pos + 4 > size) {
              return false;
            }
            const std::uint64_t len = u32(pos);
            pos += 4;
            if (len > size - pos) {
              return false;
            }
            pos += static_cast<std::size_t>(len);
          }
          return true;
        }
        packet.clear();
        ++packet_no;
      }
    }
  }
  return false;
}

// Opens the file for stb_vorbis only if it passed the check above. `err` is stb's code
// (VORBIS_invalid_setup when refused).
stb_vorbis* open_vorbis_checked(const fs::path& file, int* err, const stb_vorbis_alloc* alloc) {
  FILE* f = open_for_reading(file);
  if (!f) {
    *err = VORBIS_file_open_failure;
    return nullptr;
  }
  if (!ogg_comment_header_sane(f)) {
    std::fclose(f);
    *err = VORBIS_invalid_setup;
    return nullptr;
  }
  if (std::fseek(f, 0, SEEK_SET) != 0) {
    std::fclose(f);
    *err = VORBIS_file_open_failure;
    return nullptr;
  }
  return stb_vorbis_open_file(f, 1, err, alloc);  // (stb closes the file itself when it gives up)
}

// stb_vorbis leaks a little when it gives up on a damaged file, and a hostile file could ask it for
// a lot of memory. So it is given one fixed block to work in (and told nothing else is available);
// the block is freed with the decoder, and a file that needs more is refused.
constexpr std::size_t vorbis_arena = 24U << 20;
struct Arena {
  std::unique_ptr<char[]> memory{new char[vorbis_arena]};
  stb_vorbis_alloc alloc{memory.get(), static_cast<int>(vorbis_arena)};
};

class VorbisDecoder final : public Decoder {
 public:
  VorbisDecoder() = default;
  VorbisDecoder(const VorbisDecoder&) = delete;
  VorbisDecoder& operator=(const VorbisDecoder&) = delete;
  VorbisDecoder(VorbisDecoder&&) = delete;
  VorbisDecoder& operator=(VorbisDecoder&&) = delete;
  ~VorbisDecoder() override {
    if (conv_) {
      ma_data_converter_uninit(&converter_storage_, nullptr);
    }
    if (v_) {
      stb_vorbis_close(v_);
    }
  }
  // (arena_ is declared first below so it is freed last)
  std::string init(const fs::path& file, double rate) {
    int err = 0;
    v_ = open_vorbis_checked(file, &err, &arena_.alloc);
    if (!v_) {
      return err == VORBIS_outofmem ? "That Ogg file is too complex for this player."
                                    : "That file could not be opened as an Ogg Vorbis song.";
    }
    const stb_vorbis_info info = stb_vorbis_get_info(v_);
    if (info.channels < 1 || info.channels > 8 || info.sample_rate < 1000 ||
        info.sample_rate > 400000) {
      return "That Ogg file has settings this player cannot use.";
    }
    src_rate_ = info.sample_rate;
    channels_ = static_cast<int>(info.channels);
    rate_ = rate;
    const ma_data_converter_config cfg = ma_data_converter_config_init(
        ma_format_f32, ma_format_f32, static_cast<ma_uint32>(channels_), 2,
        static_cast<ma_uint32>(src_rate_), static_cast<ma_uint32>(rate));
    if (ma_data_converter_init(&cfg, nullptr, &converter_storage_) != MA_SUCCESS) {
      return "That Ogg file could not be converted to the output format.";
    }
    conv_ = true;
    const unsigned int total = stb_vorbis_stream_length_in_samples(v_);
    if (total > 0) {
      total_ = static_cast<long long>(std::llround(static_cast<double>(total) * rate / src_rate_));
    }
    const stb_vorbis_comment c = stb_vorbis_get_comment(v_);
    tags_ = tags_from_vorbis_comments(c.comment_list, c.comment_list_length);
    return "";
  }
  long long read(float* out, long long frames) override {
    long long produced = 0;
    while (produced < frames) {
      if (in_pos_ >= in_frames_) {  // the converter has had everything fetched so far: fetch more
        if (ended_) {
          break;
        }
        in_buf_.resize(4096U * static_cast<std::size_t>(channels_));
        const int got = stb_vorbis_get_samples_float_interleaved(v_, channels_, in_buf_.data(),
                                                                 static_cast<int>(in_buf_.size()));
        if (got <= 0) {
          ended_ = true;
          break;
        }
        in_frames_ = static_cast<std::size_t>(got);
        in_pos_ = 0;
      }
      ma_uint64 in_count = in_frames_ - in_pos_;
      auto out_count = static_cast<ma_uint64>(frames - produced);
      if (ma_data_converter_process_pcm_frames(
              &converter_storage_, in_buf_.data() + in_pos_ * static_cast<std::size_t>(channels_),
              &in_count, out + produced * 2, &out_count) != MA_SUCCESS) {
        ended_ = true;
        break;
      }
      in_pos_ += static_cast<std::size_t>(in_count);
      produced += static_cast<long long>(out_count);
      if (in_count == 0 && out_count == 0) {
        break;  // no progress: do not spin
      }
    }
    return produced;
  }
  bool seek(long long frame) override {
    const double src_frame = std::max(0.0, static_cast<double>(frame) * src_rate_ / rate_);
    if (!stb_vorbis_seek(v_, static_cast<unsigned int>(src_frame))) {
      return false;
    }
    ma_data_converter_reset(&converter_storage_);
    in_pos_ = in_frames_ = 0;
    ended_ = false;
    return true;
  }
  [[nodiscard]] long long total_frames() const override { return total_; }
  [[nodiscard]] double sample_rate() const override { return rate_; }
  [[nodiscard]] Tags tags() const override { return tags_; }
  [[nodiscard]] std::string format() const override { return "Ogg Vorbis"; }

 private:
  Arena arena_;
  stb_vorbis* v_ = nullptr;
  ma_data_converter converter_storage_{};
  bool conv_ = false, ended_ = false;
  std::vector<float> in_buf_;
  std::size_t in_pos_ = 0, in_frames_ = 0;  // source frames fetched into in_buf_, and how many of
                                            // them the converter has used
  double src_rate_ = 0, rate_ = 0;
  int channels_ = 0;
  long long total_ = -1;
  Tags tags_;
};

}  // namespace

bool looks_like_song(const fs::path& file) {
  const std::string e = extension_of(file);
  if (e == ".mp3" || e == ".flac" || e == ".wav" || e == ".ogg" || e == ".oga") {
    return true;
  }
#ifdef DVA_HAVE_FFMPEG
  return e == ".m4a" || e == ".aac" || e == ".opus" || e == ".wma" || e == ".mka" || e == ".webm" ||
         e == ".alac" || e == ".mp4";
#else
  return false;
#endif
}

bool ffmpeg_supported() {
#ifdef DVA_HAVE_FFMPEG
  return true;
#else
  return false;
#endif
}

namespace {
// What kind of file is this, from its first bytes (the name can lie).
enum class Kind { Unknown, OggVorbis, OggOpus, OggOther, Wav, Flac, Mp3, Mp4, Matroska, Asf, Adts };
Kind sniff(const unsigned char* b, std::size_t n) {
  if (n >= 4 && std::memcmp(b, "OggS", 4) == 0) {
    for (std::size_t i = 0; i + 8 <= n && i < 120; i++) {  // the codec's name is in the first page
      if (std::memcmp(b + i, "OpusHead", 8) == 0) {
        return Kind::OggOpus;
      }
      if (i + 7 <= n && b[i] == 1 && std::memcmp(b + i + 1, "vorbis", 6) == 0) {
        return Kind::OggVorbis;
      }
    }
    return Kind::OggOther;
  }
  if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WAVE", 4) == 0) {
    return Kind::Wav;
  }
  if (n >= 4 && std::memcmp(b, "fLaC", 4) == 0) {
    return Kind::Flac;
  }
  if (n >= 8 && std::memcmp(b + 4, "ftyp", 4) == 0) {
    return Kind::Mp4;
  }
  if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) {
    return Kind::Matroska;
  }
  if (n >= 4 && b[0] == 0x30 && b[1] == 0x26 && b[2] == 0xB2 && b[3] == 0x75) {
    return Kind::Asf;
  }
  if (n >= 3 && std::memcmp(b, "ID3", 3) == 0) {
    return Kind::Mp3;
  }
  if (n >= 2 && b[0] == 0xFF && (b[1] & 0xF0) == 0xF0) {
    return (b[1] & 0x06) == 0 ? Kind::Adts : Kind::Mp3;
  }
  return Kind::Unknown;
}
// dr_wav (inside miniaudio) subtracts 4 from the size of a "fact" chunk without checking it is at
// least 4: a size of 0..3 becomes about 2^64, and it then seeks forward 2 GB at a time for ever, so
// opening such a WAV hangs the program (found by media_fuzz_test). Same for the AIFF and Wave64
// versions. The library is left as it is; before it sees a RIFF WAV this walks the chunk list and
// refuses a file whose "fact" chunk is too small. Containers this program does not list (RF64,
// RIFX, Wave64, AIFF) are refused outright: they go through code paths nobody here tests.
bool wav_chunks_sane(const fs::path& file) {
  FILE* f = open_for_reading(file);
  if (!f) {
    return false;
  }
  bool ok = true;
  unsigned char h[12];
  if (std::fread(h, 1, 12, f) != 12) {
    ok = false;
  }
  for (int chunks = 0; ok && chunks < 4096; ++chunks) {
    unsigned char c[8];
    if (std::fread(c, 1, 8, f) != 8) {
      break;  // the end of the file: nothing wrong found
    }
    const std::uint32_t size =
        static_cast<std::uint32_t>(c[4]) | (static_cast<std::uint32_t>(c[5]) << 8) |
        (static_cast<std::uint32_t>(c[6]) << 16) | (static_cast<std::uint32_t>(c[7]) << 24);
    if (std::memcmp(c, "fact", 4) == 0 && size < 4) {
      ok = false;
      break;
    }
    if (std::memcmp(c, "data", 4) == 0) {
      break;  // the library stops reading headers here
    }
    const long long skip = static_cast<long long>(size) + (size & 1);
    if (skip > 0 &&
        std::fseek(f, static_cast<long>(std::min<long long>(skip, 0x7FFFFFFF)), SEEK_CUR) != 0) {
      break;
    }
    if (skip > 0x7FFFFFFF) {
      break;
    }
  }
  std::fclose(f);
  return ok;
}
bool exotic_wav_container(const unsigned char* b, std::size_t n) {
  static const unsigned char w64[8] = {0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11};
  return (n >= 4 && (std::memcmp(b, "RF64", 4) == 0 || std::memcmp(b, "RIFX", 4) == 0 ||
                     std::memcmp(b, "FORM", 4) == 0)) ||
         (n >= 8 && std::memcmp(b, w64, 8) == 0);
}

// Whatever the file says, what leaves here is a number the equalizer and the sound device can take:
// a damaged float WAV (or a decoder fed junk) can produce NaN or infinity, and one NaN in an
// equalizer filter stays there for the rest of the session (silence until the program is
// restarted). Not-a-number and infinity become silence; a finite sample beyond +-64 (far louder
// than any real file) is held at 64. Also holds a decoder to its contract: never more frames than
// asked, never fewer than 0.
class Guarded final : public Decoder {
 public:
  explicit Guarded(std::unique_ptr<Decoder> inner) : d_(std::move(inner)) {}
  long long read(float* out, long long frames) override {
    long long n = d_->read(out, frames);
    if (n < 0) {
      return 0;
    }
    if (n > frames) {
      n = frames;
    }
    for (long long i = 0; i < n * 2; ++i) {
      const float v = out[i];
      if (!(v >= -64.0F && v <= 64.0F)) {
        out[i] = std::isfinite(v) ? (v > 0 ? 64.0F : -64.0F) : 0.0F;
      }
    }
    return n;
  }
  bool seek(long long frame) override { return d_->seek(frame); }
  [[nodiscard]] long long total_frames() const override { return d_->total_frames(); }
  [[nodiscard]] double sample_rate() const override { return d_->sample_rate(); }
  [[nodiscard]] Tags tags() const override { return d_->tags(); }
  [[nodiscard]] std::string format() const override { return d_->format(); }

 private:
  std::unique_ptr<Decoder> d_;
};

OpenResult open_decoder_unguarded(const fs::path& file, double output_rate);

}  // namespace

OpenResult open_decoder(const fs::path& file, double output_rate) {
  OpenResult r = open_decoder_unguarded(file, output_rate);
  if (r.decoder) {
    r.decoder = std::make_unique<Guarded>(std::move(r.decoder));
  }
  return r;
}

namespace {
OpenResult open_decoder_unguarded(const fs::path& file, double output_rate) {
  OpenResult r;
  if (!(output_rate >= 8000 && output_rate <= 384000)) {
    r.error = "The sound output has an unusable sample rate.";
    return r;
  }
  unsigned char head[160] = {0};
  std::size_t got = 0;
  {
    FILE* f = open_for_reading(file);
    if (!f) {
      r.error = "The file could not be opened.";
      return r;
    }
    got = std::fread(head, 1, sizeof head, f);
    std::fclose(f);
    if (got < 4) {
      r.error = "That file is too short to be a song.";
      return r;
    }
  }
  const Kind kind = sniff(head, got);
  if (kind == Kind::Wav && !wav_chunks_sane(file)) {
    r.error = "That WAV file is damaged.";
    return r;
  }
  if (kind == Kind::Unknown && exotic_wav_container(head, got)) {
    r.error = "That kind of WAV or AIFF file is not supported.";
    return r;
  }
  const auto via_miniaudio = [&] {
    auto d = std::make_unique<MiniaudioDecoder>();
    OpenResult o;
    o.error = d->init(file, output_rate);
    if (o.error.empty()) {
      o.decoder = std::move(d);
    }
    return o;
  };
  const auto via_vorbis = [&] {
    auto d = std::make_unique<VorbisDecoder>();
    OpenResult o;
    o.error = d->init(file, output_rate);
    if (o.error.empty()) {
      o.decoder = std::move(d);
    }
    return o;
  };
#ifdef DVA_HAVE_FFMPEG
  const auto via_ffmpeg = [&] { return open_ffmpeg_decoder(file, output_rate); };
#endif
  switch (kind) {
    case Kind::OggVorbis:
      return via_vorbis();
    case Kind::OggOpus:
    case Kind::OggOther:
    case Kind::Mp4:
    case Kind::Matroska:
    case Kind::Asf:
    case Kind::Adts:
#ifdef DVA_HAVE_FFMPEG
      return via_ffmpeg();
#else
      r.error = kind == Kind::OggOther
                    ? "That Ogg file is not Vorbis, and this build cannot read other kinds."
                    : "This build of the program cannot play that kind of file (M4A, Opus, WMA and "
                      "AAC need the ffmpeg libraries).";
      return r;
#endif
    default:
      break;
  }
  r = via_miniaudio();
#ifdef DVA_HAVE_FFMPEG
  if (!r.decoder) {
    OpenResult second = via_ffmpeg();
    if (second.decoder) {
      return second;
    }
  }  // a WAV with an odd encoding, a file with a wrong name
#endif
  return r;
}
}  // namespace

Tags read_file_tags(const fs::path& file) {
  int err = 0;
  Arena arena;
  stb_vorbis* v = open_vorbis_checked(file, &err, &arena.alloc);
  if (!v) {
    return {};
  }
  const stb_vorbis_comment c = stb_vorbis_get_comment(v);
  Tags t = tags_from_vorbis_comments(c.comment_list, c.comment_list_length);
  stb_vorbis_close(v);
  return t;
}

}  // namespace dva
