// The look of the DejaVuAmp window, matching the web version's rack (js/dejavuamp.css): the VT323
// font, beveled panels, LCD time, bar/scope display, and the theme's colours everywhere (glow and
// scanlines when the theme has them), all drawn with ImGui's draw lists.
#pragma once

#include <filesystem>
#include <string>

#include "dva/theme.hpp"
#include "imgui.h"

namespace skin {

struct Colors {
  ImU32 bg{}, bg2{}, fg{}, dim{}, pink{}, cyan{}, lilac{}, border{}, bezel{}, warn{}, bad{}, deep{},
      field{}, on_accent{};
  ImVec4 v_fg, v_dim, v_pink, v_cyan, v_lilac, v_border, v_deep, v_field, v_bg2, v_on_accent;
  bool glow = true, scan = true, light = false;
};

Colors make_colors(const dva::Theme& t);
void apply_style(const Colors& c);

// Loads VT323 (assets/fonts next to the program or the source) and merges a system CJK font for
// Japanese, Chinese and Korean titles if one is found. Returns false (and leaves ImGui's default
// font) if the VT323 file is missing.
bool load_fonts(const std::filesystem::path& root, float size);

void backdrop(const Colors& c, ImVec2 size);
void scanlines(const Colors& c, ImVec2 size);

// A beveled panel with a title bar. Content goes between panel_begin and panel_end, positioned from
// the panel's top left.
void panel_begin(const char* id, ImVec2 size, const char* title, const Colors& c);
void panel_end();
ImVec2 panel_content_start();  // where content starts, relative to the panel's top left

void inset_box(ImVec2 a, ImVec2 b, const Colors& c);
bool button(const char* label, ImVec2 size, const Colors& c, bool enabled = true);
enum class Icon { Prev, Play, Pause, Next };
bool icon_button(const char* id, Icon icon, ImVec2 size, const Colors& c);
bool toggle(const char* label, bool on, const Colors& c);
void text(ImVec2 pos, ImU32 col, const char* s, float size = 0, float spacing = 0);
void glow_text(ImVec2 pos, ImU32 col, const char* s, float size, const Colors& c);

ImU32 mix(ImU32 a, ImU32 b, float t);
ImU32 with_alpha(ImU32 c, float alpha);

}  // namespace skin
