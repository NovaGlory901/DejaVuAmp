// Windows' path::string() uses the ANSI code page and turns non-Latin names into '?', so always
// convert through these two. On Linux they do nothing.
#pragma once

#include <filesystem>
#include <string>

namespace fh {

inline std::filesystem::path path_from_utf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string path_to_utf8(const std::filesystem::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

}  // namespace fh
