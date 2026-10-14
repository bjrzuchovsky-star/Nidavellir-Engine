// Tile Palette + Supply Wagon panels, toolbar tool strip and the Inspector's
// TileMap / Sprite sections.
#include "editor/screens/Editor2DScreen.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

ImTextureID tex_id(std::uint64_t handle) {
  return (ImTextureID)(std::uintptr_t)handle;
}

struct ToolDef {
  TileTool tool;
  const char* label;
  const char* key;
  const char* help;
};

constexpr ToolDef kTools[] = {
    {TileTool::Select, "Select", "V", "Pick, drag and box-select entities"},
    {TileTool::Paint, "Brush", "B", "Paint the selected TileMap (drag = one step)"},
    {TileTool::Erase, "Erase", "E", "Clear tiles (drag = one step)"},
    {TileTool::Fill, "Fill", "U", "Bucket-fill the matching region"},
    {TileTool::Rect, "Rect", "R", "Drag a box of tiles (Shift: erase box)"},
    {TileTool::Eyedropper, "Pick", "I", "Pick a tile from the map (or Alt+click)"},
};

const std::vector<std::string>& image_extensions() {
  static const std::vector<std::string> kExts = {
      ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd", ".pnm", ".ppm",
      ".pgm"};
  return kExts;
}

// Accept an asset drop on the last item; returns the dropped path.
bool accept_asset_drop(std::string* rel_out) {
  bool got = false;
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(assets::kDragPayload)) {
      const char* data = static_cast<const char*>(payload->Data);
      std::size_t len = 0;
      while (len < static_cast<std::size_t>(payload->DataSize) &&
             data[len] != '\0') {
        ++len;
      }
      *rel_out = std::string(data, len);
      got = true;
    }
    ImGui::EndDragDropTarget();
  }
  return got;
}

}  // namespace

void draw_tile_quad(ImDrawList* draw, const TextureInfo* tileset,
                    int tile_size, int palette_count, int tile_id,
                    const ImVec2& p0, const ImVec2& p1, float alpha) {
  if (tile_id <= 0) {
    return;
  }
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  if (tileset && tileset->ok && tileset->handle != 0 && tile_size > 0 &&
      tile_id <= palette_count) {
    const int per_row = tileset->width / tile_size;
    if (per_row > 0) {
      const int k = tile_id - 1;
      const float tw = static_cast<float>(tileset->width);
      const float th = static_cast<float>(tileset->height);
      const float u0 = static_cast<float>((k % per_row) * tile_size) / tw;
      const float v0 = static_cast<float>((k / per_row) * tile_size) / th;
      draw->AddImage(tex_id(tileset->handle), p0, p1, ImVec2(u0, v0),
                     ImVec2(u0 + tile_size / tw, v0 + tile_size / th),
                     IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f)));
      return;
    }
  }
  const BuiltinTile& bt = builtin_tile(tile_id);
  const ImVec4 base(bt.rgba[0], bt.rgba[1], bt.rgba[2], bt.rgba[3] * alpha);
  draw->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(base));
  const float px = p1.x - p0.x;
  if (px >= 10.0f) {
    // Hand-cut bevel: lit top edge, shaded bottom lip.
    const ImVec4 dark(base.x * 0.72f, base.y * 0.72f, base.z * 0.72f, base.w);
    const ImVec4 lit(std::min(1.0f, base.x * 1.2f), std::min(1.0f, base.y * 1.2f),
                     std::min(1.0f, base.z * 1.2f), base.w);
    const float lip = std::max(1.0f, px * 0.12f);
    draw->AddRectFilled(ImVec2(p0.x, p1.y - lip), p1,
                        ImGui::ColorConvertFloat4ToU32(dark));
    draw->AddLine(ImVec2(p0.x, p0.y + 0.5f), ImVec2(p1.x, p0.y + 0.5f),
                  ImGui::ColorConvertFloat4ToU32(lit));
  }
}

