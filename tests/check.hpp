// The checking helper the tests share. check() prints one line per check and counts failures;
// finish() prints the summary and gives the process exit code.
#pragma once

#include <iostream>
#include <string>

namespace test {

inline int& failure_count() {
  static int count = 0;
  return count;
}

inline std::string& section_name() {
  static std::string name;
  return name;
}

// Names the group of checks that follow; it prefixes every check name.
inline void section(const std::string& name) { section_name() = name; }

inline void check(const std::string& name, bool ok, const std::string& detail = "") {
  if (!ok) {
    failure_count()++;
  }
  std::cout << (ok ? "ok  " : "BAD ") << section_name() << ": " << name;
  if (!ok && !detail.empty()) {
    std::cout << "   <- " << detail;
  }
  std::cout << "\n";
}

inline int finish() {
  if (failure_count() == 0) {
    std::cout << "all passed\n";
    return 0;
  }
  std::cout << "FAILED: " << failure_count() << "\n";
  return 1;
}

}  // namespace test
