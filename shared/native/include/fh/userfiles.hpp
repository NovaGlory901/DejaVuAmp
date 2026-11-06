// The user's own pictures: backgrounds/ (one at a time) and artifacts/ (floating on top).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>

namespace fh {

// True for a file name that looks like a picture this program reads: png jpg jpeg webp gif bmp tga
// psd pic pnm ppm pgm (case does not matter).
bool looks_like_picture(const std::filesystem::path& file);

// The picture files directly inside `folder`, sorted by name. Does not open them. A missing folder
// gives an empty list.
std::vector<std::filesystem::path> list_pictures(const std::filesystem::path& folder);

// The files directly inside `folder` that `accept` likes, sorted by name. Does not open them. A
// missing folder gives an empty list. With `recursive`, folders inside it are searched too (down to
// 4 levels, never following links, at most 5000 files); the sort keeps each folder's files
// together.
std::vector<std::filesystem::path> list_files(
    const std::filesystem::path& folder,
    const std::function<bool(const std::filesystem::path&)>& accept, bool recursive = false);

// Watches a folder for pictures coming and going (and being replaced). Call poll() now and then;
// each call says what changed since the last one. The first call reports everything already there
// as added. A file that is still being copied in looks "changed" on each poll until it stops
// growing, so a caller that fails to read a file should simply try again when it is reported as
// changed.
class FolderWatcher {
 public:
  using Accept = std::function<bool(const std::filesystem::path&)>;
  explicit FolderWatcher(std::filesystem::path folder, Accept accept = looks_like_picture,
                         bool recursive = false)
      : folder_(std::move(folder)),
        accept_(accept ? std::move(accept) : Accept(looks_like_picture)),
        recursive_(recursive) {}

  struct Changes {
    std::vector<std::filesystem::path> added, removed, changed;
    [[nodiscard]] bool any() const {
      return !added.empty() || !removed.empty() || !changed.empty();
    }
  };
  Changes poll();

  [[nodiscard]] const std::filesystem::path& folder() const { return folder_; }

 private:
  struct Stamp {
    std::uintmax_t size = 0;
    long long modified = 0;
    bool operator==(const Stamp&) const = default;
  };
  std::filesystem::path folder_;
  Accept accept_;
  bool recursive_ = false;
  std::map<std::filesystem::path, Stamp> known_;
};

}  // namespace fh
