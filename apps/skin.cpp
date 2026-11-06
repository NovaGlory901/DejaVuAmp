#include "skin.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "fh/pathutf8.hpp"
#include "imgui_internal.h"

namespace skin {

namespace fs = std::filesystem;

namespace {
ImU32 col(dva::Rgb c, int a = 255) { return IM_COL32(c.r, c.g, c.b, a); }
ImVec4 vec(dva::Rgb c, float a = 1) {
  return {static_cast<float>(c.r) / 255.0F, static_cast<float>(c.g) / 255.0F,
          static_cast<float>(c.b) / 255.0F, a};
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }
}  // namespace

ImU32 mix(ImU32 a, ImU32 b, float t) {
  const auto ch = [&](int shift) {
    return static_cast<int>(std::lround(
        lerp(static_cast<float>((a >> shift) & 255), static_cast<float>((b >> shift) & 255), t)));
  };
  return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT),
                  ch(IM_COL32_A_SHIFT));
}
ImU32 with_alpha(ImU32 c, float alpha) {
  return (c & ~IM_COL32_A_MASK) |
         (static_cast<ImU32>(std::clamp(alpha, 0.0F, 1.0F) * 255.0F) << IM_COL32_A_SHIFT);
}

Colors make_colors(const dva::Theme& t) {
  using K = dva::Theme;
  Colors c;
  const dva::Rgb deep = t.bg_deep();
  c.bg = col(t.c[K::Bg]);
  c.bg2 = col(t.c[K::Bg2]);
  c.fg = col(t.c[K::Fg]);
  c.dim = col(t.c[K::Dim]);
  c.pink = col(t.c[K::Pink]);
  c.cyan = col(t.c[K::Cyan]);
  c.lilac = col(t.c[K::Lilac]);
  c.border = col(t.c[K::Border]);
  c.bezel = col(t.c[K::Bezel]);
  c.warn = col(t.c[K::Warn]);
  c.bad = col(t.c[K::Bad]);
  c.deep = col(deep);
  c.on_accent = c.bg;
  c.light = t.light();
  c.field = c.light ? IM_COL32(255, 255, 255, 140) : IM_COL32(0, 0, 0, 115);
  c.v_fg = vec(t.c[K::Fg]);
  c.v_dim = vec(t.c[K::Dim]);
  c.v_pink = vec(t.c[K::Pink]);
  c.v_cyan = vec(t.c[K::Cyan]);
  c.v_lilac = vec(t.c[K::Lilac]);
  c.v_border = vec(t.c[K::Border]);
  c.v_deep = vec(deep);
  c.v_bg2 = vec(t.c[K::Bg2]);
  c.v_on_accent = vec(t.c[K::Bg]);
  c.v_field = c.light ? ImVec4(1, 1, 1, 0.55F) : ImVec4(0, 0, 0, 0.45F);
  c.glow = t.glow;
  c.scan = t.scanlines;
  return c;
}

