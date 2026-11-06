#include "dva/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "fh/json.hpp"

namespace dva {

namespace {
using fh::json::Value;

constexpr std::size_t max_file = 4096, max_name = 30;
const char* const key_names[Theme::KeyCount] = {"bg",    "bg2",    "fg",    "dim",  "pink", "cyan",
                                                "lilac", "border", "bezel", "warn", "bad"};

Rgb hex(const char* s) {
  unsigned v = 0;
  std::sscanf(s + 1, "%x", &v);
  return {static_cast<int>((v >> 16) & 255), static_cast<int>((v >> 8) & 255),
          static_cast<int>(v & 255)};
}

Theme make(const char* name, bool glow, bool scan,
           std::array<const char*, Theme::KeyCount> colors) {
  Theme t;
  t.name = name;
  t.glow = glow;
  t.scanlines = scan;
  for (int i = 0; i < Theme::KeyCount; i++) {
    t.c[static_cast<std::size_t>(i)] = hex(colors[static_cast<std::size_t>(i)]);
  }
  return t;
}

bool is_hex_color(const std::string& s) {
  if (s.size() != 7 || s[0] != '#') {
    return false;
  }
  for (std::size_t i = 1; i < 7; i++) {
    if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  return true;
}
}  // namespace

double luminance(Rgb c) {
  const auto lin = [](int v) {
    const double x = v / 255.0;
    return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

double contrast(Rgb a, Rgb b) {
  double x = luminance(a);
  double y = luminance(b);
  if (x < y) {
    std::swap(x, y);
  }
  return (x + 0.05) / (y + 0.05);
}

bool Theme::light() const { return luminance(c[Bg]) > 0.4; }

Rgb Theme::bg_deep() const {
  const double by = luminance(c[Bg]) > 0.5 ? 0.06 : 0.45;
  const auto d = [&](int v) {
    return static_cast<int>(std::clamp(std::lround(v * (1 - by)), 0L, 255L));
  };
  return {d(c[Bg].r), d(c[Bg].g), d(c[Bg].b)};
}

const std::vector<Theme>& builtin_themes() {
  //                                bg         bg2        fg         dim        pink       cyan
  //                                lilac      border     bezel      warn       bad
  static const std::vector<Theme> list = {
      make("Dreamspace", true, true,
           {"#0b0720", "#150c33", "#d6f6ff", "#9a8fd6", "#ff9de6", "#8af3ff", "#b9a2ff", "#3d2f82",
            "#1b1438", "#ffd98a", "#ff8fa3"}),
      make("ANSI 16 colors", false, true,
           {"#000000", "#0000aa", "#ffffff", "#aaaaaa", "#ff55ff", "#55ffff", "#5555ff", "#00aaaa",
            "#555555", "#ffff55", "#ff5555"}),
      make("Phosphor green", true, true,
           {"#010a01", "#021602", "#33ff33", "#1fb21f", "#66ff66", "#99ff99", "#22cc22", "#135a13",
            "#0a1f0a", "#ccff33", "#ff6633"}),
      make("Amber monochrome", true, true,
           {"#0a0500", "#140a00", "#ffb000", "#b37b00", "#ffcc33", "#ffb000", "#ff8c00", "#5a3b00",
            "#1f1200", "#ffe066", "#ff6a3d"}),
      make("Paper (light)", false, false,
           {"#f6f1e7", "#ebe3d3", "#1d1a14", "#5b5444", "#b3365f", "#0d6e86", "#5a46b3", "#c9bfa8",
            "#d8cfba", "#8a5a00", "#b3261e"}),
      make("High contrast", false, false,
           {"#000000", "#000000", "#ffffff", "#d0d0d0", "#ffff00", "#00ffff", "#ff80ff", "#ffffff",
            "#303030", "#ffd000", "#ff6060"}),
  };
  return list;
}

const Theme& default_theme() { return builtin_themes().front(); }

ThemeFile parse_theme_file(std::string_view text) {
  ThemeFile r;
  if (text.size() > max_file) {
    r.error = "That file is too big to be a theme.";
    return r;
  }
  const fh::json::ParseResult parsed = fh::json::parse(text);
  if (!parsed.ok) {
    r.error = "That file is not valid JSON.";
    return r;
  }
  const Value& raw = parsed.value;
  const Value* marker = raw.is_object() ? raw.find("dva-theme") : nullptr;
  if (!marker || !marker->is_number() || marker->as_number() != 1) {
    r.error = "That file is not a DejaVuAmp theme (it has no \"dva-theme\": 1).";
    return r;
  }
  std::string name;
  if (const Value* n = raw.find("name"); n && n->is_string()) {
    for (const char ch : n->as_string()) {
      const auto c = static_cast<unsigned char>(ch);
      if (c >= 0x20 && c != 0x7F && ch != '<' && ch != '>') {
        name += ch;
      }
    }
    std::size_t a = 0;
    std::size_t b = name.size();
    while (a < b && name[a] == ' ') {
      a++;
    }
    while (b > a && name[b - 1] == ' ') {
      b--;
    }
    name = name.substr(a, b - a);
  }
  if (name.empty() || name.size() > max_name) {
    r.error = "A theme needs a name of 1 to " + std::to_string(max_name) + " characters.";
    return r;
  }
  if (const Value* logo = raw.find("logo");
      logo &&
      !(logo->is_string() && (logo->as_string() == "text" || logo->as_string() == "aperture"))) {
    r.error = "The logo must be one of: text, aperture.";
    return r;
  }
  const Value* colors = raw.find("colors");
  if (!colors || !colors->is_object()) {
    r.error = "The theme has no colors.";
    return r;
  }
  Theme t;
  t.name = name;
  for (int i = 0; i < Theme::KeyCount; i++) {
    const Value* v = colors->find(key_names[i]);
    if (!v || !v->is_string() || !is_hex_color(v->as_string())) {
      r.error = std::string("\"") + key_names[i] + "\" must be a color like #1a2b3c.";
      return r;
    }
    t.c[static_cast<std::size_t>(i)] = hex(v->as_string().c_str());
  }
  if (const Value* g = raw.find("glow"); g && g->is_bool()) {
    t.glow = g->as_bool();
  }
  if (const Value* s = raw.find("scan"); s && s->is_bool()) {
    t.scanlines = s->as_bool();
  }
  const auto at = [&](Theme::Key k) { return t.c[static_cast<std::size_t>(k)]; };
  if (contrast(at(Theme::Fg), at(Theme::Bg)) < 4.5 ||
      contrast(at(Theme::Fg), at(Theme::Bg2)) < 4.5) {
    r.error =
        "The text color is too close to the background to read. Pick colors that contrast more.";
    return r;
  }
  if (contrast(at(Theme::Dim), at(Theme::Bg)) < 3 || contrast(at(Theme::Dim), at(Theme::Bg2)) < 3) {
    r.error = "The quiet text color is too close to the background to read.";
    return r;
  }
  if (contrast(at(Theme::Warn), at(Theme::Bg)) < 3 || contrast(at(Theme::Bad), at(Theme::Bg)) < 3) {
    r.error = "The warning and error colors are too close to the background to read.";
    return r;
  }
  r.ok = true;
  r.theme = t;
  return r;
}

std::string export_theme(const Theme& t) {
  Value o = Value::object();
  o.set("dva-theme", Value::number(1));
  o.set("name", Value::string(t.name));
  o.set("logo", Value::string("text"));
  o.set("glow", Value::boolean(t.glow));
  o.set("scan", Value::boolean(t.scanlines));
  Value c = Value::object();
  for (int i = 0; i < Theme::KeyCount; i++) {
    char buf[8];
    const Rgb& k = t.c[static_cast<std::size_t>(i)];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", k.r, k.g, k.b);
    c.set(key_names[i], Value::string(buf));
  }
  o.set("colors", std::move(c));
  return fh::json::stringify(o, 2);
}

}  // namespace dva
