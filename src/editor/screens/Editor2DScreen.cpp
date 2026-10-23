#include "editor/screens/Editor2DScreen.h"
#include "editor/plugins/PluginNeeds.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/workspace/SceneIO.h"
#include "scene/SampleRider.h"
#include "scene/SampleScripts.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

std::string count_label(const char* verb, std::size_t n) {
  return std::string(verb) + " " + std::to_string(n);
}

}  // namespace

Editor2DScreen::Editor2DScreen(ProjectInfo project)
    : project_(std::move(project)) {}

void Editor2DScreen::on_enter() {
  // A fresh visit never starts mid-ride.
  play_.stop();
  play_backup_.reset();
  play_free_cam_ = false;
  play_focus_viewport_ = false;
  quit_requested_ = false;
  back_requested_ = false;
  settings_requested_ = false;
  setup_field_guide();
  renaming_ = false;
  panning_ = false;
  drag_mode_ = DragMode::None;
  drag_hit_id_ = 0;
  drag_additive_ = false;
  drag_moved_ = false;
  viewport_focused_ = false;
  hierarchy_focused_ = false;
  nudge_pending_save_ = false;
  has_range_anchor_ = false;
  insp_drag_slot_ = -1;
  edit_source_ = EditSource::None;
  insp_frame_valid_ = false;
  status_note_.clear();
  dirty_ = false;
  scene_path_ = scene_io::scene_path_for_project(project_.path);
  // Scripts and the Telegraph start fresh for each project.
  script_lib_.clear();
  script_infos_.clear();
  scripts_selected_.clear();
  scripts_refresh_time_ = -1000.0;
  telegraph_.clear();
  telegraph_seen_total_ = 0;

  std::string err;
  bool seeded = false;
  std::vector<std::string> camera_log;
  pick_action2d_.clear();
  camera_backup2d_.clear();
  if (!project_.path.empty() && scene_io::load(workspace_, scene_path_, &err, &camera_log)) {
    std::cout << "[Editor2D] loaded scene.json (" << workspace_.entities().size()
              << " entities) from " << scene_path_ << '\n';
    // Pre-0.2.0 scenes: the implicit rider camera becomes a Camera entity
    // (backup + claim log first; docs/camera.md "Migration").
    migrate_cameras_on_open_2d(camera_log);
  } else {
    if (!err.empty()) {
      std::cout << "[Editor2D] no scene.json yet (" << err << ") -- seeding defaults\n";
    } else {
      std::cout << "[Editor2D] seeding default entities\n";
    }
    workspace_.reset_defaults();
    seeded = true;
    if (!project_.path.empty()) {
      // The seeded Player rides the sample cowboy sheet.
      std::string rider_err;
      if (!sample_rider::write_into_project(project_.path, &rider_err)) {
        std::cout << "[Editor2D] sample rider: " << rider_err << '\n';
      }
      // ...and the gate / gold nugget run the sample scripts.
      std::string scripts_err;
      if (!sample_scripts::write_into_project(project_.path, &scripts_err)) {
        std::cout << "[Editor2D] sample scripts: " << scripts_err << '\n';
      }
      std::string save_err;
      if (scene_io::save(workspace_, scene_path_, &save_err)) {
        std::cout << "[Editor2D] wrote initial scene.json to " << scene_path_
                  << '\n';
        dirty_ = false;
      } else {
        std::cout << "[Editor2D] failed to write initial scene.json: " << save_err
                  << '\n';
      }
    }
  }

  workspace_.clear_history();  // history is per open project, in memory only
  prefabs_.set_project(project_.path);
  prefab_sync_all_on_load();
  open_asset_browser();

  // Supply Wagon: make sure <project>/assets exists and list it.
  stroke_id_ = 0;
  tool_ = TileTool::Select;
  textures_.clear();
  if (!project_.path.empty()) {
    std::string asset_err;
    if (!assets::ensure_assets_dir(project_.path, &asset_err)) {
      std::cout << "[Editor2D] " << asset_err << '\n';
    }
  }
  refresh_assets();
  refresh_scripts();
  open_plugins();
  // 0.2.0 split: a freshly seeded scene switches on what its rider needs;
  // a loaded one is the baseline (Play switches on what gets added later).
  if (seeded && plugins_.is_open()) {
    if (!plugins_.follow_needs(plugin_needs(workspace_.entities())).empty()) {
      apply_plugin_changes_if_idle();
    }
  } else {
    plugins_.set_needs_baseline(plugin_needs(workspace_.entities()));
  }
  telegraph_.add(runtime::LogLevel::Info, "editor",
                 "Telegraph open for " + project_.name + " (" +
                     std::to_string(scripts_.size()) + " scripts on file)");
  std::cout << "[Editor2D] workspace for \"" << project_.name << "\" ("
            << to_string(project_.kind) << ") path=" << project_.path << '\n';
}

