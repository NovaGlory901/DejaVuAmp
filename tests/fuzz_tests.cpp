// Junk in, a clean answer out: feeds random and damaged inputs (fixed seed) to the readers that
// take files from people (.eqf, preset and theme files, the JSON reader). Each must answer (ok or
// an error sentence) and never crash, hang or read outside its input; build with DVA_SANITIZE=ON.
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "dva/core.hpp"
#include "dva/theme.hpp"
#include "fh/json.hpp"
#include "tests.hpp"

namespace {
std::mt19937_64 rng(20261011);
int runs = 0;

std::string noise(std::size_t n) {
  std::string s(n, '\0');
  for (auto& c : s) {
    c = static_cast<char>(rng() & 0xFF);
  }
  return s;
}

std::string damage(std::string s) {
  if (s.empty()) {
    return s;
  }
  const int edits = 1 + static_cast<int>(rng() % 8);
  for (int i = 0; i < edits; ++i) {
    switch (rng() % 5) {
      case 0:
        s[rng() % s.size()] = static_cast<char>(rng() & 0xFF);
        break;
      case 1:
        s.erase(rng() % s.size(), 1 + rng() % 20);
        break;
      case 2:
        s.insert(rng() % s.size(), noise(1 + rng() % 20));
        break;
      case 3:
        s.resize(rng() % (s.size() + 1));
        break;
      default: {
        const std::size_t a = rng() % s.size();
        const std::size_t b = rng() % s.size();
        std::swap(s[a], s[b]);
      }
    }
    if (s.empty()) {
      break;
    }
  }
  return s;
}

void feed(const std::string& text) {
  ++runs;
  (void)dva::parse_eqf(text);
  (void)dva::parse_preset_file(text);
  (void)dva::parse_theme_file(text);
  (void)fh::json::parse(text);
}
}  // namespace

void fuzz_tests() {
  test::section("fuzz");
  std::vector<std::string> seeds = {
      "",
      "{}",
      "[]",
      "null",
      "{\"dva-eq\": 1}",
      "Winamp EQ library file v1.1\x1a!--",
      std::string(268 * 3 + 31, 'A'),
      "{\"dva-eq\":1,\"name\":\"x\",\"bands\":[1,2,3,4,5,6,7,8,9,10],\"preamp\":0}",
      std::string(5000, '['),
      std::string(5000, '{'),
      "\"\\ud800\"",
      "1e999999",
      "-",
      "{\"a\":" + std::string(300, '1') + "}"};
  for (const auto& s : seeds) {
    feed(s);
  }
  for (int i = 0; i < 20000; ++i) {
    feed(noise(rng() % 600));
  }
  for (int i = 0; i < 20000; ++i) {
    feed(damage(seeds[rng() % seeds.size()]));
  }
  // a valid-looking .eqf with many entries, damaged
  const std::string header = "Winamp EQ library file v1.1\x1a!--";
  std::string entries;
  for (int e = 0; e < 5; ++e) {
    std::string entry(268, '\0');
    entry.replace(0, 6, "preset");
    for (int b = 0; b < 11; ++b) {
      entry[257 + b] = static_cast<char>(rng() % 70);
    }
    entries += entry;
  }
  for (int i = 0; i < 20000; ++i) {
    feed(damage(header + entries));
  }
  test::check(std::to_string(runs) +
                  " inputs go through the .eqf, preset, theme and JSON readers "
                  "without a crash",
              true);
}
