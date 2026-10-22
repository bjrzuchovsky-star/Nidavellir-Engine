// Editor tools on top of Workspace2D_Tools.cpp: clipboard, group / lock /
// hide, hierarchy drops, align / distribute, framing, handle drags,
// Alt+drag copies and the shared right-click menu. Every edit is one undo
// step and autosaves scene.json; all locked while playing.
#include "editor/screens/Editor2DScreen.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

std::string& clipboard_store() {
  static std::string text;
  return text;
}

std::string plural(std::size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

const std::string& Editor2DScreen::clipboard_text() {
  // The system clipboard wins when it holds a scene clip (another project
  // or editor window copied last).
  if (ImGui::GetCurrentContext() != nullptr) {
    if (const char* sys = ImGui::GetClipboardText()) {
      const std::string t(sys);
      if (t.rfind("{", 0) == 0 && t.find("\"entities\"") != std::string::npos) {
        clipboard_store() = t;
      }
    }
  }
  return clipboard_store();
}

bool Editor2DScreen::copy_selected() {
  const std::string text = workspace_.copy_selection_json();
  if (text.empty()) return false;
  clipboard_store() = text;
  if (ImGui::GetCurrentContext() != nullptr) ImGui::SetClipboardText(text.c_str());
  note("Copied " + plural(workspace_.selection_roots().size(), "entity", "entities"));
  return true;
}

std::size_t Editor2DScreen::cut_selected() {
  if (is_playing() || !copy_selected()) return 0;
  return delete_selected();
}

std::size_t Editor2DScreen::paste() {
  if (is_playing()) return 0;
  cancel_rename();
  Workspace2D::Snapshot before = prepare_edit();
  const std::vector<std::uint64_t> ids = workspace_.paste_json(clipboard_text(), true);
  if (ids.empty()) {
    note("Nothing on the clipboard to paste");
    return 0;
  }
  commit_discrete(std::move(before), "Paste " + std::to_string(ids.size()),
                  "Pasted " + plural(ids.size(), "entity", "entities"));
  return ids.size();
}

std::uint64_t Editor2DScreen::group_selected() {
  if (is_playing()) return 0;
  Workspace2D::Snapshot before = prepare_edit();
  const std::uint64_t id = workspace_.group_selection("Group");
  if (id == 0) return 0;
  commit_discrete(std::move(before), "Group", "Grouped into " + workspace_.find(id)->name);
  return id;
}

std::size_t Editor2DScreen::ungroup_selected() {
  if (is_playing()) return 0;
  Workspace2D::Snapshot before = prepare_edit();
  const std::size_t n = workspace_.ungroup_selection();
  if (n == 0) return 0;
  commit_discrete(std::move(before), "Ungroup", "Ungrouped " + plural(n, "entity", "entities"));
  return n;
}

bool Editor2DScreen::set_entity_locked(std::uint64_t id, bool locked) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.set_locked(id, locked)) return false;
  return commit_discrete(std::move(before), locked ? "Lock" : "Unlock", "");
}

bool Editor2DScreen::set_entity_hidden(std::uint64_t id, bool hidden) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.set_hidden(id, hidden)) return false;
  return commit_discrete(std::move(before), hidden ? "Hide" : "Show", "");
}

bool Editor2DScreen::lock_selected(bool locked) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.set_selection_locked(locked)) return false;
  return commit_discrete(std::move(before), locked ? "Lock" : "Unlock",
                         locked ? "Locked (no picking or dragging)" : "Unlocked");
}

bool Editor2DScreen::hide_selected(bool hidden) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.set_selection_hidden(hidden)) return false;
  return commit_discrete(std::move(before), hidden ? "Hide" : "Show",
                         hidden ? "Hidden in the viewport (still rides in Play)" : "Shown");
}

bool Editor2DScreen::reparent(std::uint64_t id, std::uint64_t parent) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.set_parent(id, parent)) return false;
  const Entity2D* p = workspace_.find(parent);
  return commit_discrete(std::move(before), "Reparent",
                         p ? "Now under " + p->name : std::string("Back to the scene root"));
}

bool Editor2DScreen::hierarchy_drop(std::uint64_t id, std::uint64_t target,
                                    Workspace2D::HierDrop where) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.hierarchy_drop(id, target, where)) return false;
  return commit_discrete(std::move(before),
                         where == Workspace2D::HierDrop::Inside ? "Reparent" : "Reorder", "");
}

bool Editor2DScreen::align_selected(Workspace2D::Align how) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.align_selection(how)) return false;
  return commit_discrete(std::move(before), "Align", "Lined up");
}

