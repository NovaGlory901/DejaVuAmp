// Damaged songs in, a clean answer out: the small real songs in tests/data and the tag readers'
// inputs are damaged in many ways and played (open, read to the end, seek, read again, tags). Each
// must answer and never crash, hang, read outside its input, or return a sample that is not a
// number or more sound than the file could hold. Fixed seed (DVA_FUZZ_SEED); DVA_FUZZ_SECONDS runs
// longer (default 20). Build with DVA_SANITIZE=ON in Release (Debug is too slow to judge hangs). A
// failing input is kept in the temp folder; after a crash the single case.* file left there is the
// culprit.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dva/decoder.hpp"
#include "dva/tags.hpp"

#ifndef DVA_DATA
#define DVA_DATA "tests/data"
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Clock = std::chrono::steady_clock;

namespace {
std::mt19937_64 rng;
long long cases = 0, opened = 0, tag_cases = 0, problems = 0;
fs::path dir;

Bytes read_file(const std::string& name) {
  std::ifstream in(std::string(DVA_DATA) + "/" + name, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void write_file(const fs::path& p, const Bytes& b) {
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
std::size_t pick(std::size_t n) { return n ? static_cast<std::size_t>(rng() % n) : 0; }
// biased towards the start of the file, where the headers and tags are
std::size_t position(const Bytes& b) {
  return b.empty() ? 0
                   : (rng() % 3 == 0 ? pick(b.size()) : pick(std::min<std::size_t>(b.size(), 400)));
}

Bytes damage(Bytes b) {
  const int edits = 1 + static_cast<int>(rng() % 6);
  for (int i = 0; i < edits && !b.empty(); ++i) {
    switch (rng() % 10) {
      case 0:
        b[position(b)] = static_cast<unsigned char>(rng());
        break;
      case 1:
        b.resize(pick(b.size() + 1));
        break;
      case 2: {
        const std::size_t at = position(b);
        for (std::size_t k = at; k < std::min(b.size(), at + 1 + pick(32)); ++k) {
          b[k] = 0xFF;
        }
      } break;
      case 3: {
        const std::size_t at = position(b);
        for (std::size_t k = at; k < std::min(b.size(), at + 1 + pick(32)); ++k) {
          b[k] = 0x00;
        }
      } break;
      case 4: {
        const std::size_t at = position(b);
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), 1 + pick(8),
                 static_cast<unsigned char>(rng()));
      } break;
      case 5: {
        const std::size_t at = position(b);
        const std::size_t n = std::min<std::size_t>(1 + pick(64), b.size() - at);
        b.erase(b.begin() + static_cast<std::ptrdiff_t>(at),
                b.begin() + static_cast<std::ptrdiff_t>(at + n));
      } break;
      case 6: {  // a 32-bit field in the header area set to something huge (sizes, counts, lengths)
        if (b.size() >= 8) {
          const std::size_t at = pick(std::min<std::size_t>(b.size() - 4, 200));
          const unsigned char v[] = {0xFF, 0xFF, 0xFF, 0x7F, 0x80, 0x00, 0x00, 0x00};
          const std::size_t o = pick(5);
          for (int k = 0; k < 4; ++k) {
            b[at + k] = v[o + k];
          }
        }
      } break;
      case 7: {  // a piece of the file repeated
        const std::size_t at = pick(b.size());
        const std::size_t n = std::min<std::size_t>(1 + pick(512), b.size() - at);
        Bytes piece(b.begin() + static_cast<std::ptrdiff_t>(at),
                    b.begin() + static_cast<std::ptrdiff_t>(at + n));
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(pick(b.size() + 1)), piece.begin(),
                 piece.end());
      } break;
      case 8:
        b[pick(b.size())] ^= static_cast<unsigned char>(1U << (rng() % 8));
        break;
      default: {
        const std::size_t a = position(b);
        const std::size_t c = pick(b.size());
        std::swap(b[a], b[c]);
      }
    }
  }
  return b;
}

// Both directions of one file, kept for the person to look at when something goes wrong.
void keep(const Bytes& b, const std::string& ext, const std::string& why) {
  const fs::path p = dir / ("failure-" + std::to_string(++problems) + ext);
  write_file(p, b);
  std::cout << "BAD " << why << "   <- input kept at " << p << "\n";
}

void play(const fs::path& file, const Bytes& original, const std::string& ext) {
  ++cases;
  const auto start = Clock::now();
  static const double rates[] = {44100, 48000, 8000, 22050, 96000, 192000};
  const double rate = rates[rng() % 6];
  dva::OpenResult r = dva::open_decoder(file, rate);
  if (!r.decoder && r.error.empty()) {
    keep(original, ext, "a file was refused without a reason");
  }
  if (r.decoder) {
    ++opened;
    long long total = 0;
    std::vector<float> buf(2 * 1024);
    const long long cap = static_cast<long long>(rate) * 60 *
                          10;  // ten minutes of sound is far more than any of these files can hold
    bool finite = true;
    for (int pass = 0; pass < 3; ++pass) {
      for (;;) {
        const long long n = r.decoder->read(buf.data(), 1024);
        if (n < 0 || n > 1024) {
          keep(original, ext, "read() returned " + std::to_string(n) + " for 1024 frames");
          break;
        }
        if (n == 0) {
          break;
        }
        for (long long i = 0; i < n * 2; ++i) {
          if (!std::isfinite(buf[static_cast<std::size_t>(i)])) {
            finite = false;
          }
        }
        total += n;
        if (total > cap) {
          keep(original, ext, "a tiny file gave more than ten minutes of sound");
          break;
        }
      }
      if (pass == 0) {
        const long long t = r.decoder->total_frames();
        r.decoder->seek(t > 0 ? static_cast<long long>(rng() % static_cast<unsigned long long>(t))
                              : static_cast<long long>(rng() % 100000));
      } else {
        r.decoder->seek(-1 - static_cast<long long>(rng() % 5));
      }
    }
    r.decoder->tags();
    r.decoder->format();
    if (!finite) {
      keep(original, ext, "a sample that is not a number");
    }
  }
  dva::read_tags(file);
  const double secs = std::chrono::duration<double>(Clock::now() - start).count();
  if (secs > 20) {
    keep(original, ext, "took " + std::to_string(secs) + " s (hang?)");
  }
}

// Tag blocks made from nothing, with sizes that lie.
Bytes id3_block() {
  Bytes b = {'I', 'D',
             '3', static_cast<unsigned char>(2 + rng() % 3),
             0,   static_cast<unsigned char>(rng() & 0xFF)};
  for (int i = 0; i < 4; ++i) {
    b.push_back(static_cast<unsigned char>(rng() & 0x7F));  // syncsafe size
  }
  static const char* ids[] = {"TIT2", "TPE1", "TALB", "TT2", "TP1", "TAL", "APIC", "COMM", "TXXX"};
  for (int f = 0, n = static_cast<int>(rng() % 6); f < n; ++f) {
    const char* id = ids[rng() % 9];
    for (int i = 0; i < 4; ++i) {
      b.push_back(static_cast<unsigned char>(id[i]));
    }
    const std::size_t len = rng() % 4 == 0 ? rng() % 0xFFFFFFFFULL : rng() % 40;
    for (int i = 3; i >= 0; --i) {
      b.push_back(static_cast<unsigned char>(len >> (8 * i)));
    }
    b.push_back(static_cast<unsigned char>(rng() & 0xFF));
    b.push_back(static_cast<unsigned char>(rng() & 0xFF));
    b.push_back(static_cast<unsigned char>(rng() % 5));
    for (std::size_t i = 0, m = rng() % 40; i < m; ++i) {
      b.push_back(static_cast<unsigned char>(rng()));
    }
  }
  return b;
}
Bytes flac_block() {
  Bytes b = {'f', 'L', 'a', 'C'};
  for (int blocks = 0, n = 1 + static_cast<int>(rng() % 4); blocks < n; ++blocks) {
    const std::size_t len = rng() % 4 == 0 ? rng() % 0xFFFFFF : rng() % 80;
    b.push_back(static_cast<unsigned char>((blocks == n - 1 ? 0x80 : 0) |
                                           (rng() % 3 == 0 ? 4 : rng() % 8)));
    b.push_back(static_cast<unsigned char>(len >> 16));
    b.push_back(static_cast<unsigned char>(len >> 8));
    b.push_back(static_cast<unsigned char>(len));
    for (std::size_t i = 0, m = std::min<std::size_t>(len, 120); i < m; ++i) {
      b.push_back(static_cast<unsigned char>(rng() % 5 ? ((i % 11 == 0) ? 0xFF : rng()) : '='));
    }
  }
  return b;
}
void tag_inputs() {
  for (int i = 0; i < 2000; ++i) {
    ++tag_cases;
    Bytes a = id3_block();
    Bytes c = flac_block();
    if (rng() % 2) {
      a = damage(a);
    }
    if (rng() % 2) {
      c = damage(c);
    }
    dva::parse_id3(a.data(), a.size());
    dva::parse_flac_metadata(c.data(), c.size());
    Bytes v1(128 + pick(40));
    for (auto& x : v1) {
      x = static_cast<unsigned char>(rng());
    }
    if (v1.size() >= 128) {
      v1[v1.size() - 128] = 'T';
      v1[v1.size() - 127] = 'A';
      v1[v1.size() - 126] = 'G';
    }
    dva::parse_id3(v1.data(), v1.size());
    std::string s(pick(300), '\0');
    for (auto& ch : s) {
      ch = static_cast<char>(rng());
    }
    const std::string cleaned = dva::clean_tag_text(s);
    if (cleaned.size() > 200) {
      keep(Bytes(s.begin(), s.end()), ".txt",
           "cleanTagText returned " + std::to_string(cleaned.size()) + " bytes");
    }
    std::vector<std::string> store;
    std::vector<const char*> comments;
    for (int k = 0, n = static_cast<int>(rng() % 6); k < n; ++k) {
      std::string t(pick(60), 'a');
      for (auto& ch : t) {
        ch = static_cast<char>(rng() % 3 ? 'a' + rng() % 26 : rng() % 256);
      }
      if (!t.empty() && rng() % 2) {
        t[pick(t.size())] = '=';
      }
      store.push_back(t);
    }
    comments.reserve(store.size());
    for (const auto& t : store) {
      comments.push_back(t.c_str());
    }
    comments.push_back(nullptr);
    dva::tags_from_vorbis_comments(comments.data(), static_cast<int>(comments.size()));
    dva::tags_from_vorbis_comments(nullptr, 5);
  }
}
}  // namespace

