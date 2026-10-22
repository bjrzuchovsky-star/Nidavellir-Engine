#include "editor/screens/Editor2DScreen.h"
#include "editor/export/FreightOffice.h"
#include "editor/tools/AutoTilePanel.h"
#include "editor/tools/SpriteSlicerPanel.h"
#include "editor/tools/StringsPanel.h"
#include "editor/scripting/BrandBook.h"

#include "editor/audio/EditorAudio.h"
#include "editor/ui/Brand.h"
#include "editor/ui/Theme.h"
#include "plugins/Builtins.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

void Editor2DScreen::setup_default_dock_layout(unsigned int dockspace_id) {
  if (dock_layout_initialized_) {
    return;
  }
  // Only build once when the dockspace has no saved layout yet.
  if (ImGui::DockBuilderGetNode(dockspace_id) != nullptr) {
    dock_layout_initialized_ = true;
    return;
  }
  dock_layout_initialized_ = true;

  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

  ImGuiID dock_main = dockspace_id;
  ImGuiID dock_left = 0;
  ImGuiID dock_right = 0;
  ImGuiID dock_center = 0;

  ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.22f, &dock_left,
                              &dock_main);
  ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.30f, &dock_right,
                              &dock_center);

  // Left column: Hierarchy over Supply Wagon (Scripts tabbed beside it).
  // Right: Inspector over Tile Palette, with the Stable tabbed beside it.
  // The Telegraph runs along the bottom of the Viewport.
  ImGuiID dock_left_bottom = 0;
  ImGuiID dock_right_bottom = 0;
  ImGuiID dock_bottom = 0;
  ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.42f,
                              &dock_left_bottom, &dock_left);
  ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.50f,
                              &dock_right_bottom, &dock_right);
  ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Down, 0.26f, &dock_bottom,
                              &dock_center);

  ImGui::DockBuilderDockWindow("Hierarchy", dock_left);
  ImGui::DockBuilderDockWindow("Supply Wagon", dock_left_bottom);
  ImGui::DockBuilderDockWindow("Scripts", dock_left_bottom);
  ImGui::DockBuilderDockWindow("Prefabs", dock_left_bottom);
  ImGui::DockBuilderDockWindow("Plugins", dock_left_bottom);
  ImGui::DockBuilderDockWindow("Telegraph", dock_bottom);
  ImGui::DockBuilderDockWindow("Asset Browser", dock_bottom);
  ImGui::DockBuilderDockWindow("Vault", dock_bottom);
  ImGui::DockBuilderDockWindow("Viewport2D", dock_center);
  ImGui::DockBuilderDockWindow("Inspector", dock_right);
  ImGui::DockBuilderDockWindow("Tile Palette", dock_right_bottom);
  ImGui::DockBuilderDockWindow("Stable", dock_right_bottom);
  ImGui::DockBuilderDockWindow("Network", dock_right_bottom);
  ImGui::DockBuilderDockWindow("Saloon Band (Mixer)", dock_right_bottom);
  ImGui::DockBuilderFinish(dockspace_id);
}

namespace {

constexpr float kGridPresets[] = {8.0f, 16.0f, 32.0f, 64.0f, 128.0f};
// Arrow taps closer together than this share one "Nudge" undo step.
constexpr double kNudgeCoalesceSeconds = 0.6;

}  // namespace

