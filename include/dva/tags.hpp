// Song details (title, artist, album) from a file's own tags: ID3v2/ID3v1 in MP3, Vorbis comments
// in FLAC and Ogg. Files are untrusted (bounds-checked reads, limited sizes); no tags just means
// empty text.
#pragma once

#include <filesystem>
#include <string>

namespace dva {

struct Tags {
  std::string title, artist, album;  // UTF-8, control characters removed, at most 200 bytes each
  [[nodiscard]] bool any() const { return !title.empty() || !artist.empty() || !album.empty(); }
};

Tags read_tags(const std::filesystem::path& file);

Tags parse_id3(const unsigned char* data,
               std::size_t size);  // the start of an MP3 file (ID3v2) or its last 128 bytes (ID3v1)
Tags parse_flac_metadata(const unsigned char* data, std::size_t size);  // from "fLaC" on
Tags tags_from_vorbis_comments(const char* const* comments,
                               int count);  // "TITLE=...", from stb_vorbis
std::string clean_tag_text(const std::string& s);

}  // namespace dva
