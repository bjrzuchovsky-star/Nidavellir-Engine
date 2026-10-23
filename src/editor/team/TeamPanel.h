#pragma once

// Live Team Claims UI shared by both editors: the Team menu, the Team
// panel (Host / Join / Users), Hierarchy lock badges and the viewport
// overlay (remote cursors + selection outlines).

#include "editor/team/TeamSession.h"

#include <cstdint>
#include <functional>

struct ImDrawList;
struct ImVec2;

namespace ts {
namespace tombstone {
namespace editor {
namespace team_ui {

// "#rrggbb" -> packed ImU32 (alpha 0..255).
unsigned int colour_u32(const std::string& hex, int alpha = 255);

// Team menu contents (call between BeginMenu("Team") / EndMenu()).
void draw_menu(TeamSession& t, bool* show_panel);
// The Team window.
void draw_panel(TeamSession& t, bool* open, const char* window_id);
// After a Hierarchy row: a coloured padlock + holder name when another
// rider holds the entity. True when it is held by someone else.
bool draw_lock_badge(const TeamSession* t, std::uint64_t id);
// Same, drawn into a Hierarchy row's rect (right-aligned, before `pad` px).
bool draw_lock_badge_in(const TeamSession* t, std::uint64_t id, const ImVec2& mn,
                        const ImVec2& mx, float pad);
// Viewport overlay. project(x, y, z, &screen) maps a presence cursor to
// the screen; bounds(id, &min, &max) gives an entity's screen rect.
void draw_presence(TeamSession& t, ImDrawList* draw,
                   const std::function<bool(const float* xyz, ImVec2* out)>& project,
                   const std::function<bool(std::uint64_t id, ImVec2* mn, ImVec2* mx)>& bounds);
// Status-bar chip ("Posse of 3").
void draw_status_chip(TeamSession* t);

}  // namespace team_ui
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