void Editor2DScreen::draw_help_menu_contents() {
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted("TombStone Admin -- 2D field kit");
  ImGui::PopStyleColor();
  ImGui::Separator();
  theme::KeyHint("Ctrl+Z", "Undo");
  theme::KeyHint("Ctrl+Y", "Redo (also Ctrl+Shift+Z)");
  theme::KeyHint("LMB drag", "Move selection");
  theme::KeyHint("Ctrl+click", "Add / remove from selection");
  theme::KeyHint("Shift+click", "Add (Hierarchy: range)");
  theme::KeyHint("Drag empty", "Box select (Ctrl/Shift adds)");
  theme::KeyHint("Arrows", "Nudge 1px / 1 cell (snap)");
  theme::KeyHint("Shift+Arrows", "Nudge 10px / 4 cells (snap)");
  theme::KeyHint("Ctrl+D", "Duplicate selection");
  theme::KeyHint("Del", "Delete selection");
  theme::KeyHint("Ctrl+A / Esc", "Select all / clear (Esc cancels drag)");
  theme::KeyHint("F2", "Rename primary");
  theme::KeyHint("G / Shift+G", "Toggle grid / snap");
  theme::KeyHint("[ / ]", "Halve / double grid size");
  theme::KeyHint("Handles", "Drag edges/corners resize (Shift aspect), knob rotates");
  theme::KeyHint("Alt+drag", "Duplicate and drag the copy");
  theme::KeyHint("RMB click", "Context menu (viewport + hierarchy)");
  theme::KeyHint("Ctrl+C/X/V", "Copy / cut / paste (works across scenes)");
  theme::KeyHint("Ctrl+G / +Shift", "Group / ungroup");
  theme::KeyHint("Ctrl+L / +Shift", "Lock / unlock");
  theme::KeyHint("H / Shift+H", "Hide / show selection");
  theme::KeyHint("F", "Frame selection");
  theme::KeyHint("MMB/RMB-drag/Alt", "Pan viewport");
  theme::KeyHint("Wheel", "Zoom toward cursor");
  theme::KeyHint("Ctrl+S", "Save scene.json");
  ImGui::Separator();
  theme::KeyHint("V", "Select tool");
  theme::KeyHint("B / E", "Brush / Erase tiles (drag = 1 undo step)");
  theme::KeyHint("U", "Bucket fill");
  theme::KeyHint("R", "Rect fill (Shift: erase box)");
  theme::KeyHint("I / Alt+click", "Pick tile from the map");
  theme::KeyHint("1 / 2 / 3", "Brush size (tile tools)");
  theme::KeyHint("Drag asset", "Supply Wagon -> viewport: new sprite");
  theme::KeyHint("Stable", "Cut a sheet into clips (sheets with a .anim.json "
                           "drop in animated)");
  ImGui::Separator();
  theme::KeyHint("F5 / Ctrl+P", "Play / Stop (Stop restores the scene)");
  theme::KeyHint("F6 / F10", "Pause / step one tick while paused");
  theme::KeyHint("WASD / arrows", "Ride player 1 (gamepads: P1-P4)");
  theme::KeyHint("E / Space", "Action while riding (pad A): gates, signs");
  theme::KeyHint("C", "Free camera while playing");
}

