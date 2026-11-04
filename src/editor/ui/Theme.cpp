#include "editor/ui/Theme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace ts {
namespace tombstone {
namespace editor {
namespace theme {

namespace {

bool g_light = false;
enum class Look { Forge, Dusk, Parchment };
Look g_look = Look::Forge;

constexpr float kPi = 3.14159265358979f;

ImVec4 rgba(int r, int g, int b, int a = 255) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

ImVec4 with_alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// Shared geometry for both variants: sharp, hand-cut corners and inked frames.
void apply_metrics(ImGuiStyle& style) {
  style.WindowRounding = metrics::kRadiusSmall;
  style.ChildRounding = metrics::kRadiusCard;
  style.FrameRounding = metrics::kRadiusSmall;
  style.PopupRounding = metrics::kRadiusSmall;
  style.ScrollbarRounding = 1.0f;
  style.GrabRounding = 1.0f;
  style.TabRounding = metrics::kRadiusSmall;
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.TabBorderSize = 0.0f;
  style.WindowPadding = ImVec2(12.0f, 10.0f);
  style.FramePadding = ImVec2(9.0f, 5.0f);
  style.CellPadding = ImVec2(6.0f, 4.0f);
  style.ItemSpacing = ImVec2(9.0f, 7.0f);
  style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
  style.IndentSpacing = 16.0f;
  style.ScrollbarSize = 11.0f;
  style.GrabMinSize = 9.0f;
  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
  style.SeparatorTextBorderSize = 1.0f;
  style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
  style.SeparatorTextPadding = ImVec2(10.0f, 3.0f);
  style.WindowMenuButtonPosition = ImGuiDir_None;
}

struct Palette {
  ImVec4 deep, bg, bg2, bg3, frame, frame_h, frame_a, border, border_soft;
  ImVec4 text, text_dis;
  ImVec4 button, button_h, button_a;
};

void apply_colors(const Palette& p) {
  ImGuiStyle& style = ImGui::GetStyle();
  ImVec4* c = style.Colors;
  const ImVec4 amber = Accent();
  const ImVec4 amber_h = AccentHover();
  const ImVec4 copper = Copper();

  c[ImGuiCol_Text] = p.text;
  c[ImGuiCol_TextDisabled] = p.text_dis;
  c[ImGuiCol_WindowBg] = p.bg;
  c[ImGuiCol_ChildBg] = p.bg2;
  c[ImGuiCol_PopupBg] = with_alpha(p.bg2, 0.98f);
  c[ImGuiCol_Border] = p.border;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = p.frame;
  c[ImGuiCol_FrameBgHovered] = p.frame_h;
  c[ImGuiCol_FrameBgActive] = p.frame_a;
  c[ImGuiCol_TitleBg] = p.deep;
  c[ImGuiCol_TitleBgActive] = p.bg3;
  c[ImGuiCol_TitleBgCollapsed] = p.deep;
  c[ImGuiCol_MenuBarBg] = p.bg3;
  c[ImGuiCol_ScrollbarBg] = with_alpha(p.deep, 0.6f);
  c[ImGuiCol_ScrollbarGrab] = p.button;
  c[ImGuiCol_ScrollbarGrabHovered] = p.button_h;
  c[ImGuiCol_ScrollbarGrabActive] = copper;
  c[ImGuiCol_CheckMark] = amber;
  c[ImGuiCol_SliderGrab] = amber;
  c[ImGuiCol_SliderGrabActive] = amber_h;
  c[ImGuiCol_Button] = p.button;
  c[ImGuiCol_ButtonHovered] = p.button_h;
  c[ImGuiCol_ButtonActive] = p.button_a;
  c[ImGuiCol_Header] = with_alpha(amber, 0.26f);
  c[ImGuiCol_HeaderHovered] = with_alpha(copper, 0.34f);
  c[ImGuiCol_HeaderActive] = with_alpha(amber, 0.45f);
  c[ImGuiCol_Separator] = p.border_soft;
  c[ImGuiCol_SeparatorHovered] = copper;
  c[ImGuiCol_SeparatorActive] = amber;
  c[ImGuiCol_ResizeGrip] = with_alpha(copper, 0.25f);
  c[ImGuiCol_ResizeGripHovered] = with_alpha(copper, 0.7f);
  c[ImGuiCol_ResizeGripActive] = amber;
  c[ImGuiCol_Tab] = p.bg2;
  c[ImGuiCol_TabHovered] = p.button_h;
#if IMGUI_VERSION_NUM >= 19090
  c[ImGuiCol_TabSelected] = p.bg3;
  c[ImGuiCol_TabSelectedOverline] = amber;
  c[ImGuiCol_TabDimmed] = p.deep;
  c[ImGuiCol_TabDimmedSelected] = p.bg2;
  c[ImGuiCol_TabDimmedSelectedOverline] = with_alpha(copper, 0.6f);
#else
  c[ImGuiCol_TabActive] = p.bg3;
  c[ImGuiCol_TabUnfocused] = p.deep;
  c[ImGuiCol_TabUnfocusedActive] = p.bg2;
#endif
  c[ImGuiCol_DockingPreview] = with_alpha(amber, 0.40f);
  c[ImGuiCol_DockingEmptyBg] = p.deep;
  c[ImGuiCol_PlotLines] = amber;
  c[ImGuiCol_PlotLinesHovered] = copper;
  c[ImGuiCol_PlotHistogram] = amber;
  c[ImGuiCol_PlotHistogramHovered] = copper;
  c[ImGuiCol_TableHeaderBg] = p.bg3;
  c[ImGuiCol_TableBorderStrong] = p.border;
  c[ImGuiCol_TableBorderLight] = p.border_soft;
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = with_alpha(p.text, 0.03f);
  c[ImGuiCol_TextSelectedBg] = with_alpha(amber, 0.32f);
  c[ImGuiCol_DragDropTarget] = amber_h;
#if IMGUI_VERSION_NUM >= 19140
  c[ImGuiCol_NavCursor] = amber;
#else
  c[ImGuiCol_NavHighlight] = amber;
#endif
  c[ImGuiCol_NavWindowingHighlight] = with_alpha(amber, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = rgba(0, 0, 0, 120);
  c[ImGuiCol_ModalWindowDimBg] = rgba(8, 6, 4, 170);
}

// Forge: iron greys, molten orange #E8792B as the accent.
void apply_forge() {
  ImGui::StyleColorsDark();
  apply_metrics(ImGui::GetStyle());
  Palette p{};
  p.deep = rgba(16, 17, 19);
  p.bg = rgba(26, 28, 31);
  p.bg2 = rgba(34, 36, 40);
  p.bg3 = rgba(44, 47, 52);
  p.frame = rgba(38, 40, 45);
  p.frame_h = rgba(52, 50, 48);
  p.frame_a = rgba(72, 56, 42);
  p.border = rgba(72, 74, 80);
  p.border_soft = rgba(54, 56, 62);
  p.text = rgba(232, 230, 224);
  p.text_dis = rgba(154, 152, 146);
  p.button = rgba(46, 48, 53);
  p.button_h = rgba(84, 62, 44);
  p.button_a = rgba(168, 84, 32);
  apply_colors(p);
}

// Dusk: deep charcoal with warm stone, amber/gold primary, copper secondary.
void apply_dark() {
  ImGui::StyleColorsDark();
  apply_metrics(ImGui::GetStyle());
  Palette p{};
  p.deep = rgba(14, 13, 12);
  p.bg = rgba(21, 19, 18);
  p.bg2 = rgba(28, 25, 23);
  p.bg3 = rgba(39, 34, 30);
  p.frame = rgba(33, 29, 26);
  p.frame_h = rgba(47, 41, 35);
  p.frame_a = rgba(60, 51, 42);
  p.border = rgba(74, 62, 50);
  p.border_soft = rgba(56, 48, 40);
  p.text = rgba(234, 224, 204);      // bone
  p.text_dis = rgba(148, 136, 116);  // weathered sand
  p.button = rgba(52, 45, 39);       // stone
  p.button_h = rgba(88, 61, 42);     // copper-warmed stone
  p.button_a = rgba(140, 76, 44);    // rust
  apply_colors(p);
}

// Parchment: warm paper, ink-brown text, darker gold + copper for contrast.
void apply_light() {
  ImGui::StyleColorsLight();
  apply_metrics(ImGui::GetStyle());
  Palette p{};
  p.deep = rgba(214, 202, 180);
  p.bg = rgba(236, 228, 212);
  p.bg2 = rgba(243, 237, 225);
  p.bg3 = rgba(224, 212, 190);
  p.frame = rgba(250, 246, 236);
  p.frame_h = rgba(242, 230, 206);
  p.frame_a = rgba(232, 212, 176);
  p.border = rgba(170, 148, 116);
  p.border_soft = rgba(198, 182, 156);
  p.text = rgba(40, 32, 26);
  p.text_dis = rgba(118, 102, 84);
  p.button = rgba(226, 214, 192);
  p.button_h = rgba(224, 192, 152);
  p.button_a = rgba(204, 150, 104);
  apply_colors(p);
}

}  // namespace

void Apply(const std::string& theme_name) {
  if (theme_name == "light" || theme_name == "parchment") {
    g_look = Look::Parchment;
    g_light = true;
    apply_light();
  } else if (theme_name == "dusk" || theme_name == "dark") {
    g_look = Look::Dusk;
    g_light = false;
    apply_dark();
  } else {
    g_look = Look::Forge;
    g_light = false;
    apply_forge();
  }
}

bool IsLight() { return g_light; }

ImVec4 Accent() {
  if (g_look == Look::Parchment) return rgba(170, 106, 16);
  if (g_look == Look::Dusk) return rgba(222, 160, 52);
  return rgba(232, 121, 43);  // #E8792B
}
ImVec4 AccentHover() {
  if (g_look == Look::Parchment) return rgba(194, 128, 28);
  if (g_look == Look::Dusk) return rgba(240, 186, 88);
  return rgba(244, 154, 84);
}
ImVec4 AccentActive() {
  if (g_look == Look::Parchment) return rgba(144, 88, 10);
  if (g_look == Look::Dusk) return rgba(190, 128, 34);
  return rgba(196, 90, 24);
}
ImVec4 AccentMuted() { return with_alpha(Accent(), 0.55f); }
ImVec4 Copper() {
  if (g_look == Look::Parchment) return rgba(150, 70, 34);
  if (g_look == Look::Dusk) return rgba(184, 98, 56);
  return rgba(176, 84, 36);
}
ImVec4 CopperMuted() { return with_alpha(Copper(), 0.55f); }
ImVec4 Sand() {
  if (g_look == Look::Parchment) return rgba(150, 126, 90);
  if (g_look == Look::Dusk) return rgba(201, 180, 140);
  return rgba(180, 176, 168);
}
ImVec4 Charcoal() {
  if (g_look == Look::Parchment) return rgba(58, 48, 40);
  if (g_look == Look::Dusk) return rgba(14, 13, 12);
  return rgba(16, 17, 19);
}
ImVec4 Danger() {
  if (g_look == Look::Parchment) return rgba(160, 36, 32);
  return rgba(206, 74, 64);
}
ImVec4 Success() {
  if (g_look == Look::Parchment) return rgba(70, 112, 52);
  if (g_look == Look::Dusk) return rgba(138, 176, 104);
  return rgba(120, 168, 112);
}
ImVec4 Warning() {
  if (g_look == Look::Parchment) return rgba(176, 100, 20);
  if (g_look == Look::Dusk) return rgba(236, 176, 86);
  return rgba(232, 160, 72);
}
ImVec4 PanelBg() {
  if (g_look == Look::Parchment) return rgba(246, 241, 230);
  if (g_look == Look::Dusk) return rgba(27, 24, 22);
  return rgba(30, 32, 36);
}
ImVec4 HeaderBg() {
  if (g_look == Look::Parchment) return rgba(226, 214, 192);
  if (g_look == Look::Dusk) return rgba(33, 29, 26);
  return rgba(38, 40, 45);
}
ImVec4 Text() {
  if (g_look == Look::Parchment) return rgba(40, 32, 26);
  if (g_look == Look::Dusk) return rgba(234, 224, 204);
  return rgba(232, 230, 224);
}
ImVec4 TextMuted() {
  if (g_look == Look::Parchment) return rgba(118, 102, 84);
  if (g_look == Look::Dusk) return rgba(148, 136, 116);
  return rgba(154, 152, 146);
}
ImVec4 TextOnAccent() {
  if (g_look == Look::Parchment) return rgba(255, 249, 238);
  if (g_look == Look::Dusk) return rgba(26, 20, 14);
  return rgba(28, 18, 12);
}
ImVec4 Border() {
  if (g_look == Look::Parchment) return rgba(170, 148, 116);
  if (g_look == Look::Dusk) return rgba(74, 62, 50);
  return rgba(72, 74, 80);
}
ImVec4 CanvasBg() {
  if (g_look == Look::Parchment) return rgba(222, 211, 190);
  if (g_look == Look::Dusk) return rgba(17, 16, 15);
  return rgba(20, 21, 24);
}
ImVec4 GridLine() {
  if (g_look == Look::Parchment) return rgba(150, 130, 100, 90);
  if (g_look == Look::Dusk) return rgba(120, 100, 78, 70);
  return rgba(96, 98, 104, 80);
}

ImU32 U32(const ImVec4& c, float alpha_mul) {
  return ImGui::ColorConvertFloat4ToU32(with_alpha(c, c.w * alpha_mul));
}

bool BeginRoot(const char* id, ImGuiWindowFlags extra_flags) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoDocking | extra_flags;
  const bool open = ImGui::Begin(id, nullptr, flags);
  ImGui::PopStyleVar();