void Editor2DScreen::on_exit() {
  // Leaving mid-ride: drop the run and put the edit scene back first.
  if (is_playing()) {
    stop_play();
  }
  if (posse_running()) {
    stop_posse();
  }
  plugins_.close();
  cancel_rename();
  if (stroke_active()) {
    end_paint_stroke();
  }
  if (workspace_.move_active()) {
    workspace_.cancel_move();
    drag_mode_ = DragMode::None;
  }
  flush_pending_edit();
  if (dirty_ || !project_.path.empty()) {
    std::string err;
    if (!save_scene(&err) && !err.empty()) {
      std::cout << "[Editor2D] save on leave failed: " << err << '\n';
    }
  }
  workspace_.clear_history();
  edit_source_ = EditSource::None;
  textures_.clear();
  std::cout << "[Editor2D] leaving workspace\n";
}

void Editor2DScreen::mark_dirty() { dirty_ = true; }

void Editor2DScreen::mark_dirty_and_autosave() {
  dirty_ = true;
  std::string err;
  if (!save_scene(&err) && !err.empty()) {
    std::cout << "[Editor2D] autosave failed: " << err << '\n';
    telegraph_.add(runtime::LogLevel::Warn, "editor", "Autosave failed: " + err);
  }
}

bool Editor2DScreen::save_scene(std::string* error_out) {
  if (is_playing()) {
    // Play mode never writes scene.json; the edit scene is parked as-is.
    if (error_out) {
      *error_out = "Not while riding. Stop play (F5) to save.";
    }
    return false;
  }
  if (project_.path.empty()) {
    if (error_out) {
      *error_out = "Project path is empty; cannot save scene.json";
    }
    return false;
  }
  if (scene_path_.empty()) {
    scene_path_ = scene_io::scene_path_for_project(project_.path);
  }
  refresh_prefab_overrides();
  std::string err;
  if (!scene_io::save(workspace_, scene_path_, &err)) {
    if (error_out) {
      *error_out = err;
    }
    return false;
  }
  dirty_ = false;
  std::cout << "[Editor2D] saved scene.json (" << workspace_.entities().size()
            << " entities) -> " << scene_path_ << '\n';
  return true;
}

void Editor2DScreen::note(std::string message) {
  std::cout << "[Editor2D] " << message << '\n';
  // Every status note also goes down the wire to the Telegraph.
  telegraph_.add(runtime::LogLevel::Info, "editor", message);
  status_note_ = std::move(message);
  status_note_time_ =
      ImGui::GetCurrentContext() != nullptr ? ImGui::GetTime() : 0.0;
}

double Editor2DScreen::now_seconds() const {
  if (ImGui::GetCurrentContext() != nullptr) {
    return ImGui::GetTime();
  }
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

Workspace2D::Snapshot Editor2DScreen::prepare_edit() {
  flush_pending_edit();
  return workspace_.snapshot();
}

bool Editor2DScreen::flush_pending_edit() {
  const EditSource src = edit_source_;
  edit_source_ = EditSource::None;
  if (!workspace_.edit_open()) {
    return false;
  }
  const bool pushed = workspace_.commit_edit();
  if (pushed && src == EditSource::Nudge && nudge_pending_save_) {
    // History step is in; make sure scene.json is too.
    nudge_pending_save_ = false;
    mark_dirty_and_autosave();
  }
  return pushed;
}

std::uint64_t Editor2DScreen::create_entity(std::string name) {
  if (is_playing()) {
    return 0;  // edit tools are locked while playing
  }
  cancel_rename();
  Workspace2D::Snapshot before = prepare_edit();
  const std::uint64_t id = workspace_.create_entity(std::move(name));
  if (id == 0) {
    return 0;
  }
  workspace_.commit_step("Create Entity", std::move(before));
  mark_dirty_and_autosave();
  if (const Entity2D* e = workspace_.find(id)) {
    note("Staked " + e->name);
  }
  return id;
}

std::size_t Editor2DScreen::duplicate_selected() {
  if (is_playing()) {
    return 0;
  }
  cancel_rename();
  Workspace2D::Snapshot before = prepare_edit();
  const std::vector<std::uint64_t> ids = workspace_.duplicate_selection();
  if (ids.empty()) {
    return 0;
  }
  workspace_.commit_step(count_label("Duplicate", ids.size()),
                         std::move(before));
  mark_dirty_and_autosave();
  note("Duplicated " + std::to_string(ids.size()) +
       (ids.size() == 1 ? " entity" : " entities"));
  return ids.size();
}

std::size_t Editor2DScreen::delete_selected() {
  if (is_playing()) {
    return 0;
  }
  cancel_rename();
  if (workspace_.move_active()) {
    workspace_.cancel_move();
    drag_mode_ = DragMode::None;
  }
  Workspace2D::Snapshot before = prepare_edit();
  const std::size_t n = workspace_.delete_selection();
  if (n == 0) {
    return 0;
  }
  workspace_.commit_step(count_label("Delete", n), std::move(before));
  has_range_anchor_ = false;
  mark_dirty_and_autosave();
  note("Buried " + std::to_string(n) + (n == 1 ? " entity" : " entities"));
  return n;
}

bool Editor2DScreen::snap_selected_to_grid() {
  if (is_playing()) {
    return false;
  }
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.snap_selection_to_grid()) {
    return false;
  }
  workspace_.commit_step("Snap to Grid", std::move(before));
  mark_dirty_and_autosave();
  note("Snapped to grid");
  return true;
}