void Editor2DScreen::draw_menu_bar() {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }

  // Brand mark in the menu strip.
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    theme::TombstoneMark(dl, ImVec2(p.x + 7.0f, p.y + 3.0f), h - 6.0f,
                         theme::U32(theme::Sand()),
                         theme::U32(theme::Charcoal(), 0.85f));
    ImGui::Dummy(ImVec2(16.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    ImGui::TextUnformatted(brand::kProduct);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(6.0f, 0.0f));
  }

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !is_playing())) {
      std::string err;
      if (!save_scene(&err) && !err.empty()) {
        std::cout << "[Editor2D] save failed: " << err << '\n';
      } else {
        note("Scene saved");
      }
    }
    if (ImGui::MenuItem("Launch Game (ts_game)", nullptr, false,
                        !is_playing())) {
      launch_game();
    }
    if (ImGui::MenuItem("Export... (Freight Office)", nullptr, false, !is_playing())) {
      FreightOffice::get().open(project_.path, project_.name);
    }
    if (ImGui::MenuItem("Brand Book... (visual scripts)")) {
      BrandBook::get().open(project_.path, project_.name);
    }
    if (ImGui::MenuItem("Strings... (localisation)")) {
      StringsPanel::get().open(project_.path, project_.name);
    }
    if (ImGui::MenuItem("Autotiles... (terrain brushes)")) {
      AutoTilePanel::get().open(project_.path, project_.name);
    }
    if (ImGui::MenuItem("Sprite Slicer...")) {
      SpriteSlicerPanel::get().open(project_.path);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Back to Projects")) {
      request_back_to_projects();
    }
    if (ImGui::MenuItem("Quit", "Alt+F4")) {
      request_quit();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit", !is_playing())) {
    const bool has_sel = workspace_.selection_count() > 0;
    {
      // Labels name the step ("Undo Move 3"); ### keeps the IDs stable.
      const bool can_undo = workspace_.can_undo() || workspace_.edit_changed();
      const bool can_redo = workspace_.can_redo();
      const std::string& undo_name = workspace_.edit_changed()
                                         ? workspace_.edit_label()
                                         : workspace_.undo_label();
      const std::string undo_text =
          (can_undo ? "Undo " + undo_name : std::string("Undo")) +
          "###EditUndo";
      const std::string redo_text =
          (can_redo ? "Redo " + workspace_.redo_label() : std::string("Redo")) +
          "###EditRedo";
      if (ImGui::MenuItem(undo_text.c_str(), "Ctrl+Z", false, can_undo)) {
        undo();
      }
      if (ImGui::MenuItem(redo_text.c_str(), "Ctrl+Y", false, can_redo)) {
        redo();
      }
      ImGui::Separator();
    }
    if (ImGui::MenuItem("Create Entity")) {
      create_entity("Entity");
    }
    if (ImGui::MenuItem("Create TileMap")) {
      create_tilemap();
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, has_sel)) {
      duplicate_selected();
    }
    if (ImGui::MenuItem("Delete Selected", "Del", false, has_sel)) {
      delete_selected();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_sel)) copy_selected();
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, has_sel)) cut_selected();
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboard_text().empty())) paste();
    if (ImGui::MenuItem("Group", "Ctrl+G", false, has_sel)) group_selected();
    if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, has_sel)) ungroup_selected();
    if (ImGui::MenuItem("Lock Selection", "Ctrl+L", false, has_sel)) lock_selected(true);
    if (ImGui::MenuItem("Unlock Selection", "Ctrl+Shift+L", false, has_sel)) lock_selected(false);
    if (ImGui::MenuItem("Hide Selection", "H", false, has_sel)) hide_selected(true);
    if (ImGui::MenuItem("Show Selection", "Shift+H", false, has_sel)) hide_selected(false);
    if (ImGui::MenuItem("Frame Selection", "F", false, has_sel)) frame_selected();
    const std::size_t nsel = workspace_.selection_count();
    if (ImGui::BeginMenu("Align / Distribute", nsel >= 2)) {
      if (ImGui::MenuItem("Align left")) align_selected(Workspace2D::Align::Left);
      if (ImGui::MenuItem("Align centre (H)")) align_selected(Workspace2D::Align::CenterX);
      if (ImGui::MenuItem("Align right")) align_selected(Workspace2D::Align::Right);
      if (ImGui::MenuItem("Align top")) align_selected(Workspace2D::Align::Top);
      if (ImGui::MenuItem("Align middle (V)")) align_selected(Workspace2D::Align::CenterY);
      if (ImGui::MenuItem("Align bottom")) align_selected(Workspace2D::Align::Bottom);
      ImGui::Separator();
      if (ImGui::MenuItem("Distribute horizontally", nullptr, false, nsel >= 3)) distribute_selected(true);
      if (ImGui::MenuItem("Distribute vertically", nullptr, false, nsel >= 3)) distribute_selected(false);
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Select All", "Ctrl+A")) {
      workspace_.select_all();
    }
    if (ImGui::MenuItem("Clear Selection", "Esc", false, has_sel)) {
      workspace_.clear_selection();
    }
    if (ImGui::MenuItem("Snap Selection to Grid", nullptr, false, has_sel)) {
      snap_selected_to_grid();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Scene Placeholders")) {
      reset_scene_placeholders();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Hierarchy", nullptr, &show_hierarchy_);
    ImGui::MenuItem("Viewport", nullptr, &show_viewport_);
    ImGui::MenuItem("Inspector", nullptr, &show_inspector_);
    ImGui::MenuItem("Tile Palette", nullptr, &show_tile_palette_);
    ImGui::MenuItem("Supply Wagon", nullptr, &show_supply_wagon_);
    ImGui::MenuItem("Prefabs", nullptr, &show_prefabs_);
    ImGui::MenuItem("Asset Browser", nullptr, &show_asset_browser_);
    ImGui::MenuItem("Vault", nullptr, &show_vault_);
    ImGui::MenuItem("Stable (Animation)", nullptr, &show_stable_);
    ImGui::MenuItem("UI Preview", nullptr, &show_ui_preview_);
    ImGui::MenuItem("Lighting", nullptr, &show_lighting_);
    ImGui::MenuItem("Scripts", nullptr, &show_scripts_);
    ImGui::MenuItem("Telegraph (Console)", nullptr, &show_console_);
    ImGui::MenuItem("Saloon Band (Mixer)", nullptr, &show_mixer_);
    ImGui::MenuItem("Plugins (Outfitter)", nullptr, &show_plugins_);
    draw_plugin_view_items();
    ImGui::MenuItem("Toolbar", nullptr, &show_toolbar_);
    ImGui::MenuItem("Status Bar", nullptr, &show_status_bar_);
    ImGui::Separator();
    ImGui::MenuItem("Preview Animations", nullptr, &anim_preview_);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Edit mode: animators run their default clip in the "
                        "viewport. Off = first frame.");
    }
    // Grid / snap / camera are saved with the scene: edit mode only.
    ImGui::BeginDisabled(is_playing());
    const bool grid = workspace_.show_grid();
    if (ImGui::MenuItem("Show Grid", "G", grid)) {
      workspace_.set_show_grid(!grid);
      mark_dirty_and_autosave();
    }
    const bool snap = workspace_.snap_enabled();
    if (ImGui::MenuItem("Snap to Grid", "Shift+G", snap)) {
      workspace_.set_snap_enabled(!snap);
      mark_dirty_and_autosave();
    }
    if (ImGui::MenuItem("Snap to Objects", nullptr, workspace_.object_snap())) {
      workspace_.set_object_snap(!workspace_.object_snap());
      note(workspace_.object_snap() ? "Object snap on (edges + centres)"
                                    : "Object snap off");
    }
    if (ImGui::BeginMenu("Grid Size")) {
      for (float g : kGridPresets) {
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f px", g);
        if (ImGui::MenuItem(label, nullptr, workspace_.grid_size() == g)) {
          workspace_.set_grid_size(g);
          mark_dirty_and_autosave();
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset Camera")) {
      workspace_.set_pan(0.0f, 0.0f);
      workspace_.set_zoom(1.0f);
    }
    ImGui::EndDisabled();
    ImGui::EndMenu();
  }

  draw_play_menu();
  draw_plugin_top_menus();

  if (ImGui::BeginMenu("Help")) {
    draw_help_menu_contents();
    ImGui::EndMenu();
  }

  ImGui::TextDisabled("|");
  ImGui::TextUnformatted(project_.name.c_str());
  ImGui::TextDisabled("(%s)", to_string(project_.kind));
  if (dirty_) {
    ImGui::TextColored(theme::Copper(), "*");
  }
  if (is_playing()) {
    ImGui::TextColored(is_paused() ? theme::Warning() : theme::Copper(),
                       is_paused() ? "  [PAUSED]" : "  [RIDING]");
  }
  ImGui::EndMainMenuBar();
}

