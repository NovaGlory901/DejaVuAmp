// DejaVuAmp's logic: volume in decibels, the 10-band equalizer and its presets, balance, settings
// validation and the player window's formatters. Port of js/dejavuamp-core.js (the reference;
// tests/parity/core.golden). Texts are meant to be ASCII: the JavaScript
// counts UTF-16 units, so answers differ only for odd non-ASCII input.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fh/json.hpp"

namespace dva {

constexpr double db_min = -60, db_max = 0;
constexpr double speeds[] = {0.5, 0.75, 1, 1.25, 1.5, 2};
constexpr int band_count = 10;
constexpr std::array<double, band_count> bands_hz = {60,   170,  310,   600,   1000,
                                                     3000, 6000, 12000, 14000, 16000};
constexpr std::array<const char*, band_count> band_labels = {"60", "170", "310", "600", "1K",
                                                             "3K", "6K",  "12K", "14K", "16K"};
constexpr double eq_limit = 12;  // decibels either way, on every band and on the preamp
constexpr std::size_t max_user_presets = 20, max_name = 24, max_file = 2048;
constexpr double filter_q = 1.4;

using Bands = std::array<double, band_count>;

struct EqPreset {
  std::string name;
  double preamp = 0;
  Bands bands{};
  bool operator==(const EqPreset&) const = default;
};

double db_to_gain(double db);  // anything odd (not a number, at or below -60) means silence
std::string format_db(double db);  // "muted", "0.0 dB", "-12.5 dB"
std::string signed_db(double db);  // "+3.0 dB"
double snap_speed(double s);  // the nearest of 0.5 0.75 1 1.25 1.5 2
double balance_to_pan(double balance);  // -100..100 to -1..1
std::string format_balance(long long balance);  // "center", "20% left", "35% right"

const std::vector<EqPreset>& presets();  // the 19 built-in curves
const EqPreset* preset_by_name(std::string_view name);  // built-ins only, exact name

struct FilterSpec {
  enum class Type { LowShelf, Peaking, HighShelf } type;
  double frequency, q;
};
std::array<FilterSpec, band_count>
filter_specs();  // the lowest and highest band are shelves, the rest bells

struct Settings {
  double volume_db = -2;
  int balance = 0;  // -100 (left) .. 100 (right)
  bool mono = false;
  double speed = 1;
  bool eq_on = true;
  double preamp = 0;
  Bands bands{};
  std::optional<std::string>
      preset;  // worked out, never chosen: the name of the preset the sliders match
  bool limiter = true;
  bool time_remaining = false;  // time display: "remaining" instead of "elapsed"
  std::vector<EqPreset> user_presets;
};

Settings sanitize(
    const fh::json::Value& raw);  // anything in (a stored file, hand-edited), valid settings out
fh::json::Value to_json(const Settings& s);  // the same shape the JavaScript produces

double effective_preamp(const Settings& s);  // 0 when the equalizer is off
Bands effective_bands(const Settings& s);  // all zero when the equalizer is off
std::optional<std::string> match_preset(double preamp, const Bands& bands,
                                        const std::vector<EqPreset>& user = {});

struct SaveResult {
  bool ok = false;
  std::string error;
  Settings settings;
};
SaveResult save_preset(
    const Settings& s,
    const std::string& raw_name);  // the current curve as a named preset (the same name replaces)
Settings delete_preset(const Settings& s, const std::string& name);

std::string export_preset(const EqPreset& p);  // file text, {"dva-eq": 1, ...}
struct PresetFile {
  bool ok = false;
  std::string error;
  EqPreset preset;
};
PresetFile parse_preset_file(std::string_view text);  // changes nothing

// A classic player's equalizer library file (.eqf: "Winamp EQ library file v1.1", then 268-byte
// entries of a name and 11 bytes 0..63 where 0 is +12 dB and 63 is -12 dB: ten bands, then the
// preamp). One file holds many presets. Entries that do not fit (empty name, a repeat of an earlier
// name) are skipped; `error` is set only when the file as a whole is not one (nothing usable in
// it).
struct EqfFile {
  bool ok = false;
  std::string error;
  std::vector<EqPreset> presets;
};
EqfFile parse_eqf(std::string_view bytes);  // changes nothing

std::string format_time(double seconds,
                        bool remaining);  // "03:25", "1:02:03", "-03:25"; "--:--" for nothing known
std::string ticker_text(std::string_view title, std::string_view artist);

}  // namespace dva
