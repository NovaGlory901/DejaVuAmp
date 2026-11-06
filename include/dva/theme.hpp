// Colour themes: eleven colours, a name and two switches. Six are built in; more are
// *.dva-theme.json files in themes/, checked for names, colour format and readable contrast.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace dva {

struct Rgb {
  int r = 0, g = 0, b = 0;
};

struct Theme {
  // bg bg2 fg dim pink cyan lilac border bezel warn bad (the order used in theme files)
  enum Key { Bg, Bg2, Fg, Dim, Pink, Cyan, Lilac, Border, Bezel, Warn, Bad, KeyCount };
  std::string name;
  bool glow = true, scanlines = true;
  std::array<Rgb, KeyCount> c{};
  [[nodiscard]] Rgb bg_deep() const;  // darker bg for the displays (less dark for light themes)
  [[nodiscard]] bool light() const;  // a light theme (paper): fields are pale and glows are off
};

const std::vector<Theme>& builtin_themes();
const Theme& default_theme();

double luminance(Rgb c);
double contrast(Rgb a, Rgb b);  // WCAG contrast ratio, 1..21

struct ThemeFile {
  bool ok = false;
  std::string error;
  Theme theme;
};
ThemeFile parse_theme_file(std::string_view text);  // changes nothing
std::string export_theme(const Theme& t);  // file text, {"dva-theme": 1, ...}

}  // namespace dva
