// Tests for the colour themes (include/dva/theme.hpp): the six built-ins have their documented
// colours, and theme files are checked for names, colour format and readable contrast.
#include <cmath>
#include <iostream>
#include <string>

#include "check.hpp"
#include "dva/theme.hpp"
#include "tests.hpp"

using namespace dva;
using test::check;

static std::string file(const std::string& name, const std::string& bg = "#0b0720",
                        const std::string& fg = "#d6f6ff", const std::string& dim = "#9a8fd6",
                        const std::string& extra = "") {
  return "{\"dva-theme\":1,\"name\":\"" + name + "\"," + extra + "\"colors\":{\"bg\":\"" + bg +
         "\",\"bg2\":\"#150c33\",\"fg\":\"" + fg + "\",\"dim\":\"" + dim +
         "\",\"pink\":\"#ff9de6\",\"cyan\":\"#8af3ff\",\"lilac\":\"#b9a2ff\",\"border\":\"#"
         "3d2f82\",\"bezel\":\"#1b1438\",\"warn\":\"#ffd98a\",\"bad\":\"#ff8fa3\"}}";
}

void theme_tests() {
  test::section("theme");
  const auto& all = builtin_themes();
  check("six built-in themes, Dreamspace is the default",
        all.size() == 6 && all[0].name == "Dreamspace" && default_theme().name == "Dreamspace");
  check("Dreamspace has its documented colours",
        default_theme().c[Theme::Bg].r == 0x0b && default_theme().c[Theme::Pink].g == 0x9d &&
            default_theme().c[Theme::Cyan].b == 0xff && default_theme().c[Theme::Lilac].r == 0xb9);
  check("exactly one theme is light, and no deep background is lighter than its background", [&] {
    int light = 0;
    for (const Theme& t : all) {
      if (t.light()) {
        light++;
      }
      if (luminance(t.bg_deep()) > luminance(t.c[Theme::Bg]) + 1e-9) {
        return false;
      }
    }
    return light == 1 && all[4].light();
  }());
  check("built-in themes pass the readability rules that apply to files", [&] {
    for (const Theme& t : all) {
      const ThemeFile f = parse_theme_file(export_theme(t));
      if (!f.ok) {
        std::cout << "   " << t.name << ": " << f.error << "\n";
        return false;
      }
    }
    return true;
  }());
  check("contrast is 21 for black on white and 1 for equal colours",
        std::fabs(contrast({0, 0, 0}, {255, 255, 255}) - 21) < 1e-9 &&
            std::fabs(contrast({9, 9, 9}, {9, 9, 9}) - 1) < 1e-9);
  {
    ThemeFile f = parse_theme_file(export_theme(all[2]));
    check("exporting then parsing gives the same theme",
          f.ok && f.theme.name == all[2].name && f.theme.c[Theme::Fg].g == all[2].c[Theme::Fg].g &&
              f.theme.glow == all[2].glow && f.theme.scanlines == all[2].scanlines,
          f.error);
  }
  check("a valid file loads", parse_theme_file(file("Mine")).ok);
  check("the name is required, at most 30 characters, and < and > are removed",
        !parse_theme_file(file("")).ok && !parse_theme_file(file(std::string(31, 'x'))).ok &&
            parse_theme_file(file("<b>Mine</b>")).theme.name == "bMine/b");
  check("colours must be written #rrggbb", !parse_theme_file(file("A", "blue")).ok &&
                                               !parse_theme_file(file("A", "#12345")).ok &&
                                               !parse_theme_file(file("A", "#12345g")).ok);
  check("text too close to the background colour is refused",
        !parse_theme_file(file("A", "#0b0720", "#0c0821")).ok &&
            parse_theme_file(file("A", "#0b0720", "#0c0821")).error.find("text color") !=
                std::string::npos);
  check("dim text too close to the background colour is refused",
        !parse_theme_file(file("A", "#0b0720", "#d6f6ff", "#0e0a24")).ok);
  check("a wrong marker, invalid JSON, an oversized file and an unknown logo are refused",
        !parse_theme_file("{\"name\":\"A\"}").ok && !parse_theme_file("{").ok &&
            !parse_theme_file(std::string(5000, ' ')).ok &&
            !parse_theme_file(file("A", "#0b0720", "#d6f6ff", "#9a8fd6", "\"logo\":\"cat\",")).ok);
  check("the glow and scanline switches are read",
        !parse_theme_file(
             file("A", "#0b0720", "#d6f6ff", "#9a8fd6", "\"glow\":false,\"scan\":false,"))
                .theme.glow &&
            !parse_theme_file(
                 file("A", "#0b0720", "#d6f6ff", "#9a8fd6", "\"glow\":false,\"scan\":false,"))
                 .theme.scanlines);
}
