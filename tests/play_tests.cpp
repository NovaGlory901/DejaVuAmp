// Tests for decoding, tags and the player engine (the test plays the sound device by calling
// render()). Uses the small tone files in tests/data/ (left 440 Hz, right 660 Hz, amplitude 0.125).
// The damaged-file loops are meant to be run under the sanitizers.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include "check.hpp"
#include "dva/decoder.hpp"
#include "dva/player.hpp"
#include "dva/tags.hpp"
#include "fh/pathutf8.hpp"
#include "tests.hpp"

#ifndef DVA_DATA
#define DVA_DATA "tests/data"
#endif

using namespace dva;
using test::check;
static std::string data(const std::string& f) { return std::string(DVA_DATA) + "/" + f; }
// (not "/tmp": that folder does not exist on Windows)
static std::string tmp_path(const std::string& f) {
  return (std::filesystem::temp_directory_path() / f).string();
}

struct Pcm {
  std::vector<float> x;  // interleaved stereo
  [[nodiscard]] std::size_t frames() const { return x.size() / 2; }
};
static Pcm read_all(Decoder& d, long long limit = 20'000'000) {
  Pcm p;
  std::vector<float> buf(2 * 1000);
  for (;;) {
    const long long n = d.read(buf.data(), 1000);
    if (n <= 0) {
      break;
    }
    p.x.insert(p.x.end(), buf.begin(), buf.begin() + n * 2);
    if (std::cmp_greater(p.frames(), limit)) {
      break;
    }
  }
  return p;
}
// Frequency of one channel over frames [from, to): rising zero crossings per second.
static double frequency(const Pcm& p, int ch, std::size_t from, std::size_t to, double rate) {
  int crossings = 0;
  for (std::size_t i = from + 1; i < to && i < p.frames(); i++) {
    const float a = p.x[(i - 1) * 2 + static_cast<std::size_t>(ch)];
    const float b = p.x[i * 2 + static_cast<std::size_t>(ch)];
    if (a < 0 && b >= 0) {
      crossings++;
    }
  }
  return crossings * rate / static_cast<double>(to - from);
}
static double peak(const Pcm& p, int ch, std::size_t from, std::size_t to) {
  double m = 0;
  for (std::size_t i = from; i < to && i < p.frames(); i++) {
    m = std::max(m, static_cast<double>(std::fabs(p.x[i * 2 + static_cast<std::size_t>(ch)])));
  }
  return m;
}

