#include "editor/plugins/ShipyardPanels.h"

#include "editor/ui/Theme.h"
#include "plugins/shipyard/ShipyardPlugin.h"
#include "scene/JsonTree.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

namespace fs = std::filesystem;
using namespace shipyard;

ImU32 col(const float* c, float shade = 1.0f, float a = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.0f, c[0] * shade), std::min(1.0f, c[1] * shade),
                                               std::min(1.0f, c[2] * shade), a));
}

ImU32 vcol(const ImVec4& v, float a = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * a));
}

// Designer state (one per editor session).
struct Works {
  Design design = ShipSim::sloop_design();
  std::vector<Design> undo, redo;
  std::string path = "shipyard/designs/sloop.ship.json";  // project-relative
  char name_buf[128] = {};
  char file_buf[128] = "my_airship";
  int selected = 0;
  int rot = 0;
  float cell = 18.0f;
  bool dirty = false;
  bool loaded_once = false;
  std::string status;
  // Drag painting: one undo step per stroke.
  bool stroke = false;
};

Works& works() {
  static Works w;
  return w;
}

void push_undo(Works& w) {
  w.undo.push_back(w.design);
  if (w.undo.size() > 100) w.undo.erase(w.undo.begin());
  w.redo.clear();
  w.dirty = true;
}

void sync_name(Works& w) {
  std::snprintf(w.name_buf, sizeof(w.name_buf), "%s", w.design.name.c_str());
}

bool load_into(Works& w, const std::string& project, const std::string& rel) {
  Design d;
  std::string err;
  if (!load_design((fs::path(project) / rel).string(), &d, &err)) {
    w.status = "Couldn't read the plans: " + err;
    return false;
  }
  w.design = std::move(d);
  w.path = rel;
  w.undo.clear();
  w.redo.clear();
  w.dirty = false;
  sync_name(w);
  w.status = "Unrolled " + rel + ".";
  return true;
}

bool save_to(Works& w, const std::string& project, const std::string& rel) {
  std::string err;
  if (!save_design((fs::path(project) / rel).string(), w.design, &err)) {
    w.status = "Couldn't file the plans: " + err;
    return false;
  }
  w.path = rel;
  w.dirty = false;
  w.status = "Plans filed at " + rel + ".";
  return true;
}

bool set_flagship(const std::string& project, const std::string& rel, std::string* why) {
  const fs::path p = fs::path(shipyard_dir(project)) / "sky.json";
  json::Value root = json::Value::object();
  std::ifstream in(p, std::ios::binary);
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string err;
    if (!json::parse(ss.str(), &root, &err) || !root.is_object()) {
      *why = "sky.json is broken: " + err;
      return false;
    }
  }
  in.close();
  root.set("player_design", json::Value::string(rel));
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << json::write(root);
  if (!out) {
    *why = "could not write " + p.string();
    return false;
  }
  return true;
}

void draw_part(ImDrawList* dl, const PartLibrary& lib, const Placed& p, ImVec2 origin, float cell,
               float alpha, bool loose) {
  const PartDef* d = lib.get(lib.find(p.part));
  if (!d) {
    const ImVec2 a(origin.x + p.x * cell, origin.y + p.y * cell);
    dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(200, 40, 200, 200));
    return;
  }
  int w = 1, h = 1;
  footprint(*d, p.rot, &w, &h);
  const ImVec2 a(origin.x + p.x * cell + 1, origin.y + p.y * cell + 1);
  const ImVec2 b(origin.x + (p.x + w) * cell - 1, origin.y + (p.y + h) * cell - 1);
  dl->AddRectFilled(a, b, col(d->color, 1.0f, alpha), 2.0f);
  dl->AddRect(a, b, loose ? vcol(theme::Danger(), alpha) : col(d->color, 0.6f, alpha), 2.0f, 0,
              loose ? 2.5f : 1.0f);
  const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  const float r = std::min(b.x - a.x, b.y - a.y) * 0.3f;
  switch (d->kind) {
    case PartKind::Engine: {
      float ex = 0, ey = 0;
      engine_dir(p.rot, &ex, &ey);
      // Arrow: which way she pushes; flame stub at the nozzle.
      const ImVec2 tip(c.x + ex * r * 1.3f, c.y + ey * r * 1.3f);
      const ImVec2 l(c.x - ey * r * 0.8f - ex * r * 0.4f, c.y + ex * r * 0.8f - ey * r * 0.4f);
      const ImVec2 rr(c.x + ey * r * 0.8f - ex * r * 0.4f, c.y - ex * r * 0.8f - ey * r * 0.4f);
      dl->AddTriangleFilled(tip, l, rr, IM_COL32(40, 30, 25, static_cast<int>(230 * alpha)));
      const float half = (ex != 0 ? (b.x - a.x) : (b.y - a.y)) * 0.5f;
      dl->AddCircleFilled(ImVec2(c.x - ex * half, c.y - ey * half), r * 0.5f,
                          IM_COL32(255, 150, 50, static_cast<int>(220 * alpha)));
      break;
    }
    case PartKind::Gun:
      dl->AddCircleFilled(c, r, IM_COL32(110, 105, 110, static_cast<int>(255 * alpha)));
      dl->AddLine(c, ImVec2(c.x, c.y - r * 2.2f), IM_COL32(20, 20, 20, static_cast<int>(255 * alpha)),
                  d->damage >= 30 ? 5.0f : 3.0f);
      break;
    case PartKind::Reactor:
      dl->AddCircleFilled(c, r, IM_COL32(255, 140, 45, static_cast<int>(255 * alpha)));
      break;
    case PartKind::Shield:
      dl->AddCircle(c, r * 1.2f, IM_COL32(140, 215, 255, static_cast<int>(255 * alpha)), 0, 2.0f);
      break;
    case PartKind::Quarters:
      dl->AddRectFilled(ImVec2(c.x - r * 1.5f, c.y - r * 0.4f), ImVec2(c.x + r * 1.5f, c.y + r * 0.4f),
                        IM_COL32(220, 195, 150, static_cast<int>(255 * alpha)));
      break;
    default:
      break;
  }
}