  // Subtle vignette: darker toward the bottom like lamp light falling off.
  ImDrawList* bg = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetWindowPos();
  const ImVec2 sz = ImGui::GetWindowSize();
  const ImU32 clear = U32(Charcoal(), 0.0f);
  const ImU32 shade = U32(Charcoal(), g_light ? 0.10f : 0.55f);
  bg->AddRectFilledMultiColor(ImVec2(p0.x, p0.y + sz.y * 0.55f),
                              ImVec2(p0.x + sz.x, p0.y + sz.y), clear, clear,
                              shade, shade);
  return open;
}

void EndRoot() { ImGui::End(); }

bool BeginCard(const char* id, float width, float height,
               ImGuiChildFlags child_flags, ImGuiWindowFlags window_flags) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, PanelBg());
  ImGui::PushStyleColor(ImGuiCol_Border, Border());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metrics::kRadiusCard);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 20.0f));
  const bool open =
      ImGui::BeginChild(id, ImVec2(width, height), child_flags, window_flags);
  // Amber top edge + thin copper underline: the card's "brand"
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos();
  const float w = ImGui::GetWindowSize().x;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 3.0f), U32(Accent()),
                    metrics::kRadiusCard, ImDrawFlags_RoundCornersTop);
  dl->AddRectFilled(ImVec2(p.x, p.y + 3.0f), ImVec2(p.x + w, p.y + 4.0f),
                    U32(Copper(), 0.6f));
  return open;
}

void EndCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

void SectionHeader(const char* label) { SectionHeader(label, nullptr); }

void SectionHeader(const char* label, const char* caption) {
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = std::max(ImGui::GetContentRegionAvail().x, 8.0f);
  const float lh = ImGui::GetTextLineHeight();

  // Diamond marker.
  const float d = std::max(3.0f, lh * 0.26f);
  const ImVec2 c(p.x + d + 1.0f, p.y + lh * 0.5f);
  dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y),
                    ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), U32(Accent()));

  ImGui::SetCursorScreenPos(ImVec2(p.x + d * 2.0f + 8.0f, p.y));
  ImGui::PushStyleColor(ImGuiCol_Text, Accent());
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  if (caption && caption[0] != '\0') {
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, TextMuted());
    ImGui::TextUnformatted(caption);
    ImGui::PopStyleColor();
  }

  // Fading rule: amber -> copper -> nothing.
  const float ry = p.y + lh + 4.0f;
  const float mid = p.x + w * 0.35f;
  dl->AddRectFilledMultiColor(ImVec2(p.x, ry), ImVec2(mid, ry + 1.5f),
                              U32(Accent(), 0.9f), U32(Copper(), 0.7f),
                              U32(Copper(), 0.7f), U32(Accent(), 0.9f));
  dl->AddRectFilledMultiColor(ImVec2(mid, ry), ImVec2(p.x + w, ry + 1.5f),
                              U32(Copper(), 0.7f), U32(Copper(), 0.0f),
                              U32(Copper(), 0.0f), U32(Copper(), 0.7f));
  ImGui::SetCursorScreenPos(ImVec2(p.x, ry + 8.0f));
  ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void Divider() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = std::max(ImGui::GetContentRegionAvail().x, 8.0f);
  const float y = p.y + 6.0f;
  const float cx = p.x + w * 0.5f;
  const float d = 3.5f;
  const ImU32 line = U32(Border());
  dl->AddLine(ImVec2(p.x, y), ImVec2(cx - d - 4.0f, y), line);
  dl->AddLine(ImVec2(cx + d + 4.0f, y), ImVec2(p.x + w, y), line);
  dl->AddQuadFilled(ImVec2(cx, y - d), ImVec2(cx + d, y), ImVec2(cx, y + d),
                    ImVec2(cx - d, y), U32(Copper()));
  ImGui::Dummy(ImVec2(w, 12.0f));
}