static std::vector<unsigned char> read_file(const std::string& f) {
  std::ifstream in(f, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

static void put_be32(std::vector<unsigned char>& v, std::uint32_t n) {
  for (int s = 24; s >= 0; s -= 8) {
    v.push_back(static_cast<unsigned char>(n >> s));
  }
}
static void put_syncsafe(std::vector<unsigned char>& v, std::uint32_t n) {
  for (int s = 21; s >= 0; s -= 7) {
    v.push_back(static_cast<unsigned char>((n >> s) & 0x7F));
  }
}
static std::vector<unsigned char> id3(
    int version, const std::vector<std::pair<std::string, std::vector<unsigned char>>>& frames,
    unsigned flags = 0) {
  std::vector<unsigned char> body;
  for (const auto& f : frames) {
    for (char c : f.first) {
      body.push_back(static_cast<unsigned char>(c));
    }
    if (version == 2) {
      body.push_back(static_cast<unsigned char>(f.second.size() >> 16));
      body.push_back(static_cast<unsigned char>(f.second.size() >> 8));
      body.push_back(static_cast<unsigned char>(f.second.size()));
    } else {
      if (version == 4) {
        put_syncsafe(body, static_cast<std::uint32_t>(f.second.size()));
      } else {
        put_be32(body, static_cast<std::uint32_t>(f.second.size()));
      }
      body.push_back(0);
      body.push_back(0);
    }
    body.insert(body.end(), f.second.begin(), f.second.end());
  }
  std::vector<unsigned char> out = {
      'I', 'D', '3', static_cast<unsigned char>(version), 0, static_cast<unsigned char>(flags)};
  put_syncsafe(out, static_cast<std::uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
static std::vector<unsigned char> text(unsigned enc, const std::string& s) {
  std::vector<unsigned char> v = {static_cast<unsigned char>(enc)};
  v.insert(v.end(), s.begin(), s.end());
  return v;
}
static std::vector<unsigned char> utf16(const std::u16string& s, bool bom, bool be) {
  std::vector<unsigned char> v = {static_cast<unsigned char>(bom ? 1 : 2)};
  if (bom) {
    if (be) {
      v.push_back(0xFE);
      v.push_back(0xFF);
    } else {
      v.push_back(0xFF);
      v.push_back(0xFE);
    }
  }
  for (char16_t c : s) {
    if (be) {
      v.push_back(static_cast<unsigned char>(c >> 8));
      v.push_back(static_cast<unsigned char>(c));
    } else {
      v.push_back(static_cast<unsigned char>(c));
      v.push_back(static_cast<unsigned char>(c >> 8));
    }
  }
  return v;
}

static void tag_tests() {
  for (const char* f : {"tone.flac", "tone.mp3", "tone-v24.mp3", "tone.ogg"}) {
    const Tags t = read_tags(data(f));
    check(std::string(f) + " has its title, artist and album",
          t.title == "Test Tone" && t.artist == "The Sines" && t.album == "Pure",
          t.title + "|" + t.artist + "|" + t.album);
  }
  check("a WAV with none has none",
        !read_tags(data("tone.wav")).any() && !read_tags(data("nothing-here.mp3")).any());
  {
    auto v3 = id3(3, {{"TIT2", text(0, "Caf\xE9")},
                      {"TPE1", text(3, "Bj\xC3\xB6rk")},
                      {"TALB", text(0, "Album")}});
    const Tags t = parse_id3(v3.data(), v3.size());
    check("ID3v2.3, Latin-1 and UTF-8 text",
          t.title == "Caf\xC3\xA9" && t.artist == "Bj\xC3\xB6rk" && t.album == "Album", t.title);
    auto v4 = id3(4, {{"TIT2", utf16(u"Title \x266A", true, false)},
                      {"TPE1", utf16(u"Artist", false, true)}});
    const Tags t4 = parse_id3(v4.data(), v4.size());
    check("ID3v2.4, UTF-16 with and without a byte order mark",
          t4.title == "Title \xE2\x99\xAA" && t4.artist == "Artist", t4.title);
    auto v2 =
        id3(2, {{"TT2", text(0, "Old")}, {"TP1", text(0, "School")}, {"TAL", text(0, "Tags")}});
    const Tags t2 = parse_id3(v2.data(), v2.size());
    check("ID3v2.2 three-letter frames",
          t2.title == "Old" && t2.artist == "School" && t2.album == "Tags");
    auto multi = id3(4, {{"TPE1", text(3, std::string("One\0Two", 7))}});
    check("several artists are joined", parse_id3(multi.data(), multi.size()).artist == "One, Two");
    auto ctl = id3(3, {{"TIT2", text(0,
                                     "a\x01 b\x7F"
                                     "c")}});
    check("control characters never reach the display",
          parse_id3(ctl.data(), ctl.size()).title == "a b c");
    std::vector<unsigned char> v1(128, 0);
    std::copy_n("TAG", 3, v1.begin());
    std::copy_n("Song", 4, v1.begin() + 3);
    std::copy_n("Band", 4, v1.begin() + 33);
    const Tags t1 = parse_id3(v1.data(), v1.size());
    check("ID3v1 at the end of a file", t1.title == "Song" && t1.artist == "Band");
    const std::string long_text(500, 'x');
    auto big = id3(3, {{"TIT2", text(0, long_text)}});
    check("a long title is cut to 200 bytes",
          parse_id3(big.data(), big.size()).title.size() == 200);
    check("invalid UTF-8 is replaced, not passed on",
          clean_tag_text("ok\xFF\xFE"
                         "end") ==
                  "ok\xEF\xBF\xBD\xEF\xBF\xBD"
                  "end" &&
              clean_tag_text("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  }
  {
    // damaged tags: cut anywhere, mutated anywhere: no crash
    std::mt19937 gen(5);
    const auto good = id3(3, {{"TIT2", text(0, "Title")},
                              {"TPE1", utf16(u"Artist", true, false)},
                              {"TALB", text(3, "Album")}});
    int tried = 0;
    for (int i = 0; i < 4000; i++) {
      auto b = good;
      if (i % 3 == 0) {
        b.resize(gen() % (b.size() + 1));
      } else {
        for (int k = 0; k < 1 + static_cast<int>(gen() % 5); k++) {
          b[gen() % b.size()] = static_cast<unsigned char>(gen());
        }
      }
      parse_id3(b.data(), b.size());
      tried++;
    }
    const auto flac = read_file(data("tone.flac"));
    for (int i = 0; i < 3000; i++) {
      auto b = flac;
      b.resize(std::min<std::size_t>(b.size(), 600));
      if (i % 3 == 0) {
        b.resize(gen() % (b.size() + 1));
      } else {
        for (int k = 0; k < 1 + static_cast<int>(gen() % 6); k++) {
          b[gen() % b.size()] = static_cast<unsigned char>(gen());
        }
      }
      parse_flac_metadata(b.data(), b.size());
      tried++;
    }
    check("" + std::to_string(tried) +
              " damaged ID3 and FLAC tag blocks were all answered without a crash",
          true);
  }
}

static void decoder_tests() {
  struct Case {
    const char* file;
    double seconds;
    double rate;
    const char* format;
    bool lossy;
  };
  for (const Case& c :
       {Case{"tone.wav", 0.5, 48000, "WAV", false}, Case{"tone.flac", 1, 48000, "FLAC", false},
        Case{"tone.mp3", 1, 48000, "MP3", true}, Case{"tone-v24.mp3", 1, 48000, "MP3", true},
        Case{"tone.ogg", 1, 48000, "Ogg Vorbis", true},
        Case{"tone32k.ogg", 1, 48000, "Ogg Vorbis", true},
        Case{"tone48-24bit.wav", 0.5, 48000, "WAV", false},
        Case{"tone.wav", 0.5, 44100, "WAV", false},
        Case{"tone.ogg", 1, 96000, "Ogg Vorbis", true}}) {
    const std::string label =
        std::string(c.file) + " at " + std::to_string(static_cast<int>(c.rate)) + " Hz";
    OpenResult r = open_decoder(data(c.file), c.rate);
    if (!r.decoder) {
      check("" + label + " opens", false, r.error);
      continue;
    }
    Decoder& d = *r.decoder;
    const Pcm p = read_all(d);
    const double got = static_cast<double>(p.frames()) / c.rate;
    const auto mid0 = static_cast<std::size_t>(0.2 * c.rate);
    const auto mid1 = static_cast<std::size_t>(0.45 * c.rate);
    check("" + label + " is " + std::string(c.format) + ", about " + std::to_string(c.seconds) +
              " s long, and the total it announces agrees",
          d.format() == c.format && std::fabs(got - c.seconds) < 0.08 &&
              (d.total_frames() < 0 ||
               std::fabs(static_cast<double>(d.total_frames()) / c.rate - c.seconds) < 0.08) &&
              d.sample_rate() == c.rate,
          d.format() + " " + std::to_string(got) + " s, total " + std::to_string(d.total_frames()));
    const double fl = frequency(p, 0, mid0, mid1, c.rate);
    const double fr = frequency(p, 1, mid0, mid1, c.rate);
    check("" + label + " left is 440 Hz and right is 660 Hz, at amplitude 0.125",
          std::fabs(fl - 440) < 12 && std::fabs(fr - 660) < 12 &&
              std::fabs(peak(p, 0, mid0, mid1) - 0.125) < (c.lossy ? 0.03 : 0.004),
          std::to_string(fl) + " / " + std::to_string(fr) + " / " +
              std::to_string(peak(p, 0, mid0, mid1)));
  }
  if (ffmpeg_supported()) {
    struct FCase {
      const char* file;
      const char* format;
      bool lossy;
    };
    for (const FCase& c : {FCase{"tone.m4a", "AAC", true}, FCase{"tone-alac.m4a", "ALAC", false},
                           FCase{"tone.opus", "Opus", true}, FCase{"tone.wma", "WMA", true},
                           FCase{"tone.aac", "AAC", true}}) {
      for (const double rate : {48000.0, 44100.0}) {
        const std::string label =
            std::string(c.file) + " at " + std::to_string(static_cast<int>(rate)) + " Hz";
        OpenResult r = open_decoder(data(c.file), rate);
        if (!r.decoder) {
          check("" + label + " opens", false, r.error);
          continue;
        }
        const Pcm p = read_all(*r.decoder);
        const double got = static_cast<double>(p.frames()) / rate;
        const auto mid0 = static_cast<std::size_t>(0.25 * rate);
        const auto mid1 = static_cast<std::size_t>(0.75 * rate);
        const double fl = frequency(p, 0, mid0, mid1, rate);
        const double fr = frequency(p, 1, mid0, mid1, rate);
        check("" + label + " is " + c.format +
                  ", about 1 s, left 440 Hz and right 660 Hz at amplitude 0.125",
              r.decoder->format() == c.format && std::fabs(got - 1.0) < 0.12 &&
                  std::fabs(fl - 440) < 15 && std::fabs(fr - 660) < 20 &&
                  std::fabs(peak(p, 0, mid0, mid1) - 0.125) < (c.lossy ? 0.03 : 0.004),
              r.decoder->format() + " " + std::to_string(got) + " s " + std::to_string(fl) + "/" +
                  std::to_string(fr) + " " + std::to_string(peak(p, 0, mid0, mid1)));
      }
    }
    for (const char* f : {"tone.m4a", "tone.opus", "tone.wma"}) {
      const Tags t = open_decoder(data(f), 48000).decoder->tags();
      check(std::string(f) + " has its title, artist and album",
            t.title == "Test Tone" && t.artist == "The Sines" && t.album == "Pure",
            t.title + "|" + t.artist + "|" + t.album);
    }
    for (const char* f : {"tone.m4a", "tone-alac.m4a", "tone.opus"}) {
      OpenResult r = open_decoder(data(f), 48000);
      Decoder& d = *r.decoder;
      const long long total = d.total_frames();
      bool ok = total > 40000 && d.seek(24000);
      const Pcm tail = read_all(d);
      ok = ok && std::fabs(tail.frames() / 48000.0 - (total - 24000) / 48000.0) < 0.06 &&
           std::fabs(frequency(tail, 0, 2000, 12000, 48000) - 440) < 20;
      ok = ok && d.seek(0) && read_all(d).frames() > static_cast<std::size_t>(total * 0.9);
      check(
          std::string(f) + " seeks to the middle and back to the start", ok,
          std::to_string(total) + " frames, " + std::to_string(tail.frames()) + " after the seek");
    }
    {
      // the kind of file is decided by what is inside, not by its name
      std::filesystem::copy_file(data("tone.m4a"), tmp_path("dva_renamed.dat"),
                                 std::filesystem::copy_options::overwrite_existing);
      std::filesystem::copy_file(data("tone.opus"), tmp_path("dva_renamed.mp3"),
                                 std::filesystem::copy_options::overwrite_existing);
      OpenResult a = open_decoder(tmp_path("dva_renamed.dat"), 48000);
      OpenResult b = open_decoder(tmp_path("dva_renamed.mp3"), 48000);
      check("an M4A called .dat and an Opus file called .mp3 are still played",
            a.decoder && a.decoder->format() == "AAC" && b.decoder && b.decoder->format() == "Opus",
            a.error + b.error);
      a.decoder.reset();
      b.decoder.reset();  // (Windows cannot delete a file that is still open)
      std::remove(tmp_path("dva_renamed.dat").c_str());
      std::remove(tmp_path("dva_renamed.mp3").c_str());
    }
    {
      OpenResult r = open_decoder(data("tone-float.wav"), 48000);
      const Pcm p = r.decoder ? read_all(*r.decoder) : Pcm{};
      check("a 32-bit float WAV plays (miniaudio or the ffmpeg fallback)",
            r.decoder && std::fabs(p.frames() / 48000.0 - 0.5) < 0.05 &&
                std::fabs(frequency(p, 0, 4800, 19200, 48000) - 440) < 15,
            r.error);
    }
    check("looksLikeSong accepts the newer file endings",
          looks_like_song("a.M4A") && looks_like_song("a.opus") && looks_like_song("a.wma") &&
              looks_like_song("a.aac"));
    {
      std::mt19937 gen(4242);
      int tried = 0;
      int played = 0;
      const std::string tmp = tmp_path("dva_damaged_ff");
      for (const char* f : {"tone.m4a", "tone-alac.m4a", "tone.opus", "tone.wma", "tone.aac"}) {
        const auto original = read_file(data(f));
        const std::string ext = std::string(f).substr(std::string(f).rfind('.'));
        for (int n = 0; n < 60; n++) {
          auto b = original;
          const int how = static_cast<int>(gen() % 4);
          if (how == 0) {
            b.resize(gen() % (b.size() + 1));
          } else if (how == 1) {
            for (int k = 0; k < 1 + static_cast<int>(gen() % 8); k++) {
              b[gen() % b.size()] = static_cast<unsigned char>(gen());
            }
          } else if (how == 2) {
            const std::size_t at = gen() % b.size();
            for (std::size_t k = at; k < std::min(b.size(), at + 24); k++) {
              b[k] = 0xFF;
            }
          } else {
            const std::size_t at = gen() % b.size();
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(at),
                     static_cast<unsigned char>(gen()));
          }
          {
            std::ofstream out(tmp + ext, std::ios::binary);
            out.write(reinterpret_cast<const char*>(b.data()),
                      static_cast<std::streamsize>(b.size()));
          }
          OpenResult r = open_decoder(tmp + ext, 48000);
          tried++;
          if (r.decoder) {
            played++;
            const Pcm p = read_all(*r.decoder, 2'000'000);
            r.decoder->seek(static_cast<long long>(gen() % 60000));
            read_all(*r.decoder, 200000);
            for (const float v : p.x) {
              if (!std::isfinite(v)) {
                check("damaged file gave a non-finite sample", false, f);
                break;
              }
            }
          }
        }
        std::remove((tmp + ext).c_str());
      }
      check("" + std::to_string(tried) +
                " damaged M4A, ALAC, Opus, WMA and AAC files were all answered without a crash (" +
                std::to_string(played) + " still opened)",
            true);
    }
  } else {
    check("without ffmpeg an M4A is refused with a reason",
          !open_decoder(data("tone.m4a"), 48000).decoder &&
              !open_decoder(data("tone.m4a"), 48000).error.empty());
  }
  {
    OpenResult r = open_decoder(data("mono22k.wav"), 48000);
    Pcm p = read_all(*r.decoder);
    bool same = true;
    for (std::size_t i = 0; i < p.frames(); i++) {
      if (p.x[i * 2] != p.x[i * 2 + 1]) {
        same = false;
      }
    }
    check(
        "a mono 22 kHz file comes out as stereo at 48 kHz with both sides the same, at 440 "
        "Hz",
        r.decoder && same && std::fabs(frequency(p, 0, 4800, 19200, 48000) - 440) < 12 &&
            std::fabs(p.frames() / 48000.0 - 0.5) < 0.05);
  }
  for (const char* f : {"tone.wav", "tone.flac", "tone.mp3", "tone.ogg"}) {
    OpenResult r = open_decoder(data(f), 48000);
    Decoder& d = *r.decoder;
    const long long total = d.total_frames();
    bool ok = d.seek(total / 2);
    const Pcm tail = read_all(d);
    const double left = static_cast<double>(tail.frames()) / 48000.0;
    const double want = static_cast<double>(total - total / 2) / 48000.0;
    ok = ok && std::fabs(left - want) < 0.05 &&
         std::fabs(frequency(tail, 0, 2000, std::min<std::size_t>(tail.frames(), 12000), 48000) -
                   440) < 15;
    ok = ok && d.seek(0) && read_all(d).frames() > static_cast<std::size_t>(total * 0.9);
    check(std::string(f) + " seeks to the middle and back to the start", ok,
          std::to_string(left) + " vs " + std::to_string(want));
  }
  check("a missing file, a text file and an invalid sample rate are refused with a reason",
        !open_decoder(data("nothing.mp3"), 48000).decoder &&
            !open_decoder(data("nothing.mp3"), 48000).error.empty() &&
            !open_decoder(data("../CMakeLists.txt"), 48000).decoder &&
            !open_decoder(data("tone.wav"), 5).decoder &&
            !open_decoder(data("tone.wav"), 1e9).decoder);
  check("looksLikeSong", looks_like_song("a.MP3") && looks_like_song("a.flac") &&
                             looks_like_song("x/y.ogg") && looks_like_song("a.wav") &&
                             !looks_like_song("a.txt") && !looks_like_song("a.png"));
  {
    std::mt19937 gen(77);
    int tried = 0;
    int played = 0;
    const std::string tmp = tmp_path("dva_damaged_test.bin");
    for (const char* f : {"tone.wav", "tone.flac", "tone.mp3", "tone.ogg", "tone-v24.mp3"}) {
      const auto original = read_file(data(f));
      const std::string ext = std::string(f).substr(std::string(f).rfind('.'));
      for (int n = 0; n < 60; n++) {
        auto b = original;
        const int how = static_cast<int>(gen() % 4);
        if (how == 0) {
          b.resize(gen() % (b.size() + 1));
        } else if (how == 1) {
          for (int k = 0; k < 1 + static_cast<int>(gen() % 8); k++) {
            b[gen() % b.size()] = static_cast<unsigned char>(gen());
          }
        } else if (how == 2) {
          const std::size_t at = gen() % b.size();
          for (std::size_t k = at; k < std::min(b.size(), at + 24); k++) {
            b[k] = 0xFF;
          }
        } else {
          const std::size_t at = gen() % b.size();
          b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), static_cast<unsigned char>(gen()));
        }
        {
          std::ofstream out(tmp + ext, std::ios::binary);
          out.write(reinterpret_cast<const char*>(b.data()),
                    static_cast<std::streamsize>(b.size()));
        }
        OpenResult r = open_decoder(tmp + ext, 48000);
        tried++;
        if (r.decoder) {
          played++;
          const Pcm p = read_all(*r.decoder, 2'000'000);
          r.decoder->seek(static_cast<long long>(gen() % 100000));
          read_all(*r.decoder, 200000);
          for (const float v : p.x) {
            if (!std::isfinite(v)) {
              check("damaged file gave a non-finite sample", false, f);
              break;
            }
          }
          read_tags(tmp + ext);
        }
      }
    }
    for (int n = 0; n < 200; n++) {
      std::vector<unsigned char> b(gen() % 3000);
      for (auto& x : b) {
        x = static_cast<unsigned char>(gen());
      }
      if (n % 4 == 0 && b.size() > 4) {
        std::copy_n("OggS", 4, b.begin());
      }
      if (n % 4 == 1 && b.size() > 4) {
        std::copy_n("fLaC", 4, b.begin());
      }
      if (n % 4 == 2 && b.size() > 4) {
        std::copy_n("RIFF", 4, b.begin());
      }
      {
        std::ofstream out(tmp + ".bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
      }
      OpenResult r = open_decoder(tmp + ".bin", 48000);
      if (r.decoder) {
        read_all(*r.decoder, 100000);
      }
      tried++;
    }
    std::remove((tmp + ".wav").c_str());
    std::remove((tmp + ".flac").c_str());
    std::remove((tmp + ".mp3").c_str());
    std::remove((tmp + ".ogg").c_str());
    std::remove((tmp + ".bin").c_str());
    check("" + std::to_string(tried) +
              " damaged and random files were all answered without a crash (" +
              std::to_string(played) + " still opened)",
          true);
  }
}

// Renders `frames` frames in device-sized pieces, pausing a moment between so the decoder thread
// keeps up.
static std::vector<float> render_for(Player& pl, int frames, int piece = 480,
                                     int sleep_micros = 500) {
  std::vector<float> all;
  std::vector<float> buf(static_cast<std::size_t>(piece) * 2);
  for (int done = 0; done < frames; done += piece) {
    pl.render(buf.data(), piece);
    all.insert(all.end(), buf.begin(), buf.end());
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_micros));
  }
  return all;
}
static double peak_of(const std::vector<float>& x, std::size_t from, std::size_t to, int ch) {
  double m = 0;
  for (std::size_t i = from; i < to && i * 2 + 1 < x.size(); i++) {
    m = std::max(m, static_cast<double>(std::fabs(x[i * 2 + static_cast<std::size_t>(ch)])));
  }
  return m;
}

static void player_tests() {
  Settings flat;
  flat.volume_db = 0;
  flat.limiter = false;
  {
    Player pl(48000);
    pl.set_settings(flat);
    check("nothing loaded plays silence",
          peak_of(render_for(pl, 4800), 0, 4800, 0) == 0 && !pl.status().loaded);
    std::string err;
    check("a file that is not a song is refused with a reason, and nothing changes",
          !pl.load(data("../CMakeLists.txt"), err) && !err.empty() && !pl.status().loaded);
    check("a good file loads", pl.load(data("tone.flac"), err), err);
    const Player::Status st0 = pl.status();
    check("it knows the length, the tags and the format right away",
          st0.loaded && st0.playing && std::fabs(st0.duration - 1.0) < 0.05 &&
              st0.tags.title == "Test Tone" && st0.format == "FLAC");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // let the decoder fill the ring
    const auto sound = render_for(pl, 24000);
    const Player::Status st1 = pl.status();
    check("it plays the tone (440 Hz at 0.125, with the filters flat and 0 dB)",
          std::fabs(peak_of(sound, 8000, 20000, 0) - 0.125) < 0.005 &&
              std::fabs(peak_of(sound, 8000, 20000, 1) - 0.125) < 0.005);
    check("the position follows what was played (24000 frames = 0.5 s)",
          std::fabs(st1.position - 0.5) < 0.03 && st1.underruns == 0,
          std::to_string(st1.position) + " underruns " + std::to_string(st1.underruns));
    pl.pause();
    const auto fading = render_for(pl, 4800);
    const double paused_at = pl.status().position;
    const auto silent = render_for(pl, 4800);
    check(
        "pausing fades out over a few milliseconds and then stays silent, with the "
        "position held",
        peak_of(fading, 1200, 2400, 0) < 1e-6 && peak_of(silent, 0, 2400, 0) == 0 &&
            std::fabs(pl.status().position - paused_at) < 1e-9 && !pl.status().playing);
    pl.play();
    const auto back = render_for(pl, 4800);
    check("playing again fades back in and carries on from the same place",
          peak_of(back, 0, 100, 0) < 0.05 && peak_of(back, 2000, 4800, 0) > 0.1 &&
              pl.status().position > paused_at);
    pl.seek(0.1);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto sought = render_for(pl, 2400);
    check("seeking moves the position at once and plays from there",
          std::fabs(pl.status().position - 0.15) < 0.03 && peak_of(sought, 600, 2400, 0) > 0.1,
          std::to_string(pl.status().position));
    bool ended = false;
    for (int i = 0; i < 400 && !ended; i++) {
      render_for(pl, 4800);
      ended = pl.take_ended();
    }
    check("reaching the end is reported once, and it stops",
          ended && !pl.status().playing && !pl.take_ended());
    pl.play();
    pl.stop();
    check("stop goes back to the start, paused",
          std::fabs(pl.status().position) < 0.01 && !pl.status().playing);
  }
  {
    Player pl(48000);
    Settings s = flat;
    s.volume_db = -20;
    pl.set_settings(s);
    std::string err;
    pl.load(data("tone.wav"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto sound = render_for(pl, 12000);
    check("the volume setting reaches the sound (-20 dB: 0.125 becomes 0.0125)",
          std::fabs(peak_of(sound, 6000, 12000, 0) - 0.0125) < 0.001,
          std::to_string(peak_of(sound, 6000, 12000, 0)));
  }
  {
    Player pl(48000);
    pl.set_settings(flat);
    std::string err;
    pl.load(data("tone.mp3"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    render_for(pl, 4800);
    check("a second file replaces the first at once", pl.load(data("tone.ogg"), err) &&
                                                          pl.status().format == "Ogg Vorbis" &&
                                                          pl.status().position == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto sound = render_for(pl, 9600);
    check("the second file plays from its start", peak_of(sound, 2000, 9600, 0) > 0.1);
    bool bad = false;
    for (int i = 0; i < 30; i++) {
      pl.seek((i % 7) * 0.13);
      render_for(pl, 480);
      if (i % 5 == 0) {
        pl.load(data(i % 2 ? "tone.flac" : "tone.mp3"), err);
      }
      if (i % 4 == 0) {
        pl.pause();
      } else {
        pl.play();
      }
    }
    const auto last = render_for(pl, 4800);
    for (const float v : last) {
      if (!std::isfinite(v)) {
        bad = true;
      }
    }
    check(
        "seeking, loading and pausing in a hurry stays sane (this is a thread-safety "
        "check; run it under the sanitizers)",
        !bad);
  }
  if (ffmpeg_supported()) {
    Player pl(48000);
    pl.set_settings(flat);
    std::string err;
    check("an M4A loads and shows its tags",
          pl.load(data("tone.m4a"), err) && pl.status().tags.title == "Test Tone" &&
              pl.status().format == "AAC",
          err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto sound = render_for(pl, 12000);
    check("the M4A plays", peak_of(sound, 4000, 12000, 0) > 0.1);
    check("an Opus file replaces it",
          pl.load(data("tone.opus"), err) && pl.status().format == "Opus", err);
  }
  {
    // speed: twice as fast takes half the time, the pitch stays, and the position is in song
    // seconds
    Player pl(48000);
    Settings s = flat;
    s.speed = 2;
    pl.set_settings(s);
    std::string err;
    pl.load(data("tone.flac"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<float> all;
    bool ended = false;
    for (int i = 0; i < 200 && !ended; i++) {
      auto piece = render_for(pl, 2400, 480, 6000);
      all.insert(all.end(), piece.begin(), piece.end());
      ended = pl.take_ended();
    }
    std::size_t audible = 0;
    for (std::size_t i = 0; i + 1 < all.size(); i += 2) {
      if (std::fabs(all[i]) > 0.001F) {
        audible = i / 2;
      }
    }
    Pcm p;
    p.x = all;
    const double hz = frequency(p, 0, 12000, 24000, 48000);
    check("speed 2x plays the 1 s song in about half a second at the same pitch",
          ended && audible > 20000 && audible < 29000 && std::fabs(hz - 440) < 12,
          "audible " + std::to_string(audible) + " frames, " + std::to_string(hz) + " Hz");
  }
  {
    // changing the speed in the middle keeps the place in the song
    Player pl(48000);
    pl.set_settings(flat);
    std::string err;
    pl.load(data("tone.mp3"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    render_for(pl, 12000, 480, 6000);
    const double before = pl.status().position;
    Settings s = flat;
    s.speed = 0.5;
    pl.set_settings(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto sound = render_for(pl, 12000, 480, 6000);
    const double after = pl.status().position;
    Pcm p;
    p.x = sound;
    check(
        "changing the speed in the middle carries on from the same place at the new speed "
        "(0.25 s of song, then 0.25 s heard at half speed = 0.125 s more)",
        std::fabs(before - 0.25) < 0.04 && std::fabs(after - before - 0.125) < 0.05 &&
            std::fabs(frequency(p, 0, 6000, 12000, 48000) - 440) < 12,
        std::to_string(before) + " -> " + std::to_string(after));
    s.speed = 1;
    pl.set_settings(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto normal = render_for(pl, 6000);
    Pcm q;
    q.x = normal;
    check("returning to normal speed carries on from the same place",
          pl.status().position > after && peak(q, 0, 2000, 6000) > 0.1);
  }
  {
    // a slow decoder thread must not block the device: if the ring is empty the output is silence
    // and counted
    Player pl(48000);
    pl.set_settings(flat);
    std::string err;
    pl.load(data("tone.wav"), err);
    std::vector<float> buf(960);
    for (int i = 0; i < 50; i++) {
      pl.render(buf.data(), 480);  // no waiting at all
    }
    check("rendering never waits for the decoder (it plays silence instead)", true);
  }
}

// Strength of one frequency in a stretch of the left channel (Goertzel), as an amplitude.
static double tone_part(const std::vector<float>& x, std::size_t from, std::size_t to, double hz,
                        double rate) {
  const double w = 2 * std::numbers::pi * hz / rate;
  const double c = 2 * std::cos(w);
  double s1 = 0;
  double s2 = 0;
  std::size_t n = 0;
  for (std::size_t i = from; i < to && i * 2 < x.size(); i++, n++) {
    const double s = x[i * 2] + c * s1 - s2;
    s2 = s1;
    s1 = s;
  }
  const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
  return n ? 2 * std::sqrt(std::max(0.0, power)) / static_cast<double>(n) : 0;
}
static double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double sum = 0;
  std::size_t n = 0;
  for (std::size_t i = from; i < to && i * 2 < x.size(); i++, n++) {
    sum += static_cast<double>(x[i * 2]) * x[i * 2];
  }
  return n ? std::sqrt(sum / static_cast<double>(n)) : 0;
}
// Plays until the song has ended (or `max_frames`), at ten times real time, collecting everything.
static std::vector<float> play_out(Player& pl, int max_frames, int sleep_micros = 400) {
  std::vector<float> all;
  std::vector<float> buf(2 * 480);
  bool ended = false;
  for (int done = 0; done < max_frames && !ended; done += 480) {
    pl.render(buf.data(), 480);
    all.insert(all.end(), buf.begin(), buf.end());
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_micros));
    ended = pl.take_ended();
  }
  return all;
}

static void crossfade_tests() {
  Settings flat;
  flat.volume_db = 0;
  flat.limiter = false;
  const double fs = 48000;
  std::string err;
  {
    // skipping with a crossfade: A (440 Hz) is heard until the skip, then fades out while B (880
    // Hz) fades in from its start
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(1.0);
    pl.load(data("xfadeA.flac"), err);
    render_for(pl, static_cast<int>(0.6 * fs));
    check("skip: the skip is accepted", pl.skip_to(data("xfadeB.flac"), err), err);
    auto x = render_for(pl, static_cast<int>(2.0 * fs));
    const auto s = static_cast<std::size_t>(fs);
    const double a_start = tone_part(x, 0, 480, 440, fs);
    const double b_start = tone_part(x, 0, 480, 880, fs);
    const double a_mid = tone_part(x, s / 2 - 2400, s / 2 + 2400, 440, fs);
    const double b_mid = tone_part(x, s / 2 - 2400, s / 2 + 2400, 880, fs);
    const double a_end = tone_part(x, s + 4800, s + 9600, 440, fs);
    const double b_end = tone_part(x, s + 4800, s + 9600, 880, fs);
    check(
        "skip: the outgoing song is still there at the moment of the skip (no hard cut), both are "
        "heard mid-blend, then only the new one",
        a_start > 0.03 && b_start < 0.03 && a_mid > 0.06 && b_mid > 0.06 && a_end < 0.01 &&
            b_end > 0.11,
        "A " + std::to_string(a_start) + " " + std::to_string(a_mid) + " " + std::to_string(a_end) +
            " B " + std::to_string(b_start) + " " + std::to_string(b_mid) + " " +
            std::to_string(b_end));
    double worst_step = 0;
    double lo = 1e9;
    double hi = 0;
    for (std::size_t i = 0; i + 1 < x.size() / 2 && i < s + 2400; i++) {
      worst_step = std::max(worst_step, static_cast<double>(std::fabs(x[(i + 1) * 2] - x[i * 2])));
    }
    for (std::size_t i = 0; i + 2400 < s + 2400; i += 2400) {
      const double r = rms(x, i, i + 2400);
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
#if defined(_WIN32) || defined(__SANITIZE_ADDRESS__)
    const double even_limit = 1.42;  // 3 dB (see the note below)
#else
    const double even_limit = 1.25;
#endif
    check("skip: no click at the start of the blend, and the loudness stays even through it",
          worst_step < 0.15 && hi / lo < even_limit,
          "step " + std::to_string(worst_step) + ", " + std::to_string(20 * std::log10(hi / lo)) +
              " dB");
    // Windows and sanitizer builds wake the refill thread more slowly, so they get twice the
    // silence allowance here and 3 dB in the loudness check above.
#if defined(_WIN32) || defined(__SANITIZE_ADDRESS__)
    const std::size_t quiet_limit = 1920;
#else
    const std::size_t quiet_limit = 960;
#endif
    std::size_t quiet = 0;
    std::size_t quiet_max = 0;  // the ring is dropped at the skip: the refill must be quick
    for (std::size_t i = 0; i < s / 2; i++) {
      if (std::fabs(x[i * 2]) < 1e-5F) {
        quiet++;
        quiet_max = std::max(quiet_max, quiet);
      } else {
        quiet = 0;
      }
    }
    check("skip: the join is quick (the longest silence in the first half second is under 20 ms)",
          quiet_max < quiet_limit, std::to_string(quiet_max) + " frames");
    const Player::Status st = pl.status();
    check("skip: the song heard is now B, shown from its start",
          st.tags.title == "Song B" && st.position > 1.9 && st.position < 2.1,
          st.tags.title + " " + std::to_string(st.position));
  }
  {
    // no crossfade, nothing playing, or a rapid double skip: still sane
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(0);
    pl.load(data("xfadeA.flac"), err);
    render_for(pl, 4800);
    check("skip: with no crossfade it is a plain cut to the new song",
          pl.skip_to(data("xfadeB.flac"), err) && pl.status().tags.title == "Song B" &&
              pl.status().position == 0,
          err);
    pl.set_crossfade(1.0);
    pl.pause();
    render_for(pl, 2400);
    check("skip: while paused it is a plain load",
          pl.skip_to(data("xfadeA.flac"), err) && pl.status().tags.title != "Song B" &&
              pl.status().position == 0,
          err);
    pl.play();
    render_for(pl, 24000);
    pl.skip_to(data("xfadeB.flac"), err);
    pl.skip_to(data("xfadeC.flac"), err);
    const auto y = render_for(pl, static_cast<int>(2.5 * fs));
    bool finite = true;
    for (const float v : y) {
      if (!std::isfinite(v)) {
        finite = false;
      }
    }
    check("skip: two skips in a row end on the last song, with no bad sound",
          finite && pl.status().path.find("xfadeC") != std::string::npos, pl.status().path);
    std::string bad;
    check("skip: a file that cannot be played is refused and the song goes on",
          !pl.skip_to(data("../CMakeLists.txt"), bad) && !bad.empty() && pl.status().playing);
  }
  {
    // no crossfade: the next song starts where the last one stopped, with no gap
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(0);
    pl.load(data("xfadeA.flac"), err);
    check("a following song can be lined up",
          pl.queue_next(data("xfadeB.flac"), err) && pl.status().next_queued, err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    auto x = play_out(pl, 48000 * 8);
    const double seconds = static_cast<double>(x.size() / 2) / fs;
    // find the join: where the 440 Hz stops and the 880 Hz starts
    std::size_t join = 0;
    for (std::size_t i = 24000; i + 2400 < x.size() / 2; i += 480) {
      if (tone_part(x, i, i + 480, 880, fs) > tone_part(x, i, i + 480, 440, fs)) {
        join = i;
        break;
      }
    }
    // a gap would show as a run of silence
    std::size_t run = 0;
    std::size_t longest = 0;
    for (std::size_t i = 1000; i < static_cast<std::size_t>(5.5 * fs); i++) {
      if (std::fabs(x[i * 2]) < 1e-4F) {
        run++;
        longest = std::max(longest, run);
      } else {
        run = 0;
      }
    }
    check(
        "without one the two songs play back to back (gapless): the switch is at 3 s "
        "and there is no silence in between",
        std::fabs(static_cast<double>(join) / fs - 3.0) < 0.1 && longest < 20 &&
            pl.status().tags.title == "Song B",
        "join at " + std::to_string(join / fs) + " s, longest silence " + std::to_string(longest) +
            " frames, title " + pl.status().tags.title);
    (void)seconds;
  }
  {
    // one second of crossfade: 3 + 3 - 1 = 5 seconds in all; both tones are there in the middle,
    // and the loudness stays even
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(1.0);
    pl.load(data("xfadeA.flac"), err);
    pl.queue_next(data("xfadeB.flac"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const unsigned serial0 = pl.status().track_serial;
    auto x = play_out(pl, 48000 * 8);
    const double seconds = static_cast<double>(x.size() / 2) / fs;
    const auto t2 = static_cast<std::size_t>(2.0 * fs);  // the blend runs from 2.0 s to 3.0 s
    const double a_before = tone_part(x, t2 - 4800, t2 - 480, 440, fs);
    const double b_before = tone_part(x, t2 - 4800, t2 - 480, 880, fs);
    const double a_mid = tone_part(x, t2 + 21600, t2 + 26400, 440, fs);
    const double b_mid = tone_part(x, t2 + 21600, t2 + 26400, 880, fs);
    const double a_after = tone_part(x, t2 + 52800, t2 + 57600, 440, fs);
    const double b_after = tone_part(x, t2 + 52800, t2 + 57600, 880, fs);
    check("with 1 s of it the two songs take 5 s, not 6", std::fabs(seconds - 5.0) < 0.2,
          std::to_string(seconds));
    check(
        "before the blend only song A is heard, in the middle both are heard (0.7 of "
        "full each: 0.088 of 0.125), afterwards only song B",
        a_before > 0.11 && b_before < 0.01 && a_mid > 0.075 && a_mid < 0.10 && b_mid > 0.075 &&
            b_mid < 0.10 && a_after < 0.01 && b_after > 0.11,
        "A " + std::to_string(a_before) + " " + std::to_string(a_mid) + " " +
            std::to_string(a_after) + " B " + std::to_string(b_before) + " " +
            std::to_string(b_mid) + " " + std::to_string(b_after));
    double lo = 1e9;
    double hi = 0;
    for (std::size_t i = t2 - 9600; i + 2400 < static_cast<std::size_t>(3.0 * fs) + 9600;
         i += 2400) {
      const double r = rms(x, i, i + 2400);
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
    check(
        "the loudness stays even right through the blend (within 1.5 dB: the curves are "
        "equal-power)",
        hi / lo < 1.19, std::to_string(20 * std::log10(hi / lo)) + " dB");
    double worst_step = 0;
    for (std::size_t i = t2 - 2400; i < t2 + 52800 && i * 2 + 2 < x.size(); i++) {
      worst_step = std::max(worst_step, static_cast<double>(std::fabs(x[(i + 1) * 2] - x[i * 2])));
    }
    check(
        "no clicks (the biggest step between neighbouring samples through the blend is "
        "small)",
        worst_step < 0.15, std::to_string(worst_step));
    const Player::Status st = pl.status();
    check(
        "the song heard is now B (title, path), the change was announced once, and "
        "nothing is queued any more",
        st.tags.title == "Song B" && st.track_serial == serial0 + 1 && !st.next_queued &&
            st.path.find("xfadeB") != std::string::npos,
        st.tags.title + " serial " + std::to_string(st.track_serial) + " vs " +
            std::to_string(serial0));
  }
  {
    // the position follows the new song from the moment the blend starts
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(1.0);
    pl.load(data("xfadeA.flac"), err);
    pl.queue_next(data("xfadeB.flac"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    render_for(pl, static_cast<int>(2.3 * fs));  // 0.3 s into the blend
    const Player::Status st = pl.status();
    check("0.3 s into the blend the position shows song B at about 0.3 s, with B's length",
          st.tags.title == "Song B" && std::fabs(st.position - 0.3) < 0.08 &&
              std::fabs(st.duration - 3.0) < 0.05,
          st.tags.title + " " + std::to_string(st.position));
  }
  {
    // a crossfade longer than the songs is shortened to half of them; with three songs the second
    // blend works too
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(12);
    pl.load(data("xfadeA.flac"), err);
    pl.queue_next(data("xfadeB.flac"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto x = play_out(pl, 48000 * 4, 400);
    // song A is 3 s: the blend is 1.5 s long, from 1.5 s on; at 3 s B is playing alone for what is
    // left
    check(
        "a crossfade longer than the song is cut to half the song (A is heard alone "
        "only for the first 1.5 s)",
        tone_part(x, 48000 * 1, 48000 * 1 + 4800, 880, fs) < 0.01 &&
            tone_part(x, static_cast<std::size_t>(2.2 * fs),
                      static_cast<std::size_t>(2.2 * fs) + 4800, 880, fs) > 0.05);
    pl.set_crossfade(1.0);
    pl.load(data("xfadeA.flac"), err);
    pl.queue_next(data("xfadeB.flac"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    // when B starts being heard, line up C behind it (the way the window does)
    unsigned seen = pl.status().track_serial;
    std::vector<float> all;
    std::vector<float> buf(2 * 480);
    bool queued_c = false;
    bool ended = false;
    for (int done = 0; done < 48000 * 10 && !ended; done += 480) {
      pl.render(buf.data(), 480);
      all.insert(all.end(), buf.begin(), buf.end());
      std::this_thread::sleep_for(std::chrono::microseconds(400));
      const Player::Status st = pl.status();
      if (st.track_serial != seen) {
        seen = st.track_serial;
        if (st.tags.title == "Song B" && !queued_c) {
          queued_c = pl.queue_next(data("xfadeC.flac"), err);
        }
      }
      ended = pl.take_ended();
    }
    const double seconds = static_cast<double>(all.size() / 2) / fs;
    check(
        "three songs with 1 s blends take 3 + 3 + 3 - 2 = 7 s, and C is heard at the "
        "end",
        queued_c && std::fabs(seconds - 7.0) < 0.3 &&
            tone_part(all, static_cast<std::size_t>(6.2 * fs),
                      static_cast<std::size_t>(6.2 * fs) + 4800, 1320, fs) > 0.11 &&
            pl.status().tags.title == "Song C",
        std::to_string(seconds) + " s");
  }
  {
    // changing your mind: clear what was queued and the first song just ends
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(1.0);
    pl.load(data("xfadeA.flac"), err);
    pl.queue_next(data("xfadeB.flac"), err);
    pl.clear_next();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto x = play_out(pl, 48000 * 6);
    check("after clear_next song A plays on its own and ends at 3 s",
          !pl.status().next_queued && std::fabs(x.size() / 2 / fs - 3.0) < 0.15 &&
              tone_part(x, 48000 * 2 + 20000, 48000 * 2 + 24000, 880, fs) < 0.01,
          std::to_string(x.size() / 2 / fs));
  }
  {
    // with the speed changed the blend still works (the stretcher comes after it)
    Player pl(48000);
    Settings s = flat;
    s.speed = 1.5;
    pl.set_settings(s);
    pl.set_crossfade(1.0);
    pl.load(data("xfadeA.flac"), err);
    pl.queue_next(data("xfadeB.flac"), err);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto x = play_out(pl, 48000 * 6, 3000);
    const double seconds = static_cast<double>(x.size() / 2) / fs;
    double lo = 1e9;
    for (std::size_t i = 24000; i + 2400 < x.size() / 2 - 24000; i += 2400) {
      lo = std::min(lo, rms(x, i, i + 2400));
    }
    check(
        "at 1.5x speed the two songs take about (3 + 3 - 1) / 1.5 = 3.3 s and the sound "
        "never drops out",
        std::fabs(seconds - 5.0 / 1.5) < 0.3 && lo > 0.07 && pl.status().tags.title == "Song B",
        std::to_string(seconds) + " s, quietest " + std::to_string(lo));
  }
  {
    // a seek in the middle of a blend, a new song while one is queued, pausing and speed changes in
    // a hurry: nothing breaks
    Player pl(48000);
    pl.set_settings(flat);
    pl.set_crossfade(1.0);
    bool bad = false;
    std::mt19937 gen(8);
    const char* files[] = {"xfadeA.flac", "xfadeB.flac", "xfadeC.flac", "tone.ogg", "tone.mp3"};
    pl.load(data("xfadeA.flac"), err);
    for (int i = 0; i < 120; i++) {
      const int what = static_cast<int>(gen() % 9);
      if (what == 0) {
        pl.queue_next(data(files[gen() % 5]), err);
      } else if (what == 1) {
        pl.load(data(files[gen() % 5]), err);
      } else if (what == 2) {
        pl.seek((gen() % 300) / 100.0);
      } else if (what == 3) {
        pl.clear_next();
      } else if (what == 4) {
        pl.pause();
      } else if (what == 5) {
        pl.play();
      } else if (what == 6) {
        Settings s = flat;
        s.speed = dva::speeds[gen() % 6];
        pl.set_settings(s);
      } else if (what == 7) {
        pl.set_crossfade((gen() % 40) / 10.0);
      }
      const auto piece = render_for(pl, 2400, 480, 300);
      for (const float v : piece) {
        if (!std::isfinite(v)) {
          bad = true;
        }
      }
      pl.status();
    }
    check(
        "loading, queueing, seeking, pausing and changing speed and crossfade in a "
        "hurry stays sane (run under the sanitizers)",
        !bad);
  }
}

// Songs whose file names are not ASCII (Japanese, Chinese, accents). On Windows miniaudio's,
// stb_vorbis's and fopen's narrow-string functions cannot open these, so the decoder uses the wide
// versions there; ffmpeg is given UTF-8.
static void unicode_name_tests() {
  // written as UTF-8 bytes so the source file's encoding does not matter: U+97F3 U+697D (Japanese),
  // U+6062 U+590D (Chinese), and an accented e
  const std::string dir_name =
      "dva-unicode-\xE9\x9F\xB3\xE6\xA5\xBD-" + std::to_string(std::rand());
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / fh::path_from_utf8(dir_name);
  std::filesystem::create_directories(dir);
  for (const char* f : {"tone.wav", "tone.flac", "tone.mp3", "tone.ogg", "tone.m4a", "tone.opus"}) {
    const std::string name(f);
    const std::string ext = name.substr(name.rfind('.'));
    const std::filesystem::path copy =
        dir / fh::path_from_utf8("\xE6\x81\xA2\xE5\xA4\x8D caf\xC3\xA9" + ext);
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path(data(f)), copy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      check(std::string(f) + " can be copied to a non-ASCII name", false, ec.message());
      continue;
    }
    OpenResult r = open_decoder(copy, 48000);
    const Pcm p = r.decoder ? read_all(*r.decoder) : Pcm{};
    check(std::string("a ") + ext + " with a Japanese/Chinese/accented name opens and decodes",
          r.decoder && p.frames() > 20000, r.error);
    check(std::string("the tags of the ") + ext + " are read through that name too",
          read_tags(copy).title == read_tags(std::filesystem::path(data(f))).title);
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void play_tests() {
  test::section("tags");
  tag_tests();
  test::section("decode");
  decoder_tests();
  test::section("player");
  player_tests();
  test::section("crossfade");
  crossfade_tests();
  test::section("unicode");
  unicode_name_tests();
}
