#include "fh/picker.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

namespace fh {

PresetPicker::PresetPicker(Rng rng) : rng_(std::move(rng)) {
  if (!rng_) {
    const auto gen = std::make_shared<std::mt19937_64>(std::random_device{}());
    rng_ = [gen] { return std::uniform_real_distribution<double>(0, 1)(*gen); };
  }
}

void PresetPicker::show(const std::string& name) {
  history_.resize(static_cast<std::size_t>(i_ + 1));
  history_.push_back(name);
  if (history_.size() > 100) {
    history_.erase(history_.begin());
  }
  i_ = static_cast<int>(history_.size()) - 1;
}

std::optional<std::string> PresetPicker::next(const std::vector<std::string>& pool) {
  if (i_ < static_cast<int>(history_.size()) - 1) {
    return history_[static_cast<std::size_t>(++i_)];
  }
  if (pool.empty()) {
    return std::nullopt;
  }
  const std::size_t keep =
      std::min<std::size_t>(4, pool.size() - 1);  // the last few shown are not picked again
  const std::size_t from = history_.size() > keep ? history_.size() - keep : 0;
  std::vector<std::string> fresh;
  for (const std::string& n : pool) {
    if (std::find(history_.begin() + static_cast<std::ptrdiff_t>(from), history_.end(), n) ==
        history_.end()) {
      fresh.push_back(n);
    }
  }
  const std::vector<std::string>& from2 = fresh.empty() ? pool : fresh;
  const std::size_t at =
      std::min(from2.size() - 1,
               static_cast<std::size_t>(std::floor(rng_() * static_cast<double>(from2.size()))));
  std::string name = from2[at];
  show(name);
  return name;
}

std::optional<std::string> PresetPicker::prev() {
  if (i_ <= 0) {
    return std::nullopt;
  }
  return history_[static_cast<std::size_t>(--i_)];
}

}  // namespace fh