bool Editor2DScreen::distribute_selected(bool horizontal) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.distribute_selection(horizontal)) return false;
  return commit_discrete(std::move(before), "Distribute", "Spaced out evenly");
}

bool Editor2DScreen::frame_selected() {
  if (!workspace_.frame_selection(viewport_w_, viewport_h_)) return false;
  mark_dirty_and_autosave();  // the view is saved with the scene
  return true;
}

bool Editor2DScreen::add_component(std::uint64_t id, const std::string& kind) {
  if (is_playing()) return false;
  const Entity2D* e = workspace_.find(id);
  if (!e) return false;
  Workspace2D::Snapshot before = prepare_edit();
  Entity2D* t = workspace_.find(id);
  if (kind == "Collider" && !t->collider) {
    t->collider = default_collider(*t);
  } else if (kind == "Player" && !t->player) {
    t->player = PlayerControllerData{};
  } else if (kind == "Camera" && !t->camera) {
    Camera2DData c;
    for (const Entity2D& other : workspace_.entities()) {
      if (other.player && other.id != id) {
        c.target = other.id;
        break;
      }
    }
    t->camera = c;
  } else if (kind == "Spawn" && !t->spawn) {
    t->spawn = SpawnPointData{};
  } else {
    return false;
  }
  return commit_discrete(std::move(before), "Add " + kind, kind + " added to " + t->name);
}

// --- Handle drags ------------------------------------------------------------

bool Editor2DScreen::begin_handle_drag(int hx, int hy, bool rotate, float wx, float wy) {
  if (is_playing()) return false;
  flush_pending_edit();
  workspace_.begin_edit(rotate ? "Rotate" : "Resize");
  if (!workspace_.begin_transform()) {
    workspace_.cancel_edit();
    return false;
  }
  edit_source_ = EditSource::Drag;
  handle_x_ = hx;
  handle_y_ = hy;
  handle_rotate_ = rotate;
  handle_active_ = true;
  handle_start_wx_ = wx;
  handle_start_wy_ = wy;
  return true;
}

void Editor2DScreen::update_handle_drag(float wx, float wy, bool shift) {
  if (!handle_active_) return;
  if (handle_rotate_) {
    const Entity2D* p = workspace_.selected();
    if (!p) return;
    // Angle swept around the start centre (centre does not move on a turn).
    const float cx = p->center_x(), cy = p->center_y();
    const float a0 = std::atan2(handle_start_wy_ - cy, handle_start_wx_ - cx);
    const float a1 = std::atan2(wy - cy, wx - cx);
    const float deg = (a1 - a0) * 180.0f / 3.14159265f;
    workspace_.rotate_to(deg, shift || workspace_.snap_enabled() ? 15.0f : 0.0f);
  } else {
    workspace_.resize_to(handle_x_, handle_y_, wx, wy, shift);
  }
  mark_dirty();
}

bool Editor2DScreen::end_handle_drag() {
  if (!handle_active_) return false;
  handle_active_ = false;
  const bool changed = workspace_.end_transform();
  if (edit_source_ == EditSource::Drag) flush_pending_edit();
  if (changed) {
    mark_dirty_and_autosave();
    note(handle_rotate_ ? "Turned" : "Resized");
  }
  return changed;
}

void Editor2DScreen::cancel_handle_drag() {
  if (!handle_active_) return;
  handle_active_ = false;
  workspace_.cancel_transform();
  if (edit_source_ == EditSource::Drag) {
    workspace_.cancel_edit();
    edit_source_ = EditSource::None;
  }
}

bool Editor2DScreen::begin_duplicate_drag() {
  if (is_playing() || workspace_.selection_count() == 0) return false;
  flush_pending_edit();
  Workspace2D::Snapshot before = workspace_.snapshot();
  const std::vector<std::uint64_t> ids = workspace_.duplicate_selection(false);
  if (ids.empty()) return false;
  workspace_.begin_edit("Duplicate " + std::to_string(ids.size()), std::move(before));
  edit_source_ = EditSource::Drag;
  dup_drag_ = true;
  workspace_.begin_move();
  return true;
}

// --- Hierarchy filter ----------------------------------------------------------

void Editor2DScreen::set_hierarchy_filter(const std::string& text) {
  std::snprintf(hierarchy_filter_, sizeof(hierarchy_filter_), "%s", text.c_str());
}

