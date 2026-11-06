#include "dva/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dva {

using fh::json::Value;

namespace {

double clamp(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
double js_round(double x) {
  const double f = std::floor(x);
  return x - f >= 0.5 ? f + 1 : f;
}  // Math.round: halves go up
double half(double v) { return js_round(v * 2) / 2; }
double number_or(const Value* v, double fallback) {
  return (v && v->is_number() && std::isfinite(v->as_number())) ? v->as_number() : fallback;
}
bool bool_or(const Value* v, bool fallback) { return v && v->is_bool() ? v->as_bool() : fallback; }

// JavaScript's Number.prototype.toFixed(1): exact decimal value, halves round up (away from zero
// for negatives, which are handled by sign).
std::string to_fixed1(double x) {
  if (std::isnan(x)) {
    return "NaN";
  }
  if (std::isinf(x)) {
    return x > 0 ? "Infinity" : "-Infinity";
  }
  const bool neg = x < 0;
  char buf[512];
  std::snprintf(
      buf, sizeof buf, "%.60f",
      std::fabs(x));  // the exact value of the double, to far more digits than it can have
  const std::string s = buf;
  const std::size_t dot = s.find('.');
  const std::string whole = s.substr(0, dot);
  std::string frac = s.substr(dot + 1);
  const int tenths = frac[0] - '0';
  const bool up = frac[1] >= '5';
  std::string digits = whole + static_cast<char>('0' + tenths);
  if (up) {
    std::size_t i = digits.size();
    while (i > 0) {
      if (digits[i - 1] == '9') {
        digits[i - 1] = '0';
        i--;
      } else {
        digits[i - 1]++;
        break;
      }
    }
    if (i == 0) {
      digits.insert(digits.begin(), '1');
    }
  }
  const std::string out = digits.substr(0, digits.size() - 1) + "." + digits.back();
  return (neg ? "-" : "") + out;
}

std::string whole(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.0f", v);
  return b;
}

// Characters counted the way JavaScript counts them (UTF-16 units); a character that would be cut
// is dropped.
std::string slice_units(const std::string& s, std::size_t max) {
  std::string out;
  std::size_t used = 0;
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    const std::size_t units = len == 4 ? 2 : 1;
    if (used + units > max || i + len > s.size()) {
      break;
    }
    out.append(s, i, len);
    used += units;
    i += len;
  }
  return out;
}

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
std::string trim(const std::string& s) {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && is_space(s[a])) {
    a++;
  }
  while (b > a && is_space(s[b - 1])) {
    b--;
  }
  return s.substr(a, b - a);
}

std::string clean_name(const Value* v) {
  if (!v || !v->is_string()) {
    return "";
  }
  std::string s;
  for (const char ch : v->as_string()) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F || ch == '<' || ch == '>') {
      continue;  // removed, not replaced
    }
    s += ch;
  }
  return slice_units(trim(s), max_name);
}

Bands clean_bands(const Value* raw) {
  Bands b{};
  for (std::size_t i = 0; i < b.size(); i++) {
    const Value* item =
        (raw && raw->is_array() && i < raw->items().size()) ? &raw->items()[i] : nullptr;
    b[i] = clamp(half(number_or(item, 0)), -eq_limit, eq_limit);
  }
  return b;
}
double clean_preamp(const Value* raw) {
  return clamp(half(number_or(raw, 0)), -eq_limit, eq_limit);
}

EqPreset p(const char* name, double preamp, Bands bands) { return {name, preamp, bands}; }

}  // namespace

double db_to_gain(double db) {
  return (!std::isfinite(db) || db <= db_min) ? 0 : std::pow(10.0, clamp(db, db_min, db_max) / 20);
}
std::string format_db(double db) {
  if (db <= db_min) {
    return "muted";
  }
  if (std::fabs(db) < 0.05) {
    return "0.0 dB";
  }
  return to_fixed1(db) + " dB";
}
std::string signed_db(double db) { return std::string(db > 0 ? "+" : "") + to_fixed1(db) + " dB"; }
double snap_speed(double s) {
  double best = 1;
  for (const double x : speeds) {
    if (std::fabs(x - s) < std::fabs(best - s)) {
      best = x;
    }
  }
  return best;
}
double balance_to_pan(double b) { return clamp(std::isfinite(b) ? b : 0, -100, 100) / 100; }
std::string format_balance(long long b) {
  if (b == 0) {
    return "center";
  }
  return b < 0 ? std::to_string(-b) + "% left" : std::to_string(b) + "% right";
}

