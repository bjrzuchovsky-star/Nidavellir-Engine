#include "editor/tools/AutoTilePanel.h"
#include "editor/ux/Tooltip.h"

#include "core/PngWrite.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {
std::string slug(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!o.empty() && o.back() != '_') o += '_';
  }
  while (!o.empty() && o.back() == '_') o.pop_back();
  return o.empty() ? "terrain" : o;
}

const char* kModeNames[] = {"47-blob (8 neighbours)", "Wang 16 (4 edges)", "Rule tiles"};
AutoTileMode mode_of(int i) {
  return i == 1 ? AutoTileMode::Wang16 : i == 2 ? AutoTileMode::Rules : AutoTileMode::Blob47;
}
}  // namespace

AutoTilePanel& AutoTilePanel::get() {
  static AutoTilePanel p;
  return p;
}

void AutoTilePanel::open(const std::string& project_dir, const std::string& project_name) {
  if (project_dir != project_dir_) {
    project_dir_ = project_dir;
    project_name_ = project_name;
    brush_path_.clear();
    current_ = -1;
    scratch_ = TileMapData(20, 12, 32);
  }
  rescan();
  open_ = true;
}

void AutoTilePanel::rescan() {
  paths_ = autotile_json::list(project_dir_);
  loaded_.assign(paths_.size(), AutoTileSet{});
  errors_.assign(paths_.size(), std::string());
  for (std::size_t i = 0; i < paths_.size(); ++i) {
    autotile_json::load_file((fs::path(project_dir_) / paths_[i]).string(), &loaded_[i], &errors_[i]);
  }
  if (current_ >= static_cast<int>(paths_.size())) current_ = -1;
  if (current_ < 0 && !paths_.empty()) current_ = 0;
}

const AutoTileSet* AutoTilePanel::set(const std::string& rel) const {
  for (std::size_t i = 0; i < paths_.size(); ++i) {
    if (paths_[i] == rel && errors_[i].empty()) return &loaded_[i];
  }
  return nullptr;
}

const AutoTileSet* AutoTilePanel::brush(const std::string& project_dir) const {
  if (brush_path_.empty() || project_dir != project_dir_) return nullptr;
  return set(brush_path_);
}

bool AutoTilePanel::select_brush(const std::string& rel) {
  if (rel.empty()) {
    brush_path_.clear();
    return true;
  }
  if (!set(rel)) return false;
  brush_path_ = rel;
  for (std::size_t i = 0; i < paths_.size(); ++i) {
    if (paths_[i] == rel) current_ = static_cast<int>(i);
  }
  return true;
}

std::string AutoTilePanel::create_terrain(const std::string& project_dir, const std::string& name,
                                          AutoTileMode mode, int tile_size, const unsigned char fill[4],
                                          const unsigned char rim[4], std::string* error) {
  const std::string s = slug(name);
  const std::string suffix = mode == AutoTileMode::Blob47 ? "_blob47" : "_wang16";
  const std::string png_rel = "assets/tiles/" + s + suffix + ".png";
  const TemplateImage img =
      autotile_template(mode == AutoTileMode::Rules ? AutoTileMode::Wang16 : mode, tile_size, fill, rim);
  if (!write_png_rgba((fs::path(project_dir) / png_rel).string(), img.width, img.height, img.rgba)) {
    if (error) *error = "cannot write " + png_rel;
    return {};
  }
  AutoTileSet set;
  set.name = name;
  set.mode = mode;
  set.tileset = png_rel;
  set.tile_size = tile_size;
  set.base = 1;
  if (mode == AutoTileMode::Rules) {
    // Rule tiles over the Wang template: edges decide, corners are free.
    // Most specific first; the last rule (no neighbours) always matches.
    std::vector<std::pair<int, std::string>> rules;
    for (int m = 15; m >= 0; --m) {
      std::string p = ".";
      p += (m & 1) ? 'x' : 'o';
      p += "./";
      p += (m & 8) ? 'x' : 'o';
      p += '-';
      p += (m & 2) ? 'x' : 'o';
      p += "/.";
      p += (m & 4) ? 'x' : 'o';
      p += '.';
      rules.push_back({1 + m, p});
    }
    for (const auto& [t, p] : rules) set.rules.push_back(TileRule{t, p});
    set.fill = 16;
  }
  const std::string rel = "tiles/" + s + ".autotile.json";
  if (!autotile_json::save_file((fs::path(project_dir) / rel).string(), set, error)) return {};
  return rel;
}