void apply_style(const Colors& c) {
  ImGuiStyle& s = ImGui::GetStyle();
  ImVec4* k = s.Colors;
  k[ImGuiCol_Text] = c.v_fg;
  k[ImGuiCol_TextDisabled] = c.v_dim;
  k[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
  k[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  k[ImGuiCol_PopupBg] = ImVec4(c.v_bg2.x, c.v_bg2.y, c.v_bg2.z, 0.98F);
  k[ImGuiCol_Border] = c.v_border;
  k[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  k[ImGuiCol_FrameBg] = c.v_field;
  k[ImGuiCol_FrameBgHovered] = ImVec4(c.v_lilac.x, c.v_lilac.y, c.v_lilac.z, 0.22F);
  k[ImGuiCol_FrameBgActive] = ImVec4(c.v_lilac.x, c.v_lilac.y, c.v_lilac.z, 0.32F);
  k[ImGuiCol_SliderGrab] = c.v_pink;
  k[ImGuiCol_SliderGrabActive] = c.v_cyan;
  k[ImGuiCol_Header] = ImVec4(c.v_pink.x, c.v_pink.y, c.v_pink.z, 0.28F);
  k[ImGuiCol_HeaderHovered] = ImVec4(c.v_lilac.x, c.v_lilac.y, c.v_lilac.z, 0.30F);
  k[ImGuiCol_HeaderActive] = ImVec4(c.v_pink.x, c.v_pink.y, c.v_pink.z, 0.40F);
  k[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
  k[ImGuiCol_ButtonHovered] = ImVec4(c.v_pink.x, c.v_pink.y, c.v_pink.z, 0.5F);
  k[ImGuiCol_ButtonActive] = c.v_cyan;
  k[ImGuiCol_ScrollbarBg] = c.v_deep;
  k[ImGuiCol_ScrollbarGrab] = c.v_border;
  k[ImGuiCol_ScrollbarGrabHovered] = c.v_lilac;
  k[ImGuiCol_ScrollbarGrabActive] = c.v_pink;
  k[ImGuiCol_Separator] = c.v_border;
  k[ImGuiCol_TextSelectedBg] = ImVec4(c.v_pink.x, c.v_pink.y, c.v_pink.z, 0.35F);
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  s.FrameBorderSize = 1;
  s.PopupBorderSize = 1;
  s.WindowRounding = 0;
  s.FrameRounding = 2;
  s.GrabRounding = 2;
  s.PopupRounding = 2;
  s.ScrollbarRounding = 0;
  s.ScrollbarSize = 12;
  s.GrabMinSize = 12;
  s.FramePadding = ImVec2(8, 2);
  s.ItemSpacing = ImVec2(8, 5);
  s.WindowPadding = ImVec2(0, 0);
}

bool load_fonts(const fs::path& root, float size) {
  const ImGuiIO& io = ImGui::GetIO();
  std::error_code ec;
  const fs::path ttf = root / "assets" / "fonts" / "VT323-Regular.ttf";
  if (!fs::exists(ttf, ec)) {
    io.Fonts->AddFontDefault();
    return false;
  }
  const ImFont* main = io.Fonts->AddFontFromFileTTF(fh::path_to_utf8(ttf).c_str(), size);
  if (!main) {
    io.Fonts->AddFontDefault();
    return false;
  }
  // Japanese, Chinese and Korean titles: VT323 has none, so borrow a system font for them if there
  // is one
  static const char* cjk[] = {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                              "/usr/share/fonts/noto-cjk/NotoSansCJK-Light.ttc",
                              "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc",
                              "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                              "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                              "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                              "/usr/share/fonts/TTF/NotoSansCJK-Regular.ttc",
                              "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
                              "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                              "C:\\Windows\\Fonts\\YuGothM.ttc",
                              "C:\\Windows\\Fonts\\msgothic.ttc",
                              "C:\\Windows\\Fonts\\malgun.ttf",
                              "C:\\Windows\\Fonts\\msyh.ttc",
                              "/System/Library/Fonts/Hiragino Sans GB.ttc",
                              "/System/Library/Fonts/AppleSDGothicNeo.ttc"};
  for (const char* path : cjk) {
    if (!fs::exists(path, ec)) {
      continue;
    }
    ImFontConfig cfg;
    cfg.MergeMode = true;
    if (io.Fonts->AddFontFromFileTTF(path, size, &cfg)) {
      break;
    }
  }
  return true;
}

void backdrop(const Colors& c, ImVec2 size) {
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddRectFilledMultiColor(ImVec2(0, 0), size, c.bg, c.bg, c.bg2, c.bg2);
  if (!c.glow) {
    return;
  }
  // soft glows: pink at the top left, cyan at the right, lilac at the bottom (rings of low alpha,
  // which blend into a smooth falloff)
  const auto glow = [&](ImVec2 centre, float radius, ImU32 colour, float strength) {
    for (int i = 0; i < 14; i++) {
      const float t = static_cast<float>(i) / 14.0F;
      dl->AddCircleFilled(centre, radius * (1.0F - t),
                          with_alpha(colour, strength / 14.0F * (c.light ? 0.5F : 1.0F)), 48);
    }
  };
  const float r = std::max(size.x, size.y);
  glow(ImVec2(size.x * 0.15F, size.y * 0.10F), r * 0.45F, c.pink, 0.16F);
  glow(ImVec2(size.x * 0.90F, size.y * 0.25F), r * 0.40F, c.cyan, 0.12F);
  glow(ImVec2(size.x * 0.50F, size.y * 1.10F), r * 0.55F, c.lilac, 0.20F);
}

void scanlines(const Colors& c, ImVec2 size) {
  if (!c.scan) {
    return;
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 line = with_alpha(c.cyan, 0.045F);
  for (float y = 0; y < size.y; y += 4) {
    dl->AddRectFilled(ImVec2(0, y), ImVec2(size.x, y + 1), line);
  }
}

void text(ImVec2 pos, ImU32 colour, const char* s, float size, float spacing) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const float fs = size > 0 ? size : ImGui::GetFontSize();
  if (spacing <= 0) {
    dl->AddText(font, fs, pos, colour, s);
    return;
  }
  for (const char* p = s; *p;) {
    unsigned int cp = 0;
    const int n = ImTextCharFromUtf8(&cp, p, nullptr);
    if (n <= 0) {
      break;
    }
    char buf[8] = {0};
    for (int i = 0; i < n && i < 7; i++) {
      buf[i] = p[i];
    }
    dl->AddText(font, fs, pos, colour, buf);
    pos.x += font->CalcTextSizeA(fs, 10000, 0, buf).x + spacing;
    p += n;
  }
}

void glow_text(ImVec2 pos, ImU32 colour, const char* s, float size, const Colors& c) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  if (c.glow) {
    const ImU32 halo = with_alpha(colour, 0.20F);
    const ImU32 halo2 = with_alpha(c.pink, 0.08F);
    for (int dx = -2; dx <= 2; dx++) {
      for (int dy = -2; dy <= 2; dy++) {
        if ((dx | dy) == 0) {
          continue;
        }
        dl->AddText(font, size,
                    ImVec2(pos.x + static_cast<float>(dx), pos.y + static_cast<float>(dy)),
                    (std::abs(dx) + std::abs(dy) > 2) ? halo2 : halo, s);
      }
    }
  }
  dl->AddText(font, size, pos, colour, s);
}

namespace {
ImVec2 panel_origin;
ImVec2 panel_size;
}  // namespace

ImVec2 panel_content_start() { return {10, 34}; }

void panel_begin(const char* id, ImVec2 size, const char* title, const Colors& c) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  panel_origin = p;
  panel_size = size;
  dl->AddRectFilled(ImVec2(p.x + 4, p.y + 4), ImVec2(p.x + size.x + 4, p.y + size.y + 4),
                    with_alpha(c.deep, 0.6F));
  const ImU32 top = mix(c.bg2, c.lilac, 0.14F);
  const ImU32 bottom = mix(c.bg2, c.lilac, 0.04F);
  dl->AddRectFilledMultiColor(p, ImVec2(p.x + size.x, p.y + size.y), top, top, bottom, bottom);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + 2), c.lilac);
  dl->AddRectFilled(p, ImVec2(p.x + 2, p.y + size.y), c.lilac);
  dl->AddRectFilled(ImVec2(p.x, p.y + size.y - 2), ImVec2(p.x + size.x, p.y + size.y), c.border);
  dl->AddRectFilled(ImVec2(p.x + size.x - 2, p.y), ImVec2(p.x + size.x, p.y + size.y), c.border);
  const float th = 26;
  dl->AddRectFilledMultiColor(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + size.x - 2, p.y + 2 + th),
                              c.pink, c.cyan, c.cyan, c.pink);
  text(ImVec2(p.x + 12, p.y + 2 + (th - ImGui::GetFontSize()) * 0.5F), c.on_accent, title, 0, 3);
  ImGui::BeginChild(id, size, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
}