void TombstoneMark(ImDrawList* dl, const ImVec2& top_center, float height,
                   ImU32 stone, ImU32 engrave) {
  const float stone_h = height * 0.86f;
  const float w = stone_h * 0.70f;
  const float r = w * 0.5f;
  const float left = top_center.x - r;
  const float right = top_center.x + r;
  const float base = top_center.y + stone_h;
  const float arch_cy = top_center.y + r;

  dl->PathLineTo(ImVec2(left, base));
  dl->PathLineTo(ImVec2(left, arch_cy));
  dl->PathArcTo(ImVec2(top_center.x, arch_cy), r, kPi, 2.0f * kPi, 16);
  dl->PathLineTo(ImVec2(right, base));
  dl->PathFillConvex(stone);

  // Plinth.
  const float slab = w * 0.14f;
  dl->AddRectFilled(ImVec2(left - slab, base),
                    ImVec2(right + slab, top_center.y + height), stone, 1.0f);

  // Engraved cross.
  const float t = std::max(1.5f, w * 0.12f);
  const float cx = top_center.x;
  const float v0 = top_center.y + stone_h * 0.24f;
  const float v1 = top_center.y + stone_h * 0.80f;
  const float hy = top_center.y + stone_h * 0.42f;
  dl->AddRectFilled(ImVec2(cx - t * 0.5f, v0), ImVec2(cx + t * 0.5f, v1),
                    engrave);
  dl->AddRectFilled(ImVec2(cx - w * 0.26f, hy - t * 0.5f),
                    ImVec2(cx + w * 0.26f, hy + t * 0.5f), engrave);
}