void Editor2DScreen::draw_tile_swatch(ImDrawList* draw,
                                      const Entity2D* tilemap_entity,
                                      int tile_id, const ImVec2& p0,
                                      const ImVec2& p1) {
  const TextureInfo* set = nullptr;
  int count = kBuiltinTileCount;
  int tile_size = 32;
  if (tilemap_entity && tilemap_entity->tilemap) {
    tile_size = tilemap_entity->tilemap->tile_size;
    count = palette_count(*tilemap_entity);
    if (tileset_ready(*tilemap_entity)) {
      set = &texture(tilemap_entity->tilemap->tileset);
    }
  }
  draw_tile_quad(draw, set, tile_size, count, tile_id, p0, p1);
}

void Editor2DScreen::draw_tool_buttons(float button_width, int per_row) {
  for (std::size_t i = 0; i < sizeof(kTools) / sizeof(kTools[0]); ++i) {
    const ToolDef& t = kTools[i];
    if (i > 0 && per_row > 0 && i % static_cast<std::size_t>(per_row) != 0) {
      ImGui::SameLine(0.0f, 3.0f);
    }
    bool on = tool_ == t.tool;
    char label[32];
    std::snprintf(label, sizeof(label), "%s##tool%zu", t.label, i);
    if (theme::ToggleButton(label, &on, ImVec2(button_width, 0))) {
      set_tool(t.tool);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s (%s): %s", t.label, t.key, t.help);
    }
  }
  // Current brush tile.
  ImGui::SameLine(0.0f, 6.0f);
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(h, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Entity2D* target = workspace_.find(paint_target());
  dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), theme::U32(theme::Charcoal()));
  draw_tile_swatch(dl, target, brush_tile_, ImVec2(p.x + 2, p.y + 2),
                   ImVec2(p.x + h - 2, p.y + h - 2));
  dl->AddRect(p, ImVec2(p.x + h, p.y + h), theme::U32(theme::Accent()));
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Brush tile #%d, size %d (Tile Palette)", brush_tile_,
                      brush_size_);
  }
}