bool Editor2DScreen::hierarchy_row_visible(const Entity2D& e) const {
  if (hierarchy_filter_[0] == '\0') return true;
  const std::string needle = lower(hierarchy_filter_);
  if (lower(e.name).find(needle) != std::string::npos) return true;
  // Parents of a match stay listed so the match has somewhere to hang.
  for (std::uint64_t d : workspace_.descendants_of(e.id)) {
    const Entity2D* c = workspace_.find(d);
    if (c && lower(c->name).find(needle) != std::string::npos) return true;
  }
  return false;
}

// --- Context menu ----------------------------------------------------------------

void Editor2DScreen::draw_entity_context_menu(std::uint64_t clicked) {
  const Entity2D* e = workspace_.find(clicked);
  const std::size_t n = workspace_.selection_count();
  if (e) {
    ImGui::TextDisabled("%s%s", e->name.c_str(),
                        n > 1 ? ("  (+" + std::to_string(n - 1) + ")").c_str() : "");
    ImGui::Separator();
  }
  const bool has = n > 0;
  if (ImGui::MenuItem("Rename", "F2", false, e != nullptr && n == 1)) begin_rename(e->id);
  if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, has)) duplicate_selected();
  if (ImGui::MenuItem("Delete", "Del", false, has)) delete_selected();
  ImGui::Separator();
  if (ImGui::MenuItem("Copy", "Ctrl+C", false, has)) copy_selected();
  if (ImGui::MenuItem("Cut", "Ctrl+X", false, has)) cut_selected();
  if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboard_text().empty())) paste();
  ImGui::Separator();
  if (ImGui::MenuItem("Group", "Ctrl+G", false, has)) group_selected();
  bool any_kids = false;
  for (std::uint64_t id : workspace_.selection()) {
    any_kids |= !workspace_.children_of(id).empty();
  }
  if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, any_kids)) ungroup_selected();
  ImGui::Separator();
  draw_attach_menu_2d();
  if (e && e->camera && ImGui::MenuItem("Make active camera", nullptr, false, !is_playing()))
    make_active_camera_2d(e->id);
  ImGui::Separator();
  if (e) {
    if (ImGui::MenuItem(e->locked ? "Unlock" : "Lock", "Ctrl+L")) lock_selected(!e->locked);
    if (ImGui::MenuItem(e->hidden ? "Show" : "Hide", "H")) hide_selected(!e->hidden);
  }
  if (ImGui::MenuItem("Focus", "F", false, has)) frame_selected();
  if (n >= 2 && ImGui::BeginMenu("Align")) {
    if (ImGui::MenuItem("Left edges")) align_selected(Workspace2D::Align::Left);
    if (ImGui::MenuItem("Centres (horizontal)")) align_selected(Workspace2D::Align::CenterX);
    if (ImGui::MenuItem("Right edges")) align_selected(Workspace2D::Align::Right);
    ImGui::Separator();
    if (ImGui::MenuItem("Top edges")) align_selected(Workspace2D::Align::Top);
    if (ImGui::MenuItem("Middles (vertical)")) align_selected(Workspace2D::Align::CenterY);
    if (ImGui::MenuItem("Bottom edges")) align_selected(Workspace2D::Align::Bottom);
    ImGui::Separator();
    if (ImGui::MenuItem("Distribute horizontally", nullptr, false, n >= 3)) {
      distribute_selected(true);
    }
    if (ImGui::MenuItem("Distribute vertically", nullptr, false, n >= 3)) {
      distribute_selected(false);
    }
    ImGui::EndMenu();
  }
  if (e && ImGui::BeginMenu("Add component")) {
    const Entity2D* t = workspace_.find(clicked);
    if (ImGui::MenuItem("Collider", nullptr, false, t && !t->collider)) add_component(clicked, "Collider");
    if (ImGui::MenuItem("Player", nullptr, false, t && !t->player)) add_component(clicked, "Player");
    if (ImGui::MenuItem("Camera", nullptr, false, t && !t->camera)) add_component(clicked, "Camera");
    if (ImGui::MenuItem("Spawn Point", nullptr, false, t && !t->spawn)) add_component(clicked, "Spawn");
    ImGui::EndMenu();
  }
  // Always the last entries (the menu sizes to fit, so nothing is cut off).
  ImGui::Separator();
  if (ImGui::MenuItem("New Entity", nullptr, false, !is_playing())) create_entity("Entity");
  if (e && ImGui::MenuItem("New Child Entity", nullptr, false, !is_playing())) {
    const std::uint64_t parent = e->id;
    if (const std::uint64_t kid = create_entity("Entity")) reparent(kid, parent);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