void Editor2DScreen::reset_scene_placeholders() {
  if (is_playing()) {
    return;
  }
  cancel_rename();
  Workspace2D::Snapshot before = prepare_edit();
  workspace_.reset_defaults();
  sample_rider::write_into_project(project_.path);
  sample_scripts::write_into_project(project_.path);
  workspace_.commit_step("Reset Scene", std::move(before));
  has_range_anchor_ = false;
  mark_dirty_and_autosave();
  note("Scene reset to placeholders");
}

bool Editor2DScreen::nudge_selected(int dir_x, int dir_y, bool large) {
  if (is_playing()) {
    return false;
  }
  const double now = now_seconds();
  // A different source, or a different set of entities, starts a new step.
  if (edit_source_ != EditSource::Nudge ||
      nudge_selection_ != workspace_.selection()) {
    flush_pending_edit();
  }
  if (!workspace_.edit_open()) {
    workspace_.begin_edit("Nudge");
    edit_source_ = EditSource::Nudge;
    nudge_selection_ = workspace_.selection();
  }
  last_nudge_time_ = now;
  if (!workspace_.nudge_selection(dir_x, dir_y, large)) {
    return false;
  }
  workspace_.set_edit_label(count_label("Nudge", workspace_.selection_count()));
  mark_dirty();
  nudge_pending_save_ = true;  // flushed when the arrow keys are released
  return true;
}

void Editor2DScreen::begin_drag_move() {
  if (is_playing()) {
    return;
  }
  flush_pending_edit();
  workspace_.begin_edit("Move");
  edit_source_ = EditSource::Drag;
  workspace_.begin_move();
}

bool Editor2DScreen::end_drag_move() {
  const std::size_t n = workspace_.selection_count();
  const bool moved = workspace_.end_move();
  const bool copied = dup_drag_;
  dup_drag_ = false;
  if (edit_source_ == EditSource::Drag) {
    workspace_.set_edit_label(count_label(copied ? "Duplicate" : "Move", n));
    flush_pending_edit();
  }
  if (copied) {
    // The copies exist whether or not they moved.
    mark_dirty_and_autosave();
    note("Duplicated " + std::to_string(n) + (n == 1 ? " entity" : " entities"));
    return true;
  }
  if (moved) {
    mark_dirty_and_autosave();
    note(std::string("Moved ") + std::to_string(n) +
         (n == 1 ? " entity" : " entities") +
         (workspace_.snap_enabled() ? " (snapped)" : ""));
  }
  return moved;
}

void Editor2DScreen::cancel_drag_move() {
  workspace_.cancel_move();
  if (edit_source_ == EditSource::Drag) {
    if (dup_drag_) {
      workspace_.revert_edit();  // take the copies back too
    } else {
      workspace_.cancel_edit();
    }
    edit_source_ = EditSource::None;
  }
  dup_drag_ = false;
}

void Editor2DScreen::begin_inspector_edit(const char* label) {
  if (is_playing()) {
    return;
  }
  if (edit_source_ == EditSource::Inspector && workspace_.edit_open()) {
    workspace_.set_edit_label(label);
    return;
  }
  flush_pending_edit();
  Workspace2D::Snapshot before = workspace_.snapshot();
  // Swap in the primary as it was at the top of the frame, in case the
  // widget already changed it on its activation frame.
  if (insp_frame_valid_) {
    for (Entity2D& e : before.entities) {
      if (e.id == insp_frame_entity_.id) {
        e = insp_frame_entity_;
        break;
      }
    }
  }
  workspace_.begin_edit(label, std::move(before));
  edit_source_ = EditSource::Inspector;
}