void AutoTilePanel::paint_demo() {
  const AutoTileSet* s = current_ >= 0 && current_ < static_cast<int>(loaded_.size()) ? &loaded_[current_] : nullptr;
  if (!s) return;
  scratch_ = TileMapData(20, 12, s->tile_size);
  AutoTileSet island = *s;
  island.edges_connect = false;
  const char* shape[] = {"....................", "...#####............", "..########....###...",
                         "..#########..#####..", "...########..#####..", "....######...##.##..",
                         "....###.##....#.....", ".....#####..........", "......###.....####..",
                         "..............####..", "....................", "...................."};
  for (int r = 0; r < 12; ++r) {
    for (int c = 0; c < 20; ++c) {
      if (shape[r][c] == '#') scratch_.set(c, r, island.full_tile());
    }
  }
  retile_all(scratch_, island);
}

void AutoTilePanel::tile_quad(ImDrawList* dl, const AutoTileSet& s, int tile, const ImVec2& p0,
                              const ImVec2& p1) {
  if (tile <= 0) return;
  if (!s.tileset.empty()) {
    const TextureInfo& t = textures_.get((fs::path(project_dir_) / s.tileset).string());
    float u0, v0, u1, v1;
    if (t.ok && tileset_uv(tile, s.tile_size, t.width, t.height, &u0, &v0, &u1, &v1)) {
      dl->AddImage((ImTextureID)(std::uintptr_t)t.handle, p0, p1, ImVec2(u0, v0), ImVec2(u1, v1));
      return;
    }
  }
  const BuiltinTile& b = builtin_tile(tile);
  dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(ImVec4(b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3])));
}

void AutoTilePanel::draw_slots(const AutoTileSet& s) {
  const int n = s.slot_count();
  const float cell = 30.0f, gap = 6.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per = std::max(1, static_cast<int>((avail + gap) / (cell + gap)));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = 0; i < n; ++i) {
    if (i % per) ImGui::SameLine(0, gap);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(cell, cell));
    const int tile = s.slot_tile(i);
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), theme::U32(theme::Charcoal()));
    tile_quad(dl, s, tile, p, ImVec2(p.x + cell, p.y + cell));
    dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), theme::U32(theme::Border()));
    if (ImGui::IsItemHovered()) {
      std::uint8_t m = 0;
      std::string what;
      if (s.mode == AutoTileMode::Blob47) m = blob47_masks()[static_cast<std::size_t>(i)];
      if (s.mode == AutoTileMode::Wang16) {
        m = static_cast<std::uint8_t>(((i & 1) ? kNbN : 0) | ((i & 2) ? kNbE : 0) | ((i & 4) ? kNbS : 0) |
                                      ((i & 8) ? kNbW : 0));
      }
      if (s.mode == AutoTileMode::Rules) what = "  pattern " + s.rules[static_cast<std::size_t>(i)].pattern;
      ux::SetTooltip("slot %d -> tile #%d%s%s", i, tile,
                        s.mode == AutoTileMode::Rules ? "" : ("  mask " + std::to_string(m)).c_str(), what.c_str());
    }
  }
}

void AutoTilePanel::draw_scratch(const AutoTileSet& s) {
  const float avail = ImGui::GetContentRegionAvail().x;
  const float cell = std::clamp(std::floor(avail / scratch_.cols), 8.0f, 32.0f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(cell * scratch_.cols, cell * scratch_.rows);
  ImGui::InvisibleButton("##scratch", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), theme::U32(theme::CanvasBg()));
  AutoTileSet island = s;
  island.edges_connect = false;
  if (ImGui::IsItemActive()) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int c = static_cast<int>((m.x - p.x) / cell), r = static_cast<int>((m.y - p.y) / cell);
    paint_autotile(scratch_, island, c, r, 1, ImGui::IsMouseDown(ImGuiMouseButton_Right));
  }
  for (int r = 0; r < scratch_.rows; ++r) {
    for (int c = 0; c < scratch_.cols; ++c) {
      const ImVec2 a(p.x + c * cell, p.y + r * cell);
      tile_quad(dl, s, scratch_.at(c, r), a, ImVec2(a.x + cell, a.y + cell));
    }
  }
  for (int c = 0; c <= scratch_.cols; ++c) {
    dl->AddLine(ImVec2(p.x + c * cell, p.y), ImVec2(p.x + c * cell, p.y + size.y), theme::U32(theme::GridLine(), 0.5f));
  }
  for (int r = 0; r <= scratch_.rows; ++r) {
    dl->AddLine(ImVec2(p.x, p.y + r * cell), ImVec2(p.x + size.x, p.y + r * cell), theme::U32(theme::GridLine(), 0.5f));
  }
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), theme::U32(theme::Border()));
}