void panel_end() { ImGui::EndChild(); }

void inset_box(ImVec2 a, ImVec2 b, const Colors& c) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, c.deep);
  dl->AddRectFilled(a, ImVec2(b.x, a.y + 2), c.border);
  dl->AddRectFilled(a, ImVec2(a.x + 2, b.y), c.border);
  dl->AddRectFilled(ImVec2(a.x, b.y - 2), b, mix(c.border, c.lilac, 0.5F));
  dl->AddRectFilled(ImVec2(b.x - 2, a.y), b, mix(c.border, c.lilac, 0.5F));
}

bool button(const char* label, ImVec2 size, const Colors& c, bool enabled) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 text_size = ImGui::CalcTextSize(label, nullptr, true);
  if (size.x <= 0) {
    size.x = text_size.x + 24;
  }
  if (size.y <= 0) {
    size.y = ImGui::GetFontSize() + 4;
  }
  ImGui::PushID(label);
  ImGui::BeginDisabled(!enabled);
  const bool pressed = ImGui::InvisibleButton("##b", size);
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  ImGui::EndDisabled();
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float alpha = enabled ? 1.0F : 0.4F;
  if (hot && enabled) {
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + size.x, p.y + size.y), c.pink, c.cyan, c.cyan,
                                c.pink);
    dl->AddText(ImVec2(p.x + (size.x - text_size.x) * 0.5F, p.y + (size.y - text_size.y) * 0.5F),
                c.on_accent, label);
  } else {
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), with_alpha(c.lilac, alpha), 2.0F, 0, 1.0F);
    dl->AddText(ImVec2(p.x + (size.x - text_size.x) * 0.5F, p.y + (size.y - text_size.y) * 0.5F),
                with_alpha(c.fg, alpha), label);
  }
  return pressed && enabled;
}

