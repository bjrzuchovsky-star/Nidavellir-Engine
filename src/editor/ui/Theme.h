#pragma once

#include <imgui.h>

#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace theme {

// Editor looks. Colours live in Theme.cpp.
//   "forge" (default) — iron greys, molten orange #E8792B
//   "dusk" or "dark"  — the 0.2.x Dusk palette (still selectable)
//   "light"           — Parchment
// Call Apply once at startup and whenever Settings.theme changes.
void Apply(const std::string& theme_name);
bool IsLight();

// Shared metrics (sharp, hand-cut corners; inked 1px frames).
namespace metrics {
inline constexpr float kRadiusSmall = 2.0f;
inline constexpr float kRadiusCard = 3.0f;
inline constexpr float kTitleStripHeight = 44.0f;
inline constexpr float kToolbarHeight = 38.0f;
inline constexpr float kStatusBarHeight = 26.0f;
}  // namespace metrics

// Palette accessors (readable contrast on charcoal / warm paper).
ImVec4 Accent();        // amber / gold
ImVec4 AccentHover();
ImVec4 AccentActive();
ImVec4 AccentMuted();   // amber at reduced alpha (rules, outlines)
ImVec4 Copper();        // rust / copper secondary
ImVec4 CopperMuted();
ImVec4 Sand();          // warm stone / sand
ImVec4 Charcoal();      // deepest base (canvas / empty dock)
ImVec4 Danger();
ImVec4 Success();
ImVec4 Warning();
ImVec4 PanelBg();
ImVec4 HeaderBg();
ImVec4 Text();
ImVec4 TextMuted();
ImVec4 TextOnAccent();  // text drawn on top of amber fills
ImVec4 Border();
ImVec4 CanvasBg();      // Viewport2D background
ImVec4 GridLine();

// Packs a palette colour with an optional alpha multiplier.
ImU32 U32(const ImVec4& c, float alpha_mul = 1.0f);

// Full-viewport root window (no decoration). Returns true while open.
bool BeginRoot(const char* id, ImGuiWindowFlags extra_flags = 0);
void EndRoot();

// Centered card panel (child) with an amber top edge + stone border.
// Call EndCard after.
bool BeginCard(const char* id, float width, float height,
               ImGuiChildFlags child_flags = ImGuiChildFlags_Borders,
               ImGuiWindowFlags window_flags = 0);
void EndCard();

// Custom-drawn section header: amber diamond, label, fading copper rule.
void SectionHeader(const char* label);
void SectionHeader(const char* label, const char* caption);

// Ornamental divider: thin rule with a centered diamond.
void Divider();

// Branded title strip across the current window (tombstone mark + title).
void TitleStrip(const char* title, const char* subtitle);

// Arched headstone glyph. top_center = top of the arch; height includes the
// plinth.
void TombstoneMark(ImDrawList* dl, const ImVec2& top_center, float height,
                   ImU32 stone, ImU32 engrave);

// Text at an explicit pixel size (title / hero text without extra fonts).
ImVec2 MeasureText(float px, const char* text);
void DrawSizedText(ImDrawList* dl, const ImVec2& pos, float px, ImU32 col,
                   const char* text);
// Centered line of text at a pixel size; advances the layout cursor.
void CenteredText(const char* text, float px, const ImVec4& col,
                  float avail_width);

// Soft status / empty-state text helpers.
void StatusSuccess(const char* text);
void StatusError(const char* text);
void StatusWarn(const char* text);
void StatusInfo(const char* text);

// Primary (amber) / secondary (stone) / copper / danger buttons.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool SecondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool CopperButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Toggle button: amber when on, stone when off. Returns true when flipped.
bool ToggleButton(const char* label, bool* value,
                  const ImVec2& size = ImVec2(0, 0));

// Branded progress bar (copper->amber fill, tick marks).
void BrandProgress(float fraction, const ImVec2& size);

// Dimension badge (2D / 3D) drawn inline.
void DimensionBadge(const char* dim);

// Key hint row for help menus: "[Ctrl+D]  Duplicate".
void KeyHint(const char* keys, const char* description);

// Branded title block (headstone mark + title + amber rule + subtitle).
void BrandBlock(const char* title, const char* subtitle, float avail_width);
// Larger hero variant for the loading splash.
void BrandHero(const char* title, const char* tagline, float avail_width);

}  // namespace theme
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