std::atomic<long long> case_started_ms{0};  // when the song being played started (0 = none)
long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

int main() {
  // A loop that never ends cannot report itself: this thread ends the run when one song takes
  // longer than 60 s. The song is still in the folder (case.*).
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const long long t = case_started_ms.load();
      if (t && now_ms() - t > 60000) {
        std::cout << "BAD a song took more than 60 s (hang): the input is the one file left in "
                  << dir << std::endl;
        std::_Exit(3);
      }
    }
  }).detach();
  const char* s = std::getenv("DVA_FUZZ_SEED");
  rng.seed(s ? std::strtoull(s, nullptr, 10) : 20261012ULL);
  const char* t = std::getenv("DVA_FUZZ_SECONDS");
  const double budget = t ? std::atof(t) : 20.0;
  dir = fs::temp_directory_path() /
        ("dva-media-fuzz-" + std::to_string(static_cast<unsigned long long>(rng() % 1000000)));
  fs::create_directories(dir);

  struct Seed {
    std::string name, ext;
    Bytes data;
  };
  std::vector<Seed> seeds;
  for (const char* f : {"tone.wav", "mono22k.wav", "tone-float.wav", "tone48-24bit.wav",
                        "tone.flac", "tone.mp3", "tone-v24.mp3", "tone.ogg", "tone32k.ogg",
                        "tone.m4a", "tone-alac.m4a", "tone.opus", "tone.wma", "tone.aac"}) {
    const std::string name = f;
    const std::string ext = name.substr(name.rfind('.'));
    const bool ff = ext == ".m4a" || ext == ".opus" || ext == ".wma" || ext == ".aac";
    if (ff && !dva::ffmpeg_supported()) {
      continue;
    }
    Bytes b = read_file(name);
    if (b.empty()) {
      std::cout << "BAD missing seed file " << name << "\n";
      return 1;
    }
    seeds.push_back({name, ext, std::move(b)});
  }
  std::cout << seeds.size() << " seed songs, " << budget << " s, seed " << (s ? s : "20261012")
            << ", in " << dir << "\n";

  // Inputs that once crashed or hung the program, kept as they were found (tests/data/damaged):
  // each is played at every rate with a seek in the middle, and none may crash, hang (the watchdog
  // above) or give a sample that is not a number.
  {
    const long long before = problems;
    for (const char* name : {"comment-length-overflow.ogg", "seek-after-error.flac",
                             "fact-chunk-too-small.wav", "format-changes-midstream.aac"}) {
      const Bytes data = read_file(std::string("damaged/") + name);
      if (data.empty()) {
        std::cout << "BAD missing " << name << "\n";
        return 1;
      }
      const std::string ext = std::string(name).substr(std::string(name).rfind('.'));
      for (const double rate : {44100.0, 48000.0, 8000.0, 22050.0, 96000.0, 192000.0}) {
        for (const long long at : {0LL, 700LL, 5000LL, 20000LL, 30000LL, 47999LL}) {
          const fs::path file = dir / ("known" + ext);
          write_file(file, data);
          case_started_ms = now_ms();
          {  // close the file before removing it: Windows cannot delete an open file
            dva::OpenResult r = dva::open_decoder(file, rate);
            if (r.decoder) {
              std::vector<float> buf(2048);
              for (int pass = 0; pass < 3; ++pass) {
                long long total = 0;
                long long n = 0;
                while ((n = r.decoder->read(buf.data(), 1024)) > 0) {
                  total += n;
                  if (total > static_cast<long long>(rate) * 600) {
                    break;
                  }
                }
                r.decoder->seek(pass == 0 ? at : -3);
              }
            }
          }
          case_started_ms = 0;
          fs::remove(file);
        }
      }
      std::cout << "ok   known bad input " << name << " played at 6 rates and 6 seek points\n";
    }
    if (problems != before) {
      std::cout << "BAD a known bad input misbehaved\n";
    }
  }

  const auto begin = Clock::now();
  tag_inputs();
  while (std::chrono::duration<double>(Clock::now() - begin).count() < budget) {
    const Seed& a = seeds[pick(seeds.size())];
    Bytes b = rng() % 8 == 0 ? Bytes() : damage(a.data);
    std::string ext = a.ext;
    if (rng() % 8 == 0) {  // one format's body behind another's header and name
      const Seed& o = seeds[pick(seeds.size())];
      b = o.data;
      b.resize(std::max<std::size_t>(4, std::min(b.size(), pick(300) + 4)));
      Bytes rest = a.data;
      if (rest.size() > 100) {
        b.insert(b.end(), rest.begin() + static_cast<std::ptrdiff_t>(pick(rest.size() / 2)),
                 rest.end());
      }
      b = damage(b);
      ext = rng() % 2 ? a.ext : o.ext;
    } else if (rng() % 16 == 0) {  // a tag block with lying sizes in front of a real song
      Bytes head = rng() % 2 ? id3_block() : flac_block();
      head = damage(head);
      head.insert(head.end(), a.data.begin(), a.data.end());
      b = head;
    }
    if (rng() % 10 == 0) {
      b.assign(pick(4000), 0);
      for (auto& x : b) {
        x = static_cast<unsigned char>(rng());
      }
    }
    const fs::path file = dir / ("case" + ext);
    write_file(file, b);
    case_started_ms = now_ms();
    play(file, b, ext);
    case_started_ms = 0;
    fs::remove(
        file);  // so that after a crash the one file left in the folder is the input that did it
  }
  std::error_code ignored;
  if (!problems) {
    fs::remove_all(dir, ignored);
  }
  std::cout << cases << " damaged songs played (" << opened << " still opened), " << tag_cases
            << " tag inputs: "
            << (problems ? "PROBLEMS: " + std::to_string(problems)
                         : std::string("no crash, no hang, no bad sample"))
            << "\n";
  return problems ? 1 : 0;
}
