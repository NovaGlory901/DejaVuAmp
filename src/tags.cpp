#include "dva/tags.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace dva {

namespace {

constexpr std::size_t max_text = 200;
constexpr std::size_t read_limit = 1U
                                   << 20;  // how much of the start of a file is looked at for tags

void append_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

std::string from_latin1(const unsigned char* p, std::size_t n) {
  std::string out;
  for (std::size_t i = 0; i < n; i++) {
    append_utf8(out, p[i]);
  }
  return out;
}

std::string from_utf16(const unsigned char* p, std::size_t n, bool big_endian, bool bom_allowed) {
  std::string out;
  std::size_t i = 0;
  if (bom_allowed && n >= 2) {
    if (p[0] == 0xFF && p[1] == 0xFE) {
      big_endian = false;
      i = 2;
    } else if (p[0] == 0xFE && p[1] == 0xFF) {
      big_endian = true;
      i = 2;
    }
  }
  const auto unit = [&](std::size_t at) {
    return static_cast<char32_t>(big_endian ? (p[at] << 8) | p[at + 1] : (p[at + 1] << 8) | p[at]);
  };
  for (; i + 1 < n; i += 2) {
    char32_t c = unit(i);
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
      const char32_t d = unit(i + 2);
      if (d >= 0xDC00 && d < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        i += 2;
      } else {
        c = 0xFFFD;
      }
    } else if (c >= 0xD800 && c < 0xE000) {
      c = 0xFFFD;
    }
    append_utf8(out, c);
  }
  return out;
}

// Text in the encoding an ID3 frame says (0 Latin-1, 1 UTF-16 with a byte order mark, 2 UTF-16 big
// endian, 3 UTF-8), up to its end.
std::string decode_id3_text(unsigned encoding, const unsigned char* p, std::size_t n) {
  switch (encoding) {
    case 0:
      return from_latin1(p, n);
    case 1:
      return from_utf16(p, n, false, true);
    case 2:
      return from_utf16(p, n, true, false);
    default:
      return std::string(reinterpret_cast<const char*>(p), n);
  }
}

// A text frame holds one or more strings separated by a NUL (two bytes for UTF-16): joined with ",
// ".
std::string text_frame(const unsigned char* p, std::size_t n) {
  if (n < 1) {
    return "";
  }
  const unsigned enc = p[0];
  const bool wide = enc == 1 || enc == 2;
  std::string joined;
  std::size_t i = 1;
  std::size_t start = 1;
  const auto flush = [&](std::size_t end) {
    const std::string part = clean_tag_text(decode_id3_text(enc, p + start, end - start));
    if (!part.empty()) {
      if (!joined.empty()) {
        joined += ", ";
      }
      joined += part;
    }
  };
  while (i < n) {
    if (wide ? (i + 1 < n && p[i] == 0 && p[i + 1] == 0) : p[i] == 0) {
      flush(i);
      i += wide ? 2 : 1;
      start = i;
    } else {
      i += wide ? 2 : 1;
    }
  }
  if (start < n) {
    flush(std::min(i, n));
  }
  return joined;
}