bool icon_button(const char* id, Icon icon, ImVec2 size, const Colors& c) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##i", size);
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hot) {
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + size.x, p.y + size.y), c.pink, c.cyan, c.cyan,
                                c.pink);
  } else {
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), c.lilac, 2.0F, 0, 1.0F);
  }
  const ImU32 ink = hot ? c.on_accent : c.fg;
  const float h = size.y * 0.5F;
  const float w = size.x * 0.5F;
  const float cx = p.x + w;
  const float cy = p.y + h;
  const float u = size.y * 0.26F;
  switch (icon) {
    case Icon::Play:
      dl->AddTriangleFilled(ImVec2(cx - u * 0.7F, cy - u), ImVec2(cx - u * 0.7F, cy + u),
                            ImVec2(cx + u * 0.9F, cy), ink);
      break;
    case Icon::Pause:
      dl->AddRectFilled(ImVec2(cx - u * 0.8F, cy - u), ImVec2(cx - u * 0.2F, cy + u), ink);
      dl->AddRectFilled(ImVec2(cx + u * 0.2F, cy - u), ImVec2(cx + u * 0.8F, cy + u), ink);
      break;
    case Icon::Prev:
      dl->AddRectFilled(ImVec2(cx - u * 0.95F, cy - u), ImVec2(cx - u * 0.6F, cy + u), ink);
      dl->AddTriangleFilled(ImVec2(cx + u * 0.9F, cy - u), ImVec2(cx + u * 0.9F, cy + u),
                            ImVec2(cx - u * 0.45F, cy), ink);
      break;
    case Icon::Next:
      dl->AddRectFilled(ImVec2(cx + u * 0.6F, cy - u), ImVec2(cx + u * 0.95F, cy + u), ink);
      dl->AddTriangleFilled(ImVec2(cx - u * 0.9F, cy - u), ImVec2(cx - u * 0.9F, cy + u),
                            ImVec2(cx + u * 0.45F, cy), ink);
      break;
  }
  return pressed;
}

bool toggle(const char* label, bool on, const Colors& c) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 ts = ImGui::CalcTextSize(label);
  const ImVec2 size(ts.x + 16, ImGui::GetFontSize() + 2);
  ImGui::PushID(label);
  const bool pressed = ImGui::InvisibleButton("##t", size);
  const bool hot = ImGui::IsItemHovered();
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (on) {
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                      hot ? mix(c.cyan, c.pink, 0.35F) : c.cyan, 2);
    dl->AddText(ImVec2(p.x + 8, p.y + (size.y - ts.y) * 0.5F), c.on_accent, label);
  } else {
    if (hot) {
      dl->AddRectFilledMultiColor(p, ImVec2(p.x + size.x, p.y + size.y), c.pink, c.cyan, c.cyan,
                                  c.pink);
    } else {
      dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), c.lilac, 2.0F, 0, 1.0F);
    }
    dl->AddText(ImVec2(p.x + 8, p.y + (size.y - ts.y) * 0.5F), hot ? c.on_accent : c.fg, label);
  }
  return pressed;
}

}  // namespace skin