void stat_row(const char* label, const char* fmt, double v) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextColored(theme::TextMuted(), "%s", label);
  ImGui::TableSetColumnIndex(1);
  ImGui::Text(fmt, v);
}

void draw_works(ShipyardPlugin& p) {
  Works& w = works();
  const PartLibrary& lib = p.lib();
  const std::string& project = p.project_dir();
  if (!w.loaded_once) {
    w.loaded_once = true;
    std::error_code ec;
    const auto designs = list_designs(project);
    if (!designs.empty()) load_into(w, project, designs.front());
    else sync_name(w);
    w.status.clear();
  }
  if (lib.parts.empty()) {
    ImGui::TextColored(theme::Danger(), "No parts in shipyard/parts.json.");
    return;
  }
  w.selected = std::clamp(w.selected, 0, static_cast<int>(lib.parts.size()) - 1);

  // ---- File row --------------------------------------------------------------
  ImGui::SetNextItemWidth(220);
  if (ImGui::InputText("Ship name", w.name_buf, sizeof(w.name_buf))) {
    if (w.design.name != w.name_buf) {
      push_undo(w);
      w.design.name = w.name_buf;
    }
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(260);
  if (ImGui::BeginCombo("##plans", w.path.c_str())) {
    for (const std::string& d : list_designs(project)) {
      if (ImGui::Selectable(d.c_str(), d == w.path)) load_into(w, project, d);
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Save plans")) save_to(w, project, w.path);
  ImGui::SameLine();
  if (ImGui::Button("Save as...")) ImGui::OpenPopup("##saveas");
  ImGui::SameLine();
  if (ImGui::Button("Fresh keel")) {
    push_undo(w);
    w.design = Design{};
    w.design.name = "New airship";
    sync_name(w);
  }
  ImGui::SameLine();
  if (ImGui::Button("Fly this one")) {
    std::string why;
    if (w.dirty) save_to(w, project, w.path);
    if (set_flagship(project, w.path, &why)) {
      p.reload_data(nullptr);
      w.status = "Next launch, the posse flies " + w.path + ".";
    } else {
      w.status = why;
    }
  }
  if (ImGui::BeginPopup("##saveas")) {
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##file", w.file_buf, sizeof(w.file_buf));
    ImGui::SameLine();
    ImGui::TextDisabled(".ship.json");
    if (ImGui::Button("File it")) {
      std::string stem = w.file_buf;
      for (char& c : stem) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
      }
      if (!stem.empty()) save_to(w, project, "shipyard/designs/" + stem + ".ship.json");
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  const Validation v = validate(lib, w.design);

  // ---- Palette ------------------------------------------------------------------
  ImGui::BeginChild("##palette", ImVec2(210, 0), true);
  ImGui::TextColored(theme::Accent(), "PARTS BIN");
  for (int i = 0; i < static_cast<int>(lib.parts.size()); ++i) {
    const PartDef& d = lib.parts[static_cast<std::size_t>(i)];
    ImGui::PushID(i);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##pick", w.selected == i, 0, ImVec2(0, 22))) w.selected = i;
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::Text("%s (%s)", d.name.c_str(), part_kind_name(d.kind));
      ImGui::TextColored(theme::TextMuted(), "%dx%d cells, %.1f t, %d hp", d.w, d.h, static_cast<double>(d.mass), d.hp);
      if (d.power > 0) ImGui::Text("Makes %.0f steam", static_cast<double>(d.power));
      if (d.power < 0) ImGui::Text("Burns %.1f steam", static_cast<double>(-d.power));
      if (d.crew > 0) ImGui::Text("Bunks %d hands", d.crew);
      if (d.crew < 0) ImGui::Text("Needs %d hand(s)", -d.crew);
      if (d.thrust > 0) ImGui::Text("Thrust %.0f", static_cast<double>(d.thrust));
      if (d.damage > 0) ImGui::Text("%d dmg x %.1f/s, range %.0f", d.damage, static_cast<double>(d.fire_rate), static_cast<double>(d.range));
      if (d.shield > 0) ImGui::Text("Shield %.0f (+%.0f/s)", static_cast<double>(d.shield), static_cast<double>(d.shield_regen));
      if (d.capacity > 0) ImGui::Text("Holds %d crates", d.capacity);
      ImGui::EndTooltip();
    }
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + 2, at.y + 4), ImVec2(at.x + 18, at.y + 18), col(d.color), 2.0f);
    ImGui::SameLine(26);
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::PopID();
  }
  ImGui::Separator();
  ImGui::Text("Facing: %s", w.rot == 0 ? "bow" : w.rot == 1 ? "starboard" : w.rot == 2 ? "stern" : "port");
  if (ImGui::Button("Turn (R)")) w.rot = (w.rot + 1) & 3;
  ImGui::TextColored(theme::TextMuted(), "LMB place / drag\nRMB erase\nCtrl+LMB turn a part\nR turn, wheel zoom\nCtrl+Z / Ctrl+Y");
  ImGui::EndChild();
  ImGui::SameLine();

  // ---- Slipway ------------------------------------------------------------------
  const float side_w = 250.0f;
  ImGui::BeginChild("##slipway", ImVec2(ImGui::GetContentRegionAvail().x - side_w, 0), true,
                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_HorizontalScrollbar);
  const float cell = w.cell;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 size(kMaxGrid * cell, kMaxGrid * cell);
  ImGui::InvisibleButton("##grid", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), vcol(theme::CanvasBg()));
  for (int i = 0; i <= kMaxGrid; ++i) {
    const ImU32 g = vcol(theme::GridLine(), i % 5 == 0 ? 1.0f : 0.45f);
    dl->AddLine(ImVec2(origin.x + i * cell, origin.y), ImVec2(origin.x + i * cell, origin.y + size.y), g);
    dl->AddLine(ImVec2(origin.x, origin.y + i * cell), ImVec2(origin.x + size.x, origin.y + i * cell), g);
  }
  std::vector<bool> loose(w.design.parts.size(), false);
  for (int i : v.loose) loose[static_cast<std::size_t>(i)] = true;
  for (std::size_t i = 0; i < w.design.parts.size(); ++i) {
    draw_part(dl, lib, w.design.parts[i], origin, cell, 1.0f, loose[i]);
  }
  // Centre of mass marker.
  if (v.stats.mass > 0) {
    const ImVec2 com(origin.x + v.stats.cx / kCell * cell, origin.y + v.stats.cy / kCell * cell);
    dl->AddCircle(com, 6, vcol(theme::Accent()), 0, 2.0f);
    dl->AddLine(ImVec2(com.x - 9, com.y), ImVec2(com.x + 9, com.y), vcol(theme::Accent()));
    dl->AddLine(ImVec2(com.x, com.y - 9), ImVec2(com.x, com.y + 9), vcol(theme::Accent()));
  }
  if (hovered) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f) w.cell = std::clamp(w.cell + io.MouseWheel * 2.0f, 8.0f, 40.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) w.rot = (w.rot + 1) & 3;
    const int cx = static_cast<int>(std::floor((io.MousePos.x - origin.x) / cell));
    const int cy = static_cast<int>(std::floor((io.MousePos.y - origin.y) / cell));
    const PartDef& sel = lib.parts[static_cast<std::size_t>(w.selected)];
    int fw = 1, fh = 1;
    footprint(sel, w.rot, &fw, &fh);
    // Anchor the ghost on the cursor's cell (top-left).
    Design probe = w.design;
    std::string why;
    const bool ok = place_part(lib, &probe, sel.id, cx, cy, w.rot, &why);
    draw_part(dl, lib, Placed{sel.id, cx, cy, w.rot}, origin, cell, 0.5f, false);
    dl->AddRect(ImVec2(origin.x + cx * cell, origin.y + cy * cell),
                ImVec2(origin.x + (cx + fw) * cell, origin.y + (cy + fh) * cell),
                vcol(ok ? theme::Success() : theme::Danger()), 0, 0, 2.0f);
    if (io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      Design next = w.design;
      if (rotate_at(lib, &next, cx, cy)) {
        push_undo(w);
        w.design = std::move(next);
      } else {
        w.status = "No room to turn that.";
      }
    } else if (!io.KeyCtrl && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ok) {
      if (!w.stroke) push_undo(w);
      w.stroke = true;
      w.design = std::move(probe);
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && part_at(lib, w.design, cx, cy) >= 0) {
      if (!w.stroke) push_undo(w);
      w.stroke = true;
      erase_at(lib, &w.design, cx, cy);
    }
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    w.stroke = false;
  }
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !w.undo.empty()) {
      w.redo.push_back(w.design);
      w.design = w.undo.back();
      w.undo.pop_back();
      sync_name(w);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && !w.redo.empty()) {
      w.undo.push_back(w.design);
      w.design = w.redo.back();
      w.redo.pop_back();
      sync_name(w);
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // ---- Inspection ---------------------------------------------------------------
  ImGui::BeginChild("##inspect", ImVec2(0, 0), true);
  if (v.ok) ImGui::TextColored(theme::Success(), "AIRWORTHY");
  else ImGui::TextColored(theme::Danger(), "GROUNDED");
  ImGui::SameLine();
  ImGui::TextColored(theme::TextMuted(), "%s", w.dirty ? "(unsaved)" : "");
  for (const std::string& e : v.errors) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::Danger(), "x %s", e.c_str());
    ImGui::PopTextWrapPos();
  }
  for (const std::string& e : v.warnings) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::Warning(), "! %s", e.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Separator();
  const ShipStats& s = v.stats;
  if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_SizingStretchProp)) {
    stat_row("Parts", "%.0f", s.parts);
    stat_row("Mass (t)", "%.1f", s.mass);
    stat_row("Hull hp", "%.0f", s.hp);
    stat_row("Steam", "%.1f", s.power_made);
    stat_row("  burned", "%.1f", s.power_used);
    stat_row("Hands", "%.0f", s.hands);
    stat_row("  bunks", "%.0f", s.berths);
    stat_row("Thrust fwd", "%.0f", s.thrust[kFwd]);
    stat_row("  astern", "%.0f", s.thrust[kBack]);
    stat_row("  port/stbd", "%.0f", std::min(s.thrust[kLeft], s.thrust[kRight]));
    stat_row("Accel (px/s2)", "%.0f", s.accel);
    stat_row("Top speed", "%.0f", s.top_speed);
    stat_row("Turn (rad/s2)", "%.2f", s.inertia > 0 ? std::min(s.torque_cw, s.torque_ccw) / s.inertia : 0.0);
    stat_row("Guns", "%.0f", s.guns);
    stat_row("Damage/s", "%.0f", s.dps);
    stat_row("Shield", "%.0f", s.shield);
    stat_row("Cargo", "%.0f", s.capacity);
    ImGui::EndTable();
  }
  if (!w.status.empty()) {
    ImGui::Separator();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::TextMuted(), "%s", w.status.c_str());
    ImGui::PopTextWrapPos();
  }
  for (const std::string& warn : p.data_warnings()) ImGui::TextColored(theme::Warning(), "%s", warn.c_str());
  ImGui::EndChild();
}

}  // namespace

void install_shipyard_panels() {
  ShipyardPlugin::set_editor_hook([](ShipyardPlugin& p, plugin::Registrar& reg) {
    ShipyardPlugin* sp = &p;
    plugin::EditorPanel works_panel;
    works_panel.id = "shipyard.works";
    works_panel.title = "Airship Works";
    works_panel.menu_label = "Airship Works (Shipyard)";
    works_panel.before_begin = [] {
      ImGui::SetNextWindowSize(ImVec2(1040, 760), ImGuiCond_FirstUseEver);
    };
    works_panel.draw = [sp] { draw_works(*sp); };
    reg.add_editor_panel(std::move(works_panel));
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
