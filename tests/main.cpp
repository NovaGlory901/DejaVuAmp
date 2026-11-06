// All of DejaVuAmp's unit tests in one program. With no argument it runs every group; with a
// group name (core, theme, fuzz, dsp, play) it runs only that one.
#include <iostream>
#include <string>

#include "check.hpp"
#include "tests.hpp"

#ifndef DVA_GOLDEN
#define DVA_GOLDEN "tests/parity/core.golden"
#endif

namespace {
struct Group {
  const char* name;
  void (*run)();
};

const Group groups[] = {
    {"core", [] { core_tests(DVA_GOLDEN); }},
    {"theme", theme_tests},
    {"fuzz", fuzz_tests},
    {"dsp", dsp_tests},
    {"play", play_tests},
};
}  // namespace

int main(int argc, char* const* argv) {
  const std::string wanted = argc > 1 ? argv[1] : "";
  bool found = wanted.empty();
  for (const Group& g : groups) {
    if (wanted.empty() || wanted == g.name) {
      g.run();
      found = true;
    }
  }
  if (!found) {
    std::cerr << "unknown group '" << wanted << "' (core, theme, fuzz, dsp, play)\n";
    return 2;
  }
  return test::finish();
}