void Editor2DScreen::draw_tile_palette() {
  Entity2D* target = workspace_.find(paint_target());
  theme::SectionHeader("Tile Palette",
                       target ? target->name.c_str() : "no TileMap");
  {
    // One row of six tools when the panel is wide, else two rows of three.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float swatch = ImGui::GetFrameHeight() + 6.0f;
    if (avail >= 6.0f * 50.0f + 5.0f * 3.0f + swatch) {
      draw_tool_buttons(50.0f, 6);
    } else {
      draw_tool_buttons(
          std::clamp((avail - swatch - 2.0f * 3.0f) / 3.0f, 40.0f, 90.0f), 3);
    }
  }

  ImGui::TextDisabled("Brush");
  ImGui::SameLine();
  for (int n = 1; n <= 3; ++n) {
    char label[16];
    std::snprintf(label, sizeof(label), "%dx%d##brush%d", n, n, n);
    bool on = brush_size_ == n;
    if (n > 1) {
      ImGui::SameLine(0.0f, 3.0f);
    }
    if (theme::ToggleButton(label, &on, ImVec2(40, 0))) {
      set_brush_size(n);
    }
  }

  if (!target) {
    ImGui::Spacing();
    theme::StatusInfo(
        "No TileMap in hand. Select one in the Scene list, or stake a new "
        "one. Tiles below preview the built-in palette.");
    if (theme::PrimaryButton("+ TileMap", ImVec2(-1, 0))) {
      create_tilemap();
      target = workspace_.find(paint_target());
    }
  }

  const int count = target ? palette_count(*target) : kBuiltinTileCount;
  const bool from_set = target && tileset_ready(*target);
  if (target && !target->tilemap->tileset.empty() && !from_set) {
    theme::StatusWarn("Tileset image missing or smaller than one tile. "
                      "Painting with the built-in palette.");
  } else if (from_set) {
    ImGui::TextDisabled("Tileset %s  |  %d tiles",
                        assets::file_name(target->tilemap->tileset).c_str(),
                        count);
  } else {
    ImGui::TextDisabled("Built-in frontier palette  |  %d tiles", count);
  }

  if (brush_tile_ > count) {
    set_brush_tile(1);
  }
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float s = 22.0f;
    ImGui::Dummy(ImVec2(s, s));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_tile_swatch(dl, target, brush_tile_, p, ImVec2(p.x + s, p.y + s));
    dl->AddRect(p, ImVec2(p.x + s, p.y + s), theme::U32(theme::Accent()));
    ImGui::SameLine();
    if (from_set) {
      ImGui::Text("Tile #%d", brush_tile_);
    } else {
      ImGui::Text("Tile #%d  %s", brush_tile_, builtin_tile(brush_tile_).name);
    }
    ImGui::SameLine();
    draw_tile_solid_controls(target);
  }
  theme::Divider();

  // Swatch grid (clipped by rows so big tilesets stay cheap).
  ImGui::BeginChild("##TileGrid", ImVec2(0, 0), ImGuiChildFlags_None);
  const float cell = 34.0f;
  const float gap = 4.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per_line = std::max(1, static_cast<int>((avail + gap) / (cell + gap)));
  const int lines = (count + per_line - 1) / per_line;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Solidity is kept per tileset ("" = built-in palette).
  const std::string solid_set =
      (target && target->tilemap) ? target->tilemap->tileset : std::string();
  ImGuiListClipper clipper;
  clipper.Begin(lines, cell + gap);
  while (clipper.Step()) {
    for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
      for (int k = 0; k < per_line; ++k) {
        const int id = line * per_line + k + 1;
        if (id > count) {
          break;
        }
        if (k > 0) {
          ImGui::SameLine(0.0f, gap);
        }
        ImGui::PushID(id);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(cell, cell));
        const bool hot = ImGui::IsItemHovered();
        const bool rclicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        const bool solid = tile_solid(solid_set, id);
        const ImVec2 q(p.x + cell, p.y + cell);
        dl->AddRectFilled(p, q, theme::U32(theme::Charcoal()));
        draw_tile_swatch(dl, target, id, ImVec2(p.x + 2, p.y + 2),
                         ImVec2(q.x - 2, q.y - 2));
        if (id == brush_tile_) {
          dl->AddRect(p, q, theme::U32(theme::Accent()), 0.0f, 0, 2.5f);
        } else if (hot) {
          dl->AddRect(p, q, theme::U32(theme::Copper()), 0.0f, 0, 1.5f);
        } else {
          dl->AddRect(p, q, theme::U32(theme::Border()));
        }
        if (solid) {
          draw_solid_marker(dl, p, q);
        }
        if (hot) {
          if (from_set) {
            const int per_row = std::max(
                1, texture(target->tilemap->tileset).width /
                       target->tilemap->tile_size);
            ImGui::SetTooltip("#%d  (col %d, row %d)%s\nRight-click: toggle solid",
                              id, (id - 1) % per_row, (id - 1) / per_row,
                              solid ? "  | solid" : "");
          } else {
            ImGui::SetTooltip("#%d  %s%s\nRight-click: toggle solid", id,
                              builtin_tile(id).name, solid ? "  | solid" : "");
          }
        }
        if (clicked) {
          set_brush_tile(id);
          if (tool_ == TileTool::Select || tool_ == TileTool::Erase ||
              tool_ == TileTool::Eyedropper) {
            set_tool(TileTool::Paint);
          }
        }
        if (rclicked) {
          toggle_tile_solid(solid_set, id);
        }
        ImGui::PopID();
      }
    }
  }
  ImGui::EndChild();
}