std::uint32_t syncsafe(const unsigned char* p) {
  return (static_cast<std::uint32_t>(p[0] & 0x7F) << 21) |
         (static_cast<std::uint32_t>(p[1] & 0x7F) << 14) |
         (static_cast<std::uint32_t>(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}
std::uint32_t be32(const unsigned char* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::uint32_t le32(const unsigned char* p) {
  return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
}

std::vector<unsigned char> remove_unsync(const unsigned char* p, std::size_t n) {
  std::vector<unsigned char> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; i++) {
    out.push_back(p[i]);
    if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0) {
      i++;
    }
  }
  return out;
}

Tags parse_id3v1(const unsigned char* p) {
  const auto field = [&](std::size_t at, std::size_t len) {
    std::size_t n = 0;
    while (n < len && p[at + n] != 0) {
      n++;
    }
    return clean_tag_text(from_latin1(p + at, n));
  };
  Tags t;
  t.title = field(3, 30);
  t.artist = field(33, 30);
  t.album = field(63, 30);
  return t;
}

Tags parse_id3v2(const unsigned char* d, std::size_t size) {
  Tags t;
  if (size < 10 || std::memcmp(d, "ID3", 3) != 0) {
    return t;
  }
  const int version = d[3];
  if (version < 2 || version > 4) {
    return t;
  }
  const unsigned flags = d[5];
  if ((d[6] | d[7] | d[8] | d[9]) & 0x80) {
    return t;
  }
  const std::size_t tag_size = syncsafe(d + 6);
  const std::size_t avail = std::min(tag_size, size - 10);
  std::vector<unsigned char> body;
  const unsigned char* p = d + 10;
  std::size_t n = avail;
  if (flags & 0x80) {
    body = remove_unsync(p, n);
    p = body.data();
    n = body.size();
  }  // the whole tag was unsynchronised
  std::size_t at = 0;
  if (flags & 0x40) {  // an extended header comes first: skip it
    if (n < 4) {
      return t;
    }
    const std::size_t ext = version == 4 ? syncsafe(p) : static_cast<std::size_t>(be32(p)) + 4;
    if (ext < 4 || ext > n) {
      return t;
    }
    at = ext;
  }
  const std::size_t head_len = version == 2 ? 6 : 10;
  while (at + head_len <= n) {
    if (p[at] == 0) {
      break;
    }
    const std::string id(reinterpret_cast<const char*>(p + at), version == 2 ? 3 : 4);
    std::size_t len = 0;
    unsigned frame_flags = 0;
    if (version == 2) {
      len = (static_cast<std::size_t>(p[at + 3]) << 16) |
            (static_cast<std::size_t>(p[at + 4]) << 8) | p[at + 5];
    } else {
      len = version == 4 ? syncsafe(p + at + 4) : be32(p + at + 4);
      frame_flags = (static_cast<unsigned>(p[at + 8]) << 8) | p[at + 9];
    }
    at += head_len;
    if (len > n - at) {
      break;  // cut off (or damaged): stop here
    }
    const unsigned char* fp = p + at;
    std::size_t flen = len;
    std::vector<unsigned char> unsynced;
    if (version == 4 && (frame_flags & 0x0002)) {
      unsynced = remove_unsync(fp, flen);
      fp = unsynced.data();
      flen = unsynced.size();
    }
    if (version == 4 && (frame_flags & 0x0001)) {
      if (flen >= 4) {
        fp += 4;
        flen -= 4;
      } else {
        flen = 0;
      }
    }  // data length indicator
    const bool compressed_or_encrypted =
        version >= 3 && (frame_flags & (version == 3 ? 0x00C0 : 0x000C));
    if (!compressed_or_encrypted) {
      if (id == "TIT2" || id == "TT2") {
        if (t.title.empty()) {
          t.title = text_frame(fp, flen);
        }
      } else if (id == "TPE1" || id == "TP1") {
        if (t.artist.empty()) {
          t.artist = text_frame(fp, flen);
        }
      } else if (id == "TALB" || id == "TAL") {
        if (t.album.empty()) {
          t.album = text_frame(fp, flen);
        }
      }
    }
    at += len;
  }
  return t;
}

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

void take_comment(Tags& t, const std::string& comment) {
  const std::size_t eq = comment.find('=');
  if (eq == std::string::npos) {
    return;
  }
  const std::string key = lower(comment.substr(0, eq));
  const std::string value = clean_tag_text(comment.substr(eq + 1));
  if (key == "title" && t.title.empty()) {
    t.title = value;
  } else if (key == "artist" && t.artist.empty()) {
    t.artist = value;
  } else if (key == "album" && t.album.empty()) {
    t.album = value;
  }
}

bool read_file_part(const std::filesystem::path& file, std::uintmax_t from, std::size_t count,
                    std::vector<unsigned char>& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(static_cast<std::streamoff>(from));
  out.resize(count);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
  out.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
  return true;
}

}  // namespace