ImVec2 MeasureText(float px, const char* text) {
  return ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
}

void DrawSizedText(ImDrawList* dl, const ImVec2& pos, float px, ImU32 col,
                   const char* text) {
  dl->AddText(ImGui::GetFont(), px, pos, col, text);
}

void CenteredText(const char* text, float px, const ImVec4& col,
                  float avail_width) {
  const ImVec2 sz = MeasureText(px, text);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float x = p.x + std::max(0.0f, (avail_width - sz.x) * 0.5f);
  DrawSizedText(ImGui::GetWindowDrawList(), ImVec2(x, p.y), px, U32(col),
                text);
  ImGui::Dummy(ImVec2(avail_width, sz.y));
}

void TitleStrip(const char* title, const char* subtitle) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = std::max(ImGui::GetContentRegionAvail().x, 8.0f);
  const float h = metrics::kTitleStripHeight;

  dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), U32(HeaderBg()),
                              U32(PanelBg()), U32(PanelBg()), U32(HeaderBg()));
  dl->AddRectFilled(ImVec2(p.x, p.y + h - 2.0f), ImVec2(p.x + w, p.y + h),
                    U32(Accent()));
  dl->AddRectFilled(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h + 1.0f),
                    U32(Copper(), 0.6f));

  TombstoneMark(dl, ImVec2(p.x + 22.0f, p.y + 8.0f), h - 16.0f, U32(Sand()),
                U32(Charcoal(), 0.85f));

  const float title_px = 20.0f;
  const ImVec2 tsz = MeasureText(title_px, title);
  const float tx = p.x + 44.0f;
  const float ty = p.y + (h - 2.0f - tsz.y) * 0.5f;
  DrawSizedText(dl, ImVec2(tx, ty), title_px, U32(Text()), title);
  if (subtitle && subtitle[0] != '\0') {
    const float sub_px = ImGui::GetFontSize();
    const ImVec2 ssz = MeasureText(sub_px, subtitle);
    const float sx = tx + tsz.x + 24.0f;
    const float sy = ty + (tsz.y - ssz.y) * 0.5f + 1.0f;
    const float lx = tx + tsz.x + 12.0f;
    dl->AddLine(ImVec2(lx, ty + 3.0f), ImVec2(lx, ty + tsz.y - 3.0f),
                U32(Copper()), 1.5f);
    DrawSizedText(dl, ImVec2(sx, sy), sub_px, U32(TextMuted()), subtitle);
  }
  ImGui::Dummy(ImVec2(w, h + 6.0f));
}