bool Editor2DScreen::end_inspector_edit() {
  bool pushed = false;
  if (edit_source_ == EditSource::Inspector) {
    pushed = flush_pending_edit();
  }
  if (pushed || dirty_) {
    mark_dirty_and_autosave();
  }
  return pushed;
}

void Editor2DScreen::track_inspector_item(const char* label) {
  if (ImGui::IsItemActivated()) {
    begin_inspector_edit(label);
  }
  if (ImGui::IsItemDeactivated()) {
    end_inspector_edit();
  }
}

bool Editor2DScreen::undo() {
  if (is_playing()) {
    note("Undo waits until the ride is over (F5 stops).");
    return false;
  }
  if (drag_mode_ != DragMode::None) {
    return false;  // mid-drag: finish or Esc first
  }
  cancel_rename();
  if (stroke_active()) {
    end_paint_stroke();
  }
  flush_pending_edit();
  if (workspace_.can_undo()) {
    prefabs_.on_undo(workspace_.undo_count(), workspace_.undo_label());
  }
  std::string label;
  if (!workspace_.undo(&label)) {
    note("Nothing to undo. Trail's clean.");
    return false;
  }
  has_range_anchor_ = false;
  insp_drag_slot_ = -1;
  nudge_pending_save_ = false;
  mark_dirty_and_autosave();
  note("Undid: " + label);
  return true;
}

bool Editor2DScreen::redo() {
  if (is_playing()) {
    return false;
  }
  if (drag_mode_ != DragMode::None) {
    return false;
  }
  cancel_rename();
  if (stroke_active()) {
    end_paint_stroke();
  }
  flush_pending_edit();
  std::string label;
  if (!workspace_.redo(&label)) {
    note("Nothing to redo. End of the trail.");
    return false;
  }
  prefabs_.on_redo(workspace_.undo_count(), label);
  has_range_anchor_ = false;
  insp_drag_slot_ = -1;
  nudge_pending_save_ = false;
  mark_dirty_and_autosave();
  note("Redid: " + label);
  return true;
}

void Editor2DScreen::begin_rename(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e) {
    return;
  }
  renaming_ = true;
  rename_id_ = id;
  std::memset(rename_buf_, 0, sizeof(rename_buf_));
  std::strncpy(rename_buf_, e->name.c_str(), sizeof(rename_buf_) - 1);
}

void Editor2DScreen::commit_rename() {
  if (!renaming_) {
    return;
  }
  std::string name = rename_buf_;
  // trim
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
    name.pop_back();
  }
  if (!name.empty()) {
    Workspace2D::Snapshot before = prepare_edit();
    if (workspace_.rename_entity(rename_id_, std::move(name))) {
      workspace_.commit_step("Rename", std::move(before));
      mark_dirty_and_autosave();
    }
  }
  renaming_ = false;
}

void Editor2DScreen::cancel_rename() {
  renaming_ = false;
}

AppState Editor2DScreen::on_update(float delta_seconds) {
  tick_team(delta_seconds);
  if (ImGui::GetCurrentContext() != nullptr) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
      std::string err;
      if (is_playing()) {
        note("Not while riding. Stop play (F5) to save.");
      } else if (!save_scene(&err) && !err.empty()) {
        std::cout << "[Editor2D] save failed: " << err << '\n';
      }
    }
    if (!is_playing() && ImGui::GetTime() - prefab_poll_time_ > 1.0) {
      prefab_poll_time_ = ImGui::GetTime();
      prefab_poll();
    }
    vault_panel_.update();
    // Hold the browser's rescan while the Vault worker writes into the claim.
    if (!is_playing() && !vault_panel_.busy()) asset_browser_.poll(ImGui::GetTime());
    draw_ui();
  }

  if (quit_requested_) {
    return AppState::Quit;
  }
  if (back_requested_) {
    return AppState::ProjectManager;
  }
  if (settings_requested_) {
    settings_requested_ = false;
    return AppState::Settings;  // AppFlow comes back to this claim afterwards
  }
  return AppState::Editor2D;
}

void Editor2DScreen::request_quit() {
  quit_requested_ = true;
}

void Editor2DScreen::request_back_to_projects() {
  back_requested_ = true;
  std::cout << "[Editor2D] back to ProjectManager\n";
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