void Editor2DScreen::draw_toolbar() {
  if (!show_toolbar_) {
    return;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float bar_h = theme::metrics::kToolbarHeight;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::HeaderBg());
  ImGui::Begin("##EditorToolbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav);
  {
    // Copper underline separating the tool strip from the dock area.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    dl->AddRectFilled(ImVec2(p.x, p.y + sz.y - 1.0f),
                      ImVec2(p.x + sz.x, p.y + sz.y),
                      theme::U32(theme::Copper(), 0.55f));
  }

  draw_play_controls();
  ImGui::SameLine(0.0f, 18.0f);
  // Edit tools are locked while playing.
  ImGui::BeginDisabled(is_playing());

  {
    const bool can_undo = workspace_.can_undo() || workspace_.edit_changed();
    const bool can_redo = workspace_.can_redo();
    ImGui::BeginDisabled(!can_undo);
    if (theme::SecondaryButton("Undo", ImVec2(56, 0))) {
      undo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      const std::string& l = workspace_.edit_changed()
                                 ? workspace_.edit_label()
                                 : workspace_.undo_label();
      if (can_undo) {
        ImGui::SetTooltip("Undo %s (Ctrl+Z)", l.c_str());
      } else {
        ImGui::SetTooltip("Nothing to undo (Ctrl+Z)");
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo);
    if (theme::SecondaryButton("Redo", ImVec2(56, 0))) {
      redo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (can_redo) {
        ImGui::SetTooltip("Redo %s (Ctrl+Y / Ctrl+Shift+Z)",
                          workspace_.redo_label().c_str());
      } else {
        ImGui::SetTooltip("Nothing to redo (Ctrl+Y / Ctrl+Shift+Z)");
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 18.0f);
  }

  draw_tool_buttons();
  ImGui::SameLine(0.0f, 18.0f);

  bool grid = workspace_.show_grid();
  if (theme::ToggleButton("Grid", &grid, ImVec2(56, 0))) {
    workspace_.set_show_grid(grid);
    mark_dirty_and_autosave();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Show grid (G)");
  }
  ImGui::SameLine();
  bool snap = workspace_.snap_enabled();
  if (theme::ToggleButton("Snap", &snap, ImVec2(56, 0))) {
    workspace_.set_snap_enabled(snap);
    mark_dirty_and_autosave();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Snap drags, nudges and Inspector X/Y/W/H to the grid (Shift+G)");
  }
  ImGui::SameLine();
  int grid_px = static_cast<int>(workspace_.grid_size() + 0.5f);
  ImGui::SetNextItemWidth(86.0f);
  if (ImGui::DragInt("##gridsize", &grid_px, 0.25f,
                     static_cast<int>(Workspace2D::kMinGrid),
                     static_cast<int>(Workspace2D::kMaxGrid), "cell %d px",
                     ImGuiSliderFlags_AlwaysClamp)) {
    workspace_.set_grid_size(static_cast<float>(grid_px));
    mark_dirty();
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    mark_dirty_and_autosave();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Grid cell size. Drag or Ctrl+click to type. [ / ] halves / doubles.");
  }

  ImGui::SameLine(0.0f, 18.0f);
  const bool has_sel = workspace_.selection_count() > 0;
  ImGui::BeginDisabled(!has_sel);
  if (theme::CopperButton("Duplicate", ImVec2(84, 0))) {
    duplicate_selected();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Ctrl+D");
  }
  ImGui::SameLine();
  if (theme::DangerButton("Delete", ImVec2(70, 0))) {
    delete_selected();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Del");
  }
  ImGui::EndDisabled();

  ImGui::SameLine(0.0f, 18.0f);
  if (theme::SecondaryButton("Reset Cam", ImVec2(84, 0))) {
    workspace_.set_pan(0.0f, 0.0f);
    workspace_.set_zoom(1.0f);
  }
  ImGui::SameLine();
  if (theme::PrimaryButton("Save", ImVec2(64, 0))) {
    std::string err;
    if (save_scene(&err)) {
      note("Scene saved");
    }
  }
  ImGui::SameLine(0.0f, 18.0f);
  ImGui::TextDisabled("zoom %.0f%%", workspace_.zoom() * 100.0f);
  ImGui::EndDisabled();  // is_playing()
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void Editor2DScreen::draw_status_bar() {
  if (!show_status_bar_) {
    return;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float bar_h = theme::metrics::kStatusBarHeight;
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - bar_h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::HeaderBg());
  ImGui::Begin("##EditorStatusBar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  {
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowSize().x;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 1.5f), theme::U32(theme::Accent()));
  }
  // Brand block: headstone + wordmark.
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    theme::TombstoneMark(dl, ImVec2(p.x + 6.0f, p.y), h, theme::U32(theme::Sand()),
                         theme::U32(theme::Charcoal(), 0.85f));
    ImGui::Dummy(ImVec2(14.0f, h));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    ImGui::TextUnformatted(brand::kProduct);
    ImGui::PopStyleColor();
  }

  auto sep = []() {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
  };

  const ImGuiIO& io = ImGui::GetIO();
  const float fps = io.Framerate;
  const float ms = (fps > 0.01f) ? (1000.0f / fps) : 0.0f;
  sep();
  ImGui::Text("%.0f fps (%.1f ms)", fps, ms);
  sep();
  ImGui::Text("zoom %.0f%%", workspace_.zoom() * 100.0f);
  sep();
  if (workspace_.snap_enabled()) {
    ImGui::TextColored(theme::Accent(), "snap %.0f px", workspace_.grid_size());
  } else {
    ImGui::TextDisabled("grid %.0f px", workspace_.grid_size());
  }
  if (is_playing()) {
    draw_play_status();
  } else {
    sep();
    if (tool_ != TileTool::Select) {
      if (tool_ == TileTool::Paint || tool_ == TileTool::Erase) {
        ImGui::TextColored(theme::Accent(), "%s %dx%d", to_string(tool_),
                           brush_size_, brush_size_);
      } else {
        ImGui::TextColored(theme::Accent(), "%s", to_string(tool_));
      }
      if (hover_cell_valid_) {
        ImGui::SameLine();
        ImGui::TextDisabled("cell %d,%d tile %d", hover_col_, hover_row_,
                            hover_tile_);
      }
    } else {
      ImGui::TextDisabled("Select");
    }
    sep();
    const std::size_t count = workspace_.selection_count();
    if (const Entity2D* e = workspace_.selected()) {
      if (count > 1) {
        ImGui::Text("%zu selected (%s)", count, e->name.c_str());
      } else {
        ImGui::Text("%s @ %.0f, %.0f", e->name.c_str(), e->x, e->y);
      }
    } else {
      ImGui::TextDisabled("no selection");
    }
  }
  sep();
  draw_telegraph_badge();
  const double now = ImGui::GetTime();
  if (!status_note_.empty() && now - status_note_time_ < 3.0) {
    sep();
    ImGui::TextColored(theme::Copper(), "%s", status_note_.c_str());
  }
  sep();
  ImGui::TextDisabled("%s", project_.path.c_str());
  if (dirty_) {
    ImGui::SameLine();
    ImGui::TextColored(theme::Warning(), "  unsaved");
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void Editor2DScreen::handle_hotkeys() {
  const ImGuiIO& io = ImGui::GetIO();
  // A text field (Inspector Name, rename box, Ctrl+clicked drag field) keeps
  // its own Ctrl+Z / Ctrl+Y and every other key.
  if (io.WantTextInput || renaming_) {
    return;
  }
  const bool ctrl = io.KeyCtrl || io.KeySuper;
  const bool shift = io.KeyShift;
  const bool alt = io.KeyAlt;
  const bool scene_focus = viewport_focused_ || hierarchy_focused_;

  // Play mode keys. Esc deliberately does nothing here: only F5 / Ctrl+P /
  // the Stop button end a ride.
  if (drag_mode_ == DragMode::None || is_playing()) {
    if ((ImGui::IsKeyPressed(ImGuiKey_F5, false) && !ctrl && !alt) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) {
      toggle_play();
      return;
    }
  }
  if (is_playing()) {
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
      toggle_pause_play();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F10, true)) {
      step_play();
    }
    if (!ctrl && !alt && !shift && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
      set_play_free_camera(!play_free_cam_);
      note(play_free_cam_ ? "Free camera. C hands it back to the follow cam."
                          : "Follow camera.");
    }
    return;  // every edit hotkey is locked while playing
  }

  // Undo / redo (with key repeat). Not mid-drag or while a widget is held.
  if (drag_mode_ == DragMode::None && !ImGui::IsAnyItemActive()) {
    const ImGuiKeyChord mods = io.KeyMods;
    const bool z = ImGui::IsKeyPressed(ImGuiKey_Z, true);
    const bool y = ImGui::IsKeyPressed(ImGuiKey_Y, true);
    if (z && mods == ImGuiMod_Ctrl) {
      undo();
    } else if ((y && mods == ImGuiMod_Ctrl) ||
               (z && mods == (ImGuiMod_Ctrl | ImGuiMod_Shift))) {
      redo();
    }
  }

  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
    duplicate_selected();
  }
  if (drag_mode_ == DragMode::None && !is_playing()) {
    if (scene_focus && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copy_selected();
    if (scene_focus && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) cut_selected();
    if (scene_focus && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) group_selected();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G)) ungroup_selected();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_L)) lock_selected(true);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L)) lock_selected(false);
    if (scene_focus && !ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_H, false)) {
      hide_selected(!shift);
    }
    if (scene_focus && !ctrl && !alt && !shift && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
      frame_selected();
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
      workspace_.selection_count() > 0) {
    delete_selected();
  }
  if (scene_focus && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
    workspace_.select_all();
  }
  if (scene_focus && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    workspace_.clear_selection();
  }
  if (scene_focus && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
    if (const Entity2D* e = workspace_.selected()) {
      show_hierarchy_ = true;
      begin_rename(e->id);
    }
  }

  // Tool hotkeys (no modifiers, not mid-drag).
  if (!ctrl && !alt && !shift && drag_mode_ == DragMode::None) {
    struct ToolKey {
      ImGuiKey key;
      TileTool tool;
    };
    static constexpr ToolKey kToolKeys[] = {
        {ImGuiKey_V, TileTool::Select}, {ImGuiKey_B, TileTool::Paint},
        {ImGuiKey_E, TileTool::Erase},  {ImGuiKey_U, TileTool::Fill},
        {ImGuiKey_R, TileTool::Rect},   {ImGuiKey_I, TileTool::Eyedropper},
    };
    for (const ToolKey& k : kToolKeys) {
      if (ImGui::IsKeyPressed(k.key, false) && tool_ != k.tool) {
        set_tool(k.tool);
      }
    }
    if (tool_ != TileTool::Select) {
      const ImGuiKey size_keys[3] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3};
      for (int n = 0; n < 3; ++n) {
        if (ImGui::IsKeyPressed(size_keys[n], false)) {
          set_brush_size(n + 1);
          note("Brush " + std::to_string(n + 1) + "x" + std::to_string(n + 1));
        }
      }
    }
  }

  if (!ctrl && !alt) {
    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
      if (shift) {
        workspace_.set_snap_enabled(!workspace_.snap_enabled());
        note(workspace_.snap_enabled() ? "Snap on" : "Snap off");
      } else {
        workspace_.set_show_grid(!workspace_.show_grid());
      }
      mark_dirty_and_autosave();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) ||
        ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
      const bool up = ImGui::IsKeyPressed(ImGuiKey_RightBracket, false);
      workspace_.set_grid_size(workspace_.grid_size() * (up ? 2.0f : 0.5f));
      mark_dirty_and_autosave();
      note("Grid " + std::to_string(static_cast<int>(workspace_.grid_size())) +
           " px");
    }
  }

  // Arrow nudge (with key repeat). Only while the scene panels have focus so
  // Settings-style widgets elsewhere keep normal arrow behaviour.
  if (scene_focus && !ctrl && !alt && workspace_.selection_count() > 0 &&
      drag_mode_ == DragMode::None) {
    int dx = 0;
    int dy = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) dx -= 1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) dx += 1;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) dy -= 1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) dy += 1;
    if (dx != 0 || dy != 0) {
      nudge_selected(dx, dy, shift);
    }
  }
  const bool arrows_held = ImGui::IsKeyDown(ImGuiKey_LeftArrow) ||
                           ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
                           ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
                           ImGui::IsKeyDown(ImGuiKey_DownArrow);
  if (nudge_pending_save_ && !arrows_held) {
    nudge_pending_save_ = false;
    mark_dirty_and_autosave();
  }
  // Held arrows (key repeat) and quick taps fold into one "Nudge" step; it
  // commits once the keys are up and the coalesce window has passed.
  if (edit_source_ == EditSource::Nudge && !arrows_held &&
      now_seconds() - last_nudge_time_ > kNudgeCoalesceSeconds) {
    flush_pending_edit();
  }
}