void StatusSuccess(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, Success());
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void StatusError(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, Danger());
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void StatusWarn(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, Warning());
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

void StatusInfo(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, TextMuted());
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

namespace {

bool filled_button(const char* label, const ImVec2& size, const ImVec4& base,
                   const ImVec4& hover, const ImVec4& active,
                   const ImVec4& text, const ImVec4& border) {
  ImGui::PushStyleColor(ImGuiCol_Button, base);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
  ImGui::PushStyleColor(ImGuiCol_Text, text);
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  return pressed;
}

}  // namespace

bool PrimaryButton(const char* label, const ImVec2& size) {
  return filled_button(label, size, Accent(), AccentHover(), AccentActive(),
                       TextOnAccent(), AccentActive());
}

bool SecondaryButton(const char* label, const ImVec2& size) {
  return ImGui::Button(label, size);
}

bool CopperButton(const char* label, const ImVec2& size) {
  const ImVec4 c = Copper();
  const ImVec4 h(std::min(1.0f, c.x * 1.12f), std::min(1.0f, c.y * 1.14f),
                 std::min(1.0f, c.z * 1.16f), 1.0f);
  const ImVec4 a(c.x * 0.82f, c.y * 0.80f, c.z * 0.80f, 1.0f);
  return filled_button(label, size, c, h, a, rgba(250, 238, 222), a);
}

bool DangerButton(const char* label, const ImVec2& size) {
  return filled_button(label, size, rgba(132, 36, 32), rgba(166, 48, 42),
                       rgba(190, 60, 50), rgba(250, 236, 226),
                       rgba(96, 26, 22));
}

bool ToggleButton(const char* label, bool* value, const ImVec2& size) {
  bool pressed = false;
  if (*value) {
    pressed = filled_button(label, size, Accent(), AccentHover(),
                            AccentActive(), TextOnAccent(), AccentActive());
  } else {
    pressed = ImGui::Button(label, size);
  }
  if (pressed) {
    *value = !*value;
  }
  return pressed;
}

