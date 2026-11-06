// Finds the folder that holds a program's own folders (presets/, music/, ...) at run time.
#pragma once
#include <filesystem>
#include <system_error>

namespace fh {

// `start` itself if it holds a folder called `marker` (an installed copy: the folders sit next to
// the program); otherwise the nearest parent, up to `levels` levels up, that does (a build tree:
// <source>/build-release/program, with the folders in <source>); otherwise `start`.
inline std::filesystem::path find_root(const std::filesystem::path& start, const char* marker,
                                       int levels = 3) {
  std::error_code ec;
  std::filesystem::path dir = start;
  for (int i = 0; i <= levels; i++) {
    if (std::filesystem::is_directory(dir / marker, ec)) {
      return dir;
    }
    const std::filesystem::path parent = dir.parent_path();
    if (parent.empty() || parent == dir) {
      break;
    }
    dir = parent;
  }
  return start;
}

}  // namespace fh