const std::vector<EqPreset>& presets() {
  static const std::vector<EqPreset> list = {
      p("Flat", 0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
      p("Rock", -2, {5, 4, 3, 1, -1, -1.5, 0.5, 3, 4, 5}),
      p("Pop", -1, {-1.5, 1.5, 4, 5, 3.5, 0, -1.5, -1.5, -1, -1}),
      p("Dance", -2, {6, 4.5, 1.5, 0, 0, -3, -4, -4, 0, 0}),
      p("Club", -1, {0, 0, 4, 3, 3, 3, 2, 0, 0, 0}),
      p("Techno", -2, {5, 4, 0.5, -3, -3, 0, 4.5, 5.5, 5.5, 5}),
      p("Full bass", -4, {7, 7, 7, 4, 1, -2.5, -5, -6, -6.5, -6.5}),
      p("Full treble", -3, {-6.5, -6.5, -6.5, -2.5, 1.5, 7, 9.5, 9.5, 9.5, 10}),
      p("Full bass & treble", -4, {5, 4, 0, -5, -3.5, 1, 5, 7, 8, 8}),
      p("Soft", -2, {3, 1, 0, -1, 0, 3, 5, 6, 7, 8}),
      p("Soft rock", -1, {2.5, 2.5, 1.5, 0, -2, -2.5, -1.5, 0, 1.5, 5.5}),
      p("Classical", 0, {0, 0, 0, 0, 0, 0, -6, -6, -6, -8}),
      p("Live", -2, {-3, 0, 2, 3, 3, 3, 2, 1, 1, 1}),
      p("Large hall", -3, {6, 6, 3, 3, 0, -3, -3, -3, 0, 0}),
      p("Party", -2, {4.5, 4.5, 0, 0, 0, 0, 0, 0, 4.5, 4.5}),
      p("Reggae", -1, {0, 0, -0.5, -4.5, 0, 4, 4, 0, 0, 0}),
      p("Ska", -2, {-1.5, -3, -3, -0.5, 2.5, 3.5, 5.5, 6, 7, 6}),
      p("Headphones", -2, {3, 6.5, 3, -2, -1.5, 1, 3, 6, 8, 9}),
      p("Voice", -1, {-3, -2, 0, 3, 5, 5, 4, 2, 0, -2}),
  };
  return list;
}
const EqPreset* preset_by_name(std::string_view name) {
  for (const EqPreset& p : presets()) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}
std::array<FilterSpec, band_count> filter_specs() {
  std::array<FilterSpec, band_count> out{};
  for (int i = 0; i < band_count; i++) {
    out[static_cast<std::size_t>(i)] = {i == 0                ? FilterSpec::Type::LowShelf
                                        : i == band_count - 1 ? FilterSpec::Type::HighShelf
                                                              : FilterSpec::Type::Peaking,
                                        bands_hz[static_cast<std::size_t>(i)], filter_q};
  }
  return out;
}

double effective_preamp(const Settings& s) { return s.eq_on ? s.preamp : 0; }
Bands effective_bands(const Settings& s) { return s.eq_on ? s.bands : Bands{}; }

std::optional<std::string> match_preset(double preamp, const Bands& bands,
                                        const std::vector<EqPreset>& user) {
  const auto same = [&](const EqPreset& p) { return p.preamp == preamp && p.bands == bands; };
  for (const EqPreset& p : presets()) {
    if (same(p)) {
      return p.name;
    }
  }
  for (const EqPreset& p : user) {
    if (same(p)) {
      return p.name;
    }
  }
  return std::nullopt;
}

Settings sanitize(const Value& raw) {
  Settings s;
  const bool is_obj = raw.is_object();
  const auto get = [&](std::string_view k) { return is_obj ? raw.find(k) : nullptr; };
  if (const Value* list = get("userPresets"); list && list->is_array()) {
    for (const Value& p : list->items()) {
      if (!p.is_object()) {
        continue;
      }
      const std::string name = clean_name(p.find("name"));
      if (name.empty() || preset_by_name(name)) {
        continue;
      }
      if (std::ranges::any_of(s.user_presets, [&](const EqPreset& q) { return q.name == name; })) {
        continue;
      }
      if (s.user_presets.size() >= max_user_presets) {
        break;
      }
      s.user_presets.push_back(
          {name, clean_preamp(p.find("preamp")), clean_bands(p.find("bands"))});
    }
  }
  s.preamp = clean_preamp(get("preamp"));
  s.bands = clean_bands(get("bands"));
  s.volume_db = clamp(js_round(number_or(get("volumeDb"), -2) * 10) / 10, db_min, db_max);
  s.balance = static_cast<int>(clamp(js_round(number_or(get("balance"), 0)), -100, 100));
  s.mono = bool_or(get("mono"), false);
  s.speed = snap_speed(number_or(get("speed"), 1));
  s.eq_on = bool_or(get("eqOn"), true);
  s.preset = match_preset(s.preamp, s.bands, s.user_presets);
  s.limiter = bool_or(get("limiter"), true);
  const Value* tm = get("timeMode");
  s.time_remaining = tm && tm->is_string() && tm->as_string() == "remaining";
  return s;
}

Value to_json(const Settings& s) {
  Value o = Value::object();
  o.set("volumeDb", Value::number(s.volume_db));
  o.set("balance", Value::number(s.balance));
  o.set("mono", Value::boolean(s.mono));
  o.set("speed", Value::number(s.speed));
  o.set("eqOn", Value::boolean(s.eq_on));
  o.set("preamp", Value::number(s.preamp));
  Value bands = Value::array();
  for (const double b : s.bands) {
    bands.push(Value::number(b));
  }
  o.set("bands", std::move(bands));
  o.set("preset", s.preset ? Value::string(*s.preset) : Value::null());
  o.set("limiter", Value::boolean(s.limiter));
  o.set("timeMode", Value::string(s.time_remaining ? "remaining" : "elapsed"));
  Value user = Value::array();
  for (const EqPreset& p : s.user_presets) {
    Value u = Value::object();
    u.set("name", Value::string(p.name));
    u.set("preamp", Value::number(p.preamp));
    Value b = Value::array();
    for (const double x : p.bands) {
      b.push(Value::number(x));
    }
    u.set("bands", std::move(b));
    user.push(std::move(u));
  }
  o.set("userPresets", std::move(user));
  return o;
}

SaveResult save_preset(const Settings& s, const std::string& raw_name) {
  SaveResult r;
  const Value name_value = Value::string(raw_name);
  const std::string name = clean_name(&name_value);
  if (name.empty()) {
    r.error = "Give the preset a name (up to " + std::to_string(max_name) + " characters).";
    return r;
  }
  if (preset_by_name(name)) {
    r.error = "\"" + name + "\" is a built-in preset. Pick another name.";
    return r;
  }
  Settings next = s;
  next.user_presets.erase(
      std::ranges::remove_if(next.user_presets, [&](const EqPreset& p) { return p.name == name; })
          .begin(),
      next.user_presets.end());
  if (next.user_presets.size() >= max_user_presets) {
    r.error = "You can keep " + std::to_string(max_user_presets) + " presets. Delete one first.";
    return r;
  }
  next.user_presets.push_back({name, s.preamp, s.bands});
  r.ok = true;
  r.settings = sanitize(to_json(next));
  return r;
}

Settings delete_preset(const Settings& s, const std::string& name) {
  Settings next = s;
  next.user_presets.erase(
      std::ranges::remove_if(next.user_presets, [&](const EqPreset& p) { return p.name == name; })
          .begin(),
      next.user_presets.end());
  return sanitize(to_json(next));
}

std::string export_preset(const EqPreset& p) {
  Value o = Value::object();
  o.set("dva-eq", Value::number(1));
  o.set("name", Value::string(p.name));
  o.set("preamp", Value::number(p.preamp));
  Value b = Value::array();
  for (const double x : p.bands) {
    b.push(Value::number(x));
  }
  o.set("bands", std::move(b));
  return fh::json::stringify(o, 2);
}

PresetFile parse_preset_file(std::string_view text) {
  PresetFile r;
  if (text.size() > max_file) {
    r.error = "That file is too big to be an equalizer preset.";
    return r;
  }
  const fh::json::ParseResult parsed = fh::json::parse(text);
  if (!parsed.ok) {
    r.error = "That file is not valid JSON.";
    return r;
  }
  const Value& raw = parsed.value;
  const Value* marker = raw.is_object() ? raw.find("dva-eq") : nullptr;
  if (!marker || !marker->is_number() || marker->as_number() != 1) {
    r.error = "That file is not a DejaVuAmp preset (it has no \"dva-eq\": 1).";
    return r;
  }
  const std::string name = clean_name(raw.find("name"));
  if (name.empty()) {
    r.error = "The preset needs a name of up to " + std::to_string(max_name) + " characters.";
    return r;
  }
  const Value* bands = raw.find("bands");
  bool good = bands && bands->is_array() && bands->items().size() == band_count;
  if (good) {
    for (const Value& v : bands->items()) {
      if (!v.is_number() || !std::isfinite(v.as_number())) {
        good = false;
      }
    }
  }
  if (!good) {
    r.error =
        "The preset needs exactly " + std::to_string(band_count) + " band values, all numbers.";
    return r;
  }
  r.ok = true;
  r.preset = {name, clean_preamp(raw.find("preamp")), clean_bands(bands)};
  return r;
}

EqfFile parse_eqf(std::string_view bytes) {
  constexpr std::string_view magic = "Winamp EQ library file v1.1\x1A!--";
  constexpr std::size_t name_len = 257;
  constexpr std::size_t entry_len = name_len + 11;
  constexpr std::size_t max_eqf_bytes = 64 * 1024;
  EqfFile r;
  if (bytes.size() > max_eqf_bytes) {
    r.error = "That file is too big to be an equalizer library.";
    return r;
  }
  if (bytes.size() < magic.size() || !bytes.starts_with(magic)) {
    r.error = "That is not an equalizer library file (.eqf).";
    return r;
  }
  for (std::size_t at = magic.size(); at + entry_len <= bytes.size(); at += entry_len) {
    std::string raw(bytes.substr(at, name_len));
    raw.resize(raw.find('\0') == std::string::npos ? raw.size() : raw.find('\0'));
    bool utf8 = true;  // old files hold Latin-1 text: turn it into UTF-8 unless it already is
    for (std::size_t i = 0; i < raw.size() && utf8;) {
      const auto c0 = static_cast<unsigned char>(raw[i]);
      const std::size_t n = c0 < 0x80         ? 1
                            : (c0 >> 5) == 6  ? 2
                            : (c0 >> 4) == 14 ? 3
                            : (c0 >> 3) == 30 ? 4
                                              : 0;
      if (n == 0 || i + n > raw.size()) {
        utf8 = false;
        break;
      }
      for (std::size_t k = 1; k < n; k++) {
        if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80) {
          utf8 = false;
        }
      }
      i += n;
    }
    if (!utf8) {
      std::string fixed;
      for (const char ch : raw) {
        const auto c0 = static_cast<unsigned char>(ch);
        if (c0 < 0x80) {
          fixed += ch;
        } else {
          fixed += static_cast<char>(0xC0 | (c0 >> 6));
          fixed += static_cast<char>(0x80 | (c0 & 0x3F));
        }
      }
      raw = fixed;
    }
    const Value name_value = Value::string(raw);
    const std::string name = clean_name(&name_value);
    if (name.empty()) {
      continue;
    }
    if (std::ranges::any_of(r.presets, [&](const EqPreset& q) { return q.name == name; })) {
      continue;
    }
    const auto db = [&](std::size_t i) {
      const unsigned v =
          std::min<unsigned>(static_cast<unsigned char>(bytes[at + name_len + i]), 63);
      return clamp(half(12.0 - static_cast<double>(v) * 24.0 / 63.0), -eq_limit, eq_limit);
    };
    EqPreset p;
    p.name = name;
    for (std::size_t i = 0; i < band_count; i++) {
      p.bands[i] = db(i);
    }
    p.preamp = db(band_count);
    r.presets.push_back(p);
  }
  if (r.presets.empty()) {
    r.error = "There are no presets in that equalizer library.";
    return r;
  }
  r.ok = true;
  return r;
}

std::string format_time(double seconds, bool remaining) {
  if (!std::isfinite(seconds) || seconds < 0) {
    return remaining ? "-:--" : "--:--";
  }
  const double s = std::floor(seconds);
  const double h = std::floor(s / 3600);
  const double m = std::fmod(std::floor(s / 60), 60);
  const double sec = std::fmod(s, 60);
  const auto two = [](double n) {
    char b[32];
    std::snprintf(b, sizeof b, "%02.0f", n);
    return std::string(b);
  };
  return std::string(remaining ? "-" : "") +
         (h > 0 ? whole(h) + ":" + two(m) + ":" + two(sec) : two(m) + ":" + two(sec));
}

std::string ticker_text(std::string_view title, std::string_view artist) {
  std::string joined;
  if (!artist.empty()) {
    joined = std::string(artist);
  }
  if (!title.empty()) {
    if (!joined.empty()) {
      joined += " - ";
    }
    joined += std::string(title);
  }
  for (char& c : joined) {
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) {
      c = ' ';
    }
  }
  joined = slice_units(joined, 200);
  return joined.empty() ? "DejaVuAmp" : joined;
}

}  // namespace dva