void Editor2DScreen::draw_ui() {
  // Simulation first so this frame draws the newest ticks.
  if (is_playing()) {
    if (!ImGui::GetIO().WantTextInput) {
      for (int k = 0; k < 10; ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + k), false)) {
          plugins::select_hotbar(play_.world(), 0, k == 0 ? 9 : k - 1);
        }
      }
    }
    update_play(ImGui::GetIO().DeltaTime, poll_play_input());
  }
  if (posse_running()) {
    update_posse(ImGui::GetIO().DeltaTime, poll_play_input().slot(0));
  }
  draw_menu_bar();
  draw_toolbar();
  draw_status_bar();

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float top_pad = show_toolbar_ ? theme::metrics::kToolbarHeight : 0.0f;
  const float bottom_pad =
      show_status_bar_ ? theme::metrics::kStatusBarHeight : 0.0f;

  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + top_pad));
  ImGui::SetNextWindowSize(
      ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - top_pad - bottom_pad));
  ImGui::SetNextWindowViewport(viewport->ID);

  ImGuiWindowFlags host_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_NoDocking;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##EditorDockHost", nullptr, host_flags);
  ImGui::PopStyleVar(2);

  const ImGuiID dockspace_id = ImGui::GetID("Editor2DDockspace");
  setup_default_dock_layout(dockspace_id);
  ImGui::DockSpace(dockspace_id, ImVec2(0, 0),
                   ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::End();

  viewport_focused_ = false;
  hierarchy_focused_ = false;
  // NoNavInputs on the scene panels: arrow keys nudge entities instead of
  // moving the keyboard-nav cursor.
  // While playing the scene panels stay visible but locked; the Viewport
  // shows the running world.
  const bool locked = is_playing();
  if (show_hierarchy_) {
    if (ImGui::Begin("Hierarchy", &show_hierarchy_,
                     ImGuiWindowFlags_NoNavInputs)) {
      ImGui::BeginDisabled(locked);
      draw_hierarchy();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (show_viewport_ || play_focus_viewport_) {
    show_viewport_ = true;
    if (play_focus_viewport_) {
      ImGui::SetNextWindowFocus();  // keys go to the ride, not a panel
      play_focus_viewport_ = false;
    }
    if (ImGui::Begin("Viewport2D", &show_viewport_,
                     ImGuiWindowFlags_NoNavInputs |
                         ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse)) {
      if (locked) {
        draw_play_viewport();
      } else if (posse_running()) {
        draw_posse_viewport();
      } else {
        draw_viewport();
      }
    }
    ImGui::End();
  }
  if (show_inspector_) {
    if (ImGui::Begin("Inspector", &show_inspector_)) {
      ImGui::BeginDisabled(locked);
      if (const Entity2D* pe = workspace_.selected(); pe && pe->prefab) {
        draw_prefab_inspector(pe->id);
      }
      draw_inspector();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (show_tile_palette_) {
    ImGui::SetNextWindowSize(ImVec2(300, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Tile Palette", &show_tile_palette_)) {
      ImGui::BeginDisabled(locked);
      draw_tile_palette();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (show_supply_wagon_) {
    ImGui::SetNextWindowSize(ImVec2(300, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Supply Wagon", &show_supply_wagon_)) {
      ImGui::BeginDisabled(locked);
      draw_supply_wagon();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (show_asset_browser_) {
    ImGui::SetNextWindowSize(ImVec2(720, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Asset Browser", &show_asset_browser_)) {
      ImGui::BeginDisabled(locked);
      asset_browser_.draw();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (show_vault_) {
    ImGui::SetNextWindowSize(ImVec2(720, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Vault", &show_vault_)) {
      ImGui::BeginDisabled(locked);
      vault_panel_.draw();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (show_prefabs_) {
    ImGui::SetNextWindowSize(ImVec2(300, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Prefabs", &show_prefabs_)) {
      ImGui::BeginDisabled(locked);
      draw_prefabs_panel();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  ui_preview_.draw(&show_ui_preview_, project_.path);
  draw_lighting_panel();
  FreightOffice::get().draw();
  BrandBook::get().draw();
  StringsPanel::get().draw();
  AutoTilePanel::get().draw();
  SpriteSlicerPanel::get().draw();
  if (AutoTilePanel::get().take_retile_request()) {
    const AutoTileSet* t = AutoTilePanel::get().brush(project_.path);
    const std::uint64_t id = paint_target();
    if (t && id) {
      flush_pending_edit();
      workspace_.begin_edit("Re-tile");
      const std::size_t n = workspace_.retile_tilemap(id, *t);
      if (workspace_.commit_edit()) mark_dirty_and_autosave();
      note(("Re-tiled " + std::to_string(n) + " cells").c_str());
    } else {
      note("Select a TileMap to re-tile");
    }
  }
  if (show_stable_) {
    ImGui::SetNextWindowSize(ImVec2(320, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Stable", &show_stable_)) {
      ImGui::BeginDisabled(locked);
      draw_stable();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (show_scripts_) {
    // Stays live while riding: open a script, save, watch it hot-reload.
    ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Scripts", &show_scripts_)) {
      draw_scripts_panel();
    }
    ImGui::End();
  }
  if (show_console_) {
    ImGui::SetNextWindowSize(ImVec2(640, 220), ImGuiCond_FirstUseEver);
    if (focus_console_) {
      ImGui::SetNextWindowFocus();
      focus_console_ = false;
    }
    if (ImGui::Begin("Telegraph", &show_console_)) {
      draw_console();
    }
    ImGui::End();
  }
  if (show_mixer_) {
    // Stays live while riding: set the levels with the band playing.
    ImGui::SetNextWindowSize(ImVec2(470, 470), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Saloon Band (Mixer)", &show_mixer_)) {
      draw_mixer_panel();
    }
    ImGui::End();
  }
  draw_plugins_window();
  draw_plugin_panels();
  if (import_browser_.draw("Import Image##SupplyWagon")) {
    const std::string picked = import_browser_.take_result();
    if (!picked.empty()) {
      import_asset(picked);
    }
  }
  // Reload images edited on disk (throttled to once a second).
  textures_.poll_changes(now_seconds());

  handle_hotkeys();

  // Safety net: an Inspector edit whose widget vanished (selection change,
  // panel closed) without a deactivation still lands as one step.
  if (edit_source_ == EditSource::Inspector && !ImGui::IsAnyItemActive()) {
    end_inspector_edit();
  }
}


}  // namespace editor
}  // namespace tombstone
}  // namespace ts