std::string clean_tag_text(const std::string& s) {
  // valid UTF-8 only (anything else becomes U+FFFD), no control characters, trimmed, and not more
  // than max_text bytes
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    const std::size_t len = c < 0x80                  ? 1
                            : (c >= 0xC2 && c < 0xE0) ? 2
                            : (c >= 0xE0 && c < 0xF0) ? 3
                            : (c >= 0xF0 && c < 0xF5) ? 4
                                                      : 0;
    bool good = len > 0 && i + len <= s.size();
    for (std::size_t k = 1; good && k < len; k++) {
      good = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    }
    if (good && len == 3) {
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      good = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);
    }
    if (good && len == 4) {
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      good = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
    }
    if (!good) {
      out += "\xEF\xBF\xBD";
      i++;
      continue;
    }
    if (len == 1 && (c <= 0x20 || c == 0x7F)) {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
    } else {
      out.append(s, i, len);
    }
    i += len;
  }
  std::size_t a = 0;
  std::size_t b = out.size();
  while (a < b && out[a] == ' ') {
    a++;
  }
  while (b > a && out[b - 1] == ' ') {
    b--;
  }
  out = out.substr(a, b - a);
  if (out.size() > max_text) {
    std::size_t cut = max_text;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      cut--;
    }
    out.resize(cut);
  }
  return out;
}

Tags parse_id3(const unsigned char* data, std::size_t size) {
  if (size >= 10 && std::memcmp(data, "ID3", 3) == 0) {
    return parse_id3v2(data, size);
  }
  if (size >= 128 && std::memcmp(data + size - 128, "TAG", 3) == 0) {
    return parse_id3v1(data + size - 128);
  }
  return {};
}

Tags parse_flac_metadata(const unsigned char* d, std::size_t size) {
  Tags t;
  if (size < 4 || std::memcmp(d, "fLaC", 4) != 0) {
    return t;
  }
  std::size_t at = 4;
  while (at + 4 <= size) {
    const bool last = d[at] & 0x80;
    const unsigned type = d[at] & 0x7F;
    const std::size_t len = (static_cast<std::size_t>(d[at + 1]) << 16) |
                            (static_cast<std::size_t>(d[at + 2]) << 8) | d[at + 3];
    at += 4;
    if (len > size - at) {
      break;
    }
    if (type == 4) {
      const unsigned char* p = d + at;
      std::size_t pos = 0;
      if (len >= 4) {
        const std::size_t vendor = le32(p);
        pos = 4;
        if (vendor <= len - pos) {
          pos += vendor;
        } else {
          pos = len;
        }
        if (pos + 4 <= len) {
          std::uint32_t count = le32(p + pos);
          pos += 4;
          for (std::uint32_t i = 0; i < count && i < 1000 && pos + 4 <= len; i++) {
            const std::size_t clen = le32(p + pos);
            pos += 4;
            if (clen > len - pos) {
              break;
            }
            take_comment(t, std::string(reinterpret_cast<const char*>(p + pos), clen));
            pos += clen;
          }
        }
      }
    }
    at += len;
    if (last) {
      break;
    }
  }
  return t;
}

Tags tags_from_vorbis_comments(const char* const* comments, int count) {
  Tags t;
  for (int i = 0; comments && i < count && i < 1000; i++) {
    if (comments[i]) {
      take_comment(t, std::string(comments[i], strnlen(comments[i], 4096)));
    }
  }
  return t;
}

Tags read_file_tags(const std::filesystem::path& file);  // for Ogg (vorbis.cpp)

Tags read_tags(const std::filesystem::path& file) {
  std::vector<unsigned char> head;
  if (!read_file_part(file, 0, read_limit, head) || head.size() < 4) {
    return {};
  }
  Tags t;
  if (std::memcmp(head.data(), "fLaC", 4) == 0) {
    return parse_flac_metadata(head.data(), head.size());
  }
  if (std::memcmp(head.data(), "OggS", 4) == 0) {
    return read_file_tags(file);
  }
  if (std::memcmp(head.data(), "ID3", 3) == 0) {
    t = parse_id3(head.data(), head.size());
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (!ec && size >= 128 && (t.title.empty() || t.artist.empty() || t.album.empty())) {
    std::vector<unsigned char> tail;
    if (read_file_part(file, size - 128, 128, tail) && tail.size() == 128 &&
        std::memcmp(tail.data(), "TAG", 3) == 0) {
      const Tags v1 = parse_id3v1(tail.data());
      if (t.title.empty()) {
        t.title = v1.title;
      }
      if (t.artist.empty()) {
        t.artist = v1.artist;
      }
      if (t.album.empty()) {
        t.album = v1.album;
      }
    }
  }
  return t;
}

}  // namespace dva