bool Editor2DScreen::asset_combo(const char* label, const std::string& current,
                                 const char* none_label, std::string* rel_out) {
  bool chosen = false;
  const std::string preview =
      current.empty() ? std::string(none_label) : assets::file_name(current);
  if (ImGui::BeginCombo(label, preview.c_str())) {
    if (ImGui::Selectable(none_label, current.empty())) {
      *rel_out = std::string();
      chosen = true;
    }
    for (const std::string& rel : assets_) {
      ImGui::PushID(rel.c_str());
      const TextureInfo& t = texture(rel);
      const float s = ImGui::GetTextLineHeight();
      if (t.handle != 0) {
        ImGui::Image(tex_id(t.handle), ImVec2(s, s));
      } else {
        ImGui::Dummy(ImVec2(s, s));
      }
      ImGui::SameLine();
      if (ImGui::Selectable(rel.c_str(), rel == current)) {
        *rel_out = rel;
        chosen = true;
      }
      ImGui::PopID();
    }
    if (assets_.empty()) {
      ImGui::TextDisabled("Supply Wagon is empty. Import an image first.");
    }
    ImGui::EndCombo();
  }
  std::string dropped;
  if (accept_asset_drop(&dropped)) {
    *rel_out = dropped;
    chosen = true;
  }
  return chosen;
}

void Editor2DScreen::draw_supply_wagon() {
  // Pick up files dropped into assets/ from outside every couple of seconds.
  const double now = now_seconds();
  if (now - assets_refresh_time_ > 2.0) {
    refresh_assets();
  }
  char caption[48];
  std::snprintf(caption, sizeof(caption), "%zu image%s", assets_.size(),
                assets_.size() == 1 ? "" : "s");
  theme::SectionHeader("Supply Wagon", caption);
  const float bw = std::max(60.0f, (ImGui::GetContentRegionAvail().x -
                                    ImGui::GetStyle().ItemSpacing.x) * 0.5f);
  if (theme::PrimaryButton("Import...", ImVec2(bw, 0))) {
    std::error_code ec;
    import_browser_.open_file(std::filesystem::current_path(ec).string(),
                              image_extensions(),
                              "Pick an image to load into the Supply Wagon "
                              "(copied to <project>/assets)");
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Refresh", ImVec2(bw, 0))) {
    refresh_assets();
    textures_.poll_changes(now, 0.0);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  ImGui::TextWrapped("Drag an image onto the viewport to stake a sprite, or "
                     "right-click it for more.");
  ImGui::PopStyleColor();

  draw_wagon_sounds();
  if (assets_.empty()) {
    theme::StatusInfo("Wagon's empty. Import a PNG, or drop files into "
                      "<project>/assets.");
    return;
  }

  Entity2D* sel = workspace_.selected();
  ImGui::BeginChild("##WagonList", ImVec2(0, 0), ImGuiChildFlags_None);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float row_h = 44.0f;
  std::string assign_rel;
  std::string tileset_rel;
  std::string stake_rel;
  for (const std::string& rel : assets_) {
    ImGui::PushID(rel.c_str());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool is_sel = rel == wagon_selected_;
    if (ImGui::Selectable("##asset", is_sel,
                          ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(0, row_h))) {
      wagon_selected_ = rel;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && sel) {
        if (sel->tilemap) {
          tileset_rel = rel;
        } else {
          assign_rel = rel;
        }
      }
    }
    const TextureInfo& t = texture(rel);
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(assets::kDragPayload, rel.c_str(),
                                rel.size() + 1);
      if (t.handle != 0) {
        ImGui::Image(tex_id(t.handle), ImVec2(32, 32));
        ImGui::SameLine();
      }
      ImGui::Text("Stake %s", assets::file_name(rel).c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem()) {
      wagon_selected_ = rel;
      if (ImGui::MenuItem("Stake sprite at view centre")) {
        stake_rel = rel;
      }
      const bool can_assign = sel && !sel->tilemap;
      if (ImGui::MenuItem("Assign to selected", nullptr, false, can_assign)) {
        assign_rel = rel;
      }
      const bool can_tileset = sel && sel->tilemap;
      if (ImGui::MenuItem("Use as tileset", nullptr, false, can_tileset)) {
        tileset_rel = rel;
      }
      ImGui::EndPopup();
    }
    // Thumbnail (fit, keep aspect) + name + size, drawn over the row.
    const float th = row_h - 6.0f;
    const ImVec2 t0(p.x + 3.0f, p.y + 3.0f);
    dl->AddRectFilled(t0, ImVec2(t0.x + th, t0.y + th),
                      theme::U32(theme::Charcoal()));
    if (t.ok && t.handle != 0 && t.width > 0 && t.height > 0) {
      const float sx = th / static_cast<float>(t.width);
      const float sy = th / static_cast<float>(t.height);
      const float sc = std::min(sx, sy);
      const float w = t.width * sc;
      const float h = t.height * sc;
      const ImVec2 i0(t0.x + (th - w) * 0.5f, t0.y + (th - h) * 0.5f);
      dl->AddImage(tex_id(t.handle), i0, ImVec2(i0.x + w, i0.y + h));
    } else {
      dl->AddText(ImVec2(t0.x + th * 0.4f, t0.y + th * 0.3f),
                  theme::U32(theme::Warning()), "?");
    }
    dl->AddRect(t0, ImVec2(t0.x + th, t0.y + th), theme::U32(theme::Border()));
    const float tx = t0.x + th + 8.0f;
    dl->AddText(ImVec2(tx, p.y + 5.0f), theme::U32(theme::Text()),
                assets::file_name(rel).c_str());
    char info[64];
    if (t.ok) {
      std::snprintf(info, sizeof(info), "%d x %d px", t.width, t.height);
    } else {
      std::snprintf(info, sizeof(info), "unreadable (%s)", t.error.c_str());
    }
    dl->AddText(ImVec2(tx, p.y + 5.0f + ImGui::GetTextLineHeightWithSpacing()),
                theme::U32(t.ok ? theme::TextMuted() : theme::Warning()), info);
    ImGui::PopID();
  }
  ImGui::EndChild();

  if (!assign_rel.empty() && sel) {
    assign_sprite(sel->id, assign_rel);
  }
  if (!tileset_rel.empty() && sel) {
    set_tileset(sel->id, tileset_rel);
  }
  if (!stake_rel.empty()) {
    create_sprite_at(stake_rel, workspace_.pan_x(), workspace_.pan_y());
  }
}

