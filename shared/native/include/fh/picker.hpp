// Port of PresetPicker in js/falsehorizon-core.js, the reference (tests/parity/picker.golden).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fh {

class PresetPicker {
 public:
  using Rng = std::function<double()>;  // 0 <= value < 1; empty means a random one

  explicit PresetPicker(Rng rng = {});

  void show(const std::string& name);  // forgets any "ahead" history after stepping back
  std::optional<std::string> next(
      const std::vector<std::string>& pool);  // nothing if the pool is empty
  std::optional<std::string> prev();  // nothing if there is nothing before this one

  [[nodiscard]] const std::vector<std::string>& history() const {
    return history_;
  }  // at most 100, oldest first
  [[nodiscard]] int index() const {
    return i_;
  }  // which of the history is showing; -1 before the first

 private:
  Rng rng_;
  std::vector<std::string> history_;
  int i_ = -1;
};

}  // namespace fh