void AutoTilePanel::draw() {
  if (!open_) return;
  if (pinned_) {
    ImGui::SetNextWindowPos(ImVec2(pin_[0], pin_[1]), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pin_[2], pin_[3]), ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowSize(ImVec2(700, 720), ImGuiCond_FirstUseEver);
  }
  textures_.poll_changes(ImGui::GetTime());  // repainted tilesets reload
  if (!ImGui::Begin("Autotiles###ts_autotiles", &open_)) {
    ImGui::End();
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted("AUTOTILES");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("terrain brushes for the TileMap painter");

  ImGui::SeparatorText("Terrains");
  if (ImGui::RadioButton("Plain tiles (no autotile)", brush_path_.empty())) brush_path_.clear();
  for (std::size_t i = 0; i < paths_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    const bool bad = !errors_[i].empty();
    ImGui::BeginDisabled(bad);
    if (ImGui::RadioButton("##brush", brush_path_ == paths_[i])) select_brush(paths_[i]);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const std::string label = bad ? paths_[i] : loaded_[i].name + "  (" + to_string(loaded_[i].mode) + ")  " + paths_[i];
    if (ImGui::Selectable(label.c_str(), current_ == static_cast<int>(i))) current_ = static_cast<int>(i);
    if (bad) {
      ImGui::SameLine();
      ImGui::TextColored(theme::Danger(), "%s", errors_[i].c_str());
    }
    ImGui::PopID();
  }
  if (ImGui::Button("Rescan")) rescan();
  ImGui::SameLine();
  ImGui::BeginDisabled(brush_path_.empty());
  if (ImGui::Button("Re-tile selected TileMap")) retile_request_ = true;
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled(brush_path_.empty() ? "Painter uses plain tiles" : "Painter paints terrain: %s",
                      brush_path_.c_str());

  ImGui::SeparatorText("New terrain");
  ImGui::SetNextItemWidth(140);
  ImGui::InputText("Name", new_name_, sizeof(new_name_));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(190);
  ImGui::Combo("Kind", &new_mode_, kModeNames, 3);
  ImGui::SetNextItemWidth(100);
  ImGui::InputInt("Tile px", &new_size_, 8, 16);
  new_size_ = std::clamp(new_size_, 8, 128);
  ImGui::SameLine();
  ImGui::ColorEdit4("Fill", new_fill_, ImGuiColorEditFlags_NoInputs);
  ImGui::SameLine();
  ImGui::ColorEdit4("Rim", new_rim_, ImGuiColorEditFlags_NoInputs);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::AccentHover());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextOnAccent());
  if (ImGui::Button("Create")) {
    unsigned char f[4], r[4];
    for (int k = 0; k < 4; ++k) {
      f[k] = static_cast<unsigned char>(std::clamp(new_fill_[k], 0.0f, 1.0f) * 255.0f);
      r[k] = static_cast<unsigned char>(std::clamp(new_rim_[k], 0.0f, 1.0f) * 255.0f);
    }
    std::string err;
    const std::string rel = create_terrain(project_dir_, new_name_, mode_of(new_mode_), new_size_, f, r, &err);
    if (rel.empty()) {
      status_ = err;
    } else {
      rescan();
      select_brush(rel);
      status_ = "Created " + rel + " (template tileset to paint over)";
    }
  }
  ImGui::PopStyleColor(3);
  if (!status_.empty()) ImGui::TextColored(theme::TextMuted(), "%s", status_.c_str());

  if (current_ >= 0 && current_ < static_cast<int>(loaded_.size()) && errors_[current_].empty()) {
    const AutoTileSet& s = loaded_[static_cast<std::size_t>(current_)];
    ImGui::SeparatorText((s.name + " - " + std::to_string(s.slot_count()) + " slots").c_str());
    ImGui::TextDisabled("Tileset %s | %d px | base #%d | border counts as terrain: %s",
                        s.tileset.empty() ? "(built-in palette)" : s.tileset.c_str(), s.tile_size, s.base,
                        s.edges_connect ? "yes" : "no");
    draw_slots(s);
    ImGui::SeparatorText("Try it (left paint, right erase)");
    draw_scratch(s);
    if (ImGui::Button("Clear")) scratch_ = TileMapData(scratch_.cols, scratch_.rows, s.tile_size);
    ImGui::SameLine();
    if (ImGui::Button("Demo island")) paint_demo();
  } else if (paths_.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(theme::TextMuted(), "No terrains yet: create one above (tiles/<name>.autotile.json).");
  }
  ImGui::End();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
