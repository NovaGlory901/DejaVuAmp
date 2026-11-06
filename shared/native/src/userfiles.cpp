#include "fh/userfiles.hpp"
#include "fh/pathutf8.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <string>

namespace fh {

namespace fs = std::filesystem;

bool looks_like_picture(const fs::path& file) {
  std::string ext = path_to_utf8(file.extension());
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  static const char* known[] = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp",
                                ".tga", ".psd", ".pic",  ".pnm",  ".ppm", ".pgm"};
  return std::ranges::any_of(known, [&](const char* k) { return ext == k; });
}

std::vector<fs::path> list_pictures(const fs::path& folder) {
  return list_files(folder, looks_like_picture);
}

std::vector<fs::path> list_files(const fs::path& folder,
                                 const std::function<bool(const fs::path&)>& accept,
                                 bool recursive) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!recursive) {
    const fs::directory_iterator it(folder, ec);
    if (ec) {
      return out;
    }
    for (const auto& e : it) {
      std::error_code ec2;
      if (e.is_regular_file(ec2) && accept(e.path())) {
        out.push_back(e.path());
      }
    }
  } else {
    constexpr int max_depth = 4;
    constexpr std::size_t max_files = 5000;
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
      return out;
    }
    while (it != end && out.size() < max_files) {
      std::error_code ec2;
      if (it->is_directory(ec2) && it.depth() >= max_depth) {
        it.disable_recursion_pending();
      } else if (it->is_regular_file(ec2) && accept(it->path())) {
        out.push_back(it->path());
      }
      it.increment(ec2);
      if (ec2) {
        break;
      }
    }
  }
  std::ranges::sort(out);
  return out;
}

FolderWatcher::Changes FolderWatcher::poll() {
  Changes ch;
  std::map<fs::path, Stamp> now;
  for (const fs::path& f : list_files(folder_, accept_, recursive_)) {
    std::error_code ec;
    Stamp s;
    s.size = fs::file_size(f, ec);
    if (ec) {
      continue;  // vanished between listing and looking
    }
    const auto t = fs::last_write_time(f, ec);
    if (ec) {
      continue;
    }
    s.modified = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count());
    now[f] = s;
  }
  for (const auto& [path, stamp] : now) {
    const auto it = known_.find(path);
    if (it == known_.end()) {
      ch.added.push_back(path);
    } else if (!(it->second == stamp)) {
      ch.changed.push_back(path);
    }
  }
  for (const auto& [path, stamp] : known_) {
    if (!now.contains(path)) {
      ch.removed.push_back(path);
    }
  }
  known_ = std::move(now);
  return ch;
}

}  // namespace fh