void Editor2DScreen::draw_inspector_components(Entity2D& e) {
  const std::uint64_t id = e.id;
  if (e.tilemap) {
    ImGui::SeparatorText("TileMap");
    const TileMapData& tm = *e.tilemap;
    int cols = tm.cols;
    int rows = tm.rows;
    int tile_size = tm.tile_size;
    const ImGuiInputTextFlags enter = ImGuiInputTextFlags_EnterReturnsTrue;
    if (ImGui::InputInt("Cols", &cols, 1, 8, enter)) {
      resize_tilemap(id, cols, workspace_.find(id)->tilemap->rows);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Width in cells. Enter or +/- applies; existing tiles "
                        "are kept.");
    }
    if (ImGui::InputInt("Rows", &rows, 1, 8, enter)) {
      resize_tilemap(id, workspace_.find(id)->tilemap->cols, rows);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Height in cells. Enter or +/- applies; existing "
                        "tiles are kept.");
    }
    if (ImGui::InputInt("Tile px", &tile_size, 1, 8, enter)) {
      set_tile_size(id, tile_size);
    }
    std::string rel;
    if (asset_combo("Tileset", workspace_.find(id)->tilemap->tileset,
                    "(built-in palette)", &rel)) {
      set_tileset(id, rel);
    }
    const Entity2D* now = workspace_.find(id);
    if (!now || !now->tilemap) {
      return;
    }
    if (!now->tilemap->tileset.empty() && !tileset_ready(*now)) {
      theme::StatusWarn("Tileset missing or smaller than one tile; drawing "
                        "the built-in palette.");
    }
    ImGui::TextDisabled("%d x %d cells | %.0f x %.0f px | %zu painted",
                        now->tilemap->cols, now->tilemap->rows, now->w, now->h,
                        now->tilemap->count_nonempty());
    if (theme::SecondaryButton("Paint (B)", ImVec2(90, 0))) {
      set_tool(TileTool::Paint);
    }
    ImGui::SameLine();
    if (theme::DangerButton("Clear Tiles", ImVec2(100, 0))) {
      fill_rect_cells(id, 0, 0, now->tilemap->cols - 1, now->tilemap->rows - 1,
                      true);
    }
    return;
  }

  ImGui::SeparatorText("Sprite");
  std::string rel;
  if (asset_combo("Image", e.sprite ? e.sprite->path : std::string(), "(none)",
                  &rel)) {
    assign_sprite(id, rel);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Pick from the Supply Wagon, or drag an image here.");
  }
  Entity2D* cur = workspace_.find(id);
  if (!cur || !cur->sprite) {
    ImGui::TextDisabled("No sprite. Draws as a tinted rect.");
    return;
  }
  const SpriteState st = sprite_state(*cur);
  if (st == SpriteState::Missing) {
    std::string msg = "Image missing: " + cur->sprite->path +
                      ". Drawing the tint rect instead.";
    theme::StatusWarn(msg.c_str());
  } else {
    const TextureInfo& t = texture(cur->sprite->path);
    ImGui::TextDisabled("%d x %d px source", t.width, t.height);
  }

  auto toggle = [&](const char* label, bool SpriteData::*field,
                    const char* step) {
    bool v = (*cur->sprite).*field;
    if (ImGui::Checkbox(label, &v)) {
      Workspace2D::Snapshot before = prepare_edit();
      Entity2D* target = workspace_.find(id);
      if (target && target->sprite) {
        (*target->sprite).*field = v;
        commit_discrete(std::move(before), step, "");
      }
      cur = workspace_.find(id);
    }
  };
  toggle("Flip X", &SpriteData::flip_x, "Flip Sprite");
  ImGui::SameLine();
  toggle("Flip Y", &SpriteData::flip_y, "Flip Sprite");
  ImGui::SameLine();
  toggle("Source rect", &SpriteData::use_src_rect, "Sprite Source Rect");
  if (cur && cur->sprite && cur->sprite->use_src_rect) {
    int v[4] = {cur->sprite->src_x, cur->sprite->src_y, cur->sprite->src_w,
                cur->sprite->src_h};
    if (ImGui::DragInt4("Src x/y/w/h", v, 0.25f, 0, 16384)) {
      cur->sprite->src_x = v[0];
      cur->sprite->src_y = v[1];
      cur->sprite->src_w = v[2];
      cur->sprite->src_h = v[3];
      mark_dirty();
    }
    track_inspector_item("Edit Source Rect");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Pixels in the image. w/h 0 = to the image edge.");
    }
  }
  if (cur && cur->sprite && st == SpriteState::Ready) {
    if (theme::SecondaryButton("Fit to image", ImVec2(100, 0))) {
      const TextureInfo& t = texture(cur->sprite->path);
      const SpriteData& sp = *cur->sprite;
      float w = static_cast<float>(t.width);
      float h = static_cast<float>(t.height);
      if (sp.use_src_rect) {
        w = static_cast<float>(sp.src_w > 0 ? sp.src_w : t.width - sp.src_x);
        h = static_cast<float>(sp.src_h > 0 ? sp.src_h : t.height - sp.src_y);
      }
      Workspace2D::Snapshot before = prepare_edit();
      if (Entity2D* target = workspace_.find(id)) {
        target->w = std::clamp(w, 1.0f, 4096.0f);
        target->h = std::clamp(h, 1.0f, 4096.0f);
        commit_discrete(std::move(before), "Fit Sprite", "Sized to the image");
      }
    }
    ImGui::SameLine();
  }
  if (cur && cur->sprite) {
    if (theme::SecondaryButton("White tint", ImVec2(90, 0))) {
      Workspace2D::Snapshot before = prepare_edit();
      if (Entity2D* target = workspace_.find(id)) {
        for (float& c : target->color) {
          c = 1.0f;
        }
        commit_discrete(std::move(before), "Edit Tint", "");
      }
    }
    ImGui::SameLine();
    if (theme::DangerButton("Remove", ImVec2(70, 0))) {
      assign_sprite(id, "");
    }
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