void BrandProgress(float fraction, const ImVec2& size) {
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 q(p.x + size.x, p.y + size.y);
  dl->AddRectFilled(p, q, U32(Charcoal(), g_light ? 0.15f : 0.9f), 1.0f);
  const float fx = p.x + size.x * fraction;
  if (fraction > 0.0f) {
    dl->AddRectFilledMultiColor(p, ImVec2(fx, q.y), U32(Copper()),
                                U32(Accent()), U32(Accent()), U32(Copper()));
    dl->AddLine(ImVec2(fx, p.y - 2.0f), ImVec2(fx, q.y + 2.0f),
                U32(AccentHover()), 2.0f);
  }
  for (int i = 1; i < 10; ++i) {
    const float x = p.x + size.x * (static_cast<float>(i) / 10.0f);
    dl->AddLine(ImVec2(x, q.y - size.y * 0.45f), ImVec2(x, q.y),
                U32(Charcoal(), 0.5f));
  }
  dl->AddRect(p, q, U32(Border()), 1.0f);
  ImGui::Dummy(size);
}

void DimensionBadge(const char* dim) {
  const bool is_2d =
      (std::strcmp(dim, "2D") == 0 || std::strcmp(dim, "2d") == 0);
  const ImVec4 col = is_2d ? Accent() : Copper();
  ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(col, 0.18f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(col, 0.28f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(col, 0.28f));
  ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(col, 0.7f));
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);
  ImGui::SmallButton(dim);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(5);
}

void KeyHint(const char* keys, const char* description) {
  ImGui::PushStyleColor(ImGuiCol_Text, Accent());
  ImGui::Text("%-14s", keys);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, TextMuted());
  ImGui::TextUnformatted(description);
  ImGui::PopStyleColor();
}

void BrandBlock(const char* title, const char* subtitle, float avail_width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const float mark_h = 46.0f;
  const float cx = cursor.x + avail_width * 0.5f;
  TombstoneMark(dl, ImVec2(cx, cursor.y), mark_h, U32(Sand()),
                U32(Charcoal(), 0.85f));
  ImGui::Dummy(ImVec2(avail_width, mark_h + 10.0f));

  CenteredText(title, 22.0f, Text(), avail_width);

  // Short amber rule under the title.
  const ImVec2 rp = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(ImVec2(cx - 28.0f, rp.y + 3.0f), ImVec2(cx + 28.0f, rp.y + 5.0f),
                    U32(Accent()));
  ImGui::Dummy(ImVec2(avail_width, 10.0f));

  if (subtitle && subtitle[0] != '\0') {
    CenteredText(subtitle, ImGui::GetFontSize(), TextMuted(), avail_width);
  }
}

void BrandHero(const char* title, const char* tagline, float avail_width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 cursor = ImGui::GetCursorScreenPos();
  const float mark_h = 64.0f;
  const float cx = cursor.x + avail_width * 0.5f;
  TombstoneMark(dl, ImVec2(cx, cursor.y), mark_h, U32(Sand()),
                U32(Charcoal(), 0.85f));
  ImGui::Dummy(ImVec2(avail_width, mark_h + 14.0f));

  CenteredText(title, 34.0f, Accent(), avail_width);

  const ImVec2 rp = ImGui::GetCursorScreenPos();
  const float half = std::min(avail_width * 0.38f, 150.0f);
  dl->AddRectFilledMultiColor(ImVec2(cx - half, rp.y + 4.0f),
                              ImVec2(cx, rp.y + 5.5f), U32(Copper(), 0.0f),
                              U32(Copper()), U32(Copper()),
                              U32(Copper(), 0.0f));
  dl->AddRectFilledMultiColor(ImVec2(cx, rp.y + 4.0f),
                              ImVec2(cx + half, rp.y + 5.5f), U32(Copper()),
                              U32(Copper(), 0.0f), U32(Copper(), 0.0f),
                              U32(Copper()));
  dl->AddQuadFilled(ImVec2(cx, rp.y + 0.5f), ImVec2(cx + 4.0f, rp.y + 4.75f),
                    ImVec2(cx, rp.y + 9.0f), ImVec2(cx - 4.0f, rp.y + 4.75f),
                    U32(Accent()));
  ImGui::Dummy(ImVec2(avail_width, 16.0f));

  if (tagline && tagline[0] != '\0') {
    CenteredText(tagline, ImGui::GetFontSize(), Sand(), avail_width);
  }
}

}  // namespace theme
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
