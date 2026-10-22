// Editor tools on top of Workspace2D_Tools.cpp: clipboard, group / lock /
// hide, hierarchy drops, align / distribute, framing, handle drags,
// Alt+drag copies and the shared right-click menu. Every edit is one undo
// step and autosaves scene.json; all locked while playing.
#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Actions.h"

#include <imgui.h>

#include "editor/ux/ContextMenus.h"

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

void Editor2DScreen::paste_components_2d(const std::string& only) {
  if (!comp_clip_ || is_playing()) return;
  const Workspace2D::Snapshot before = workspace_.snapshot();
  const Entity2D& src = *comp_clip_;
  for (std::uint64_t id : workspace_.selection()) {
    Entity2D* d = workspace_.find(id);
    if (!d || d->locked || d->id == src.id) continue;
    auto want = [&](const char* name) { return only.empty() || only == name; };
    if (want("Sprite")) d->sprite = src.sprite;
    if (want("Player")) d->player = src.player;
    if (want("Camera")) d->camera = src.camera;
    if (want("Follow")) d->follow = src.follow;
    if (want("Spawn Point")) d->spawn = src.spawn;
    if (want("Collision")) d->collider = src.collider;
    if (want("Animator")) d->animator = src.animator;
    if (want("Audio Source")) d->audio = src.audio;
    if (want("Particles")) d->particles = src.particles;
    if (want("Light")) d->light = src.light;
    if (want("Rigid Body")) d->body = src.body;
    if (want("Joint")) d->joint = src.joint;
    if (only.empty()) {
      d->script = src.script;
      d->extra_scripts = src.extra_scripts;
    }
    normalize_components(*d);
  }
  if (workspace_.commit_step(only.empty() ? "Paste components" : "Paste " + only, before)) mark_dirty();
}

namespace {
// The component a 2D Inspector section edits: has / reset / remove.
bool comp_has_2d(const Entity2D& e, const std::string& n) {
  if (n == "Sprite") return e.sprite.has_value();
  if (n == "Collision") return e.collider.has_value();
  if (n == "Rigid Body") return e.body.has_value();
  if (n == "Joint") return e.joint.has_value();
  if (n == "Animator") return e.animator.has_value();
  if (n == "Follow") return e.follow.has_value();
  if (n == "Particles") return e.particles.has_value();
  if (n == "Light") return e.light.has_value();
  if (n == "Audio Source") return e.audio.has_value();
  return false;
}
void comp_set_2d(Entity2D& e, const std::string& n, bool keep) {
  if (n == "Sprite") { if (keep) e.sprite = SpriteData{}; else e.sprite.reset(); }
  if (n == "Collision") { if (keep) e.collider = default_collider(e); else e.collider.reset(); }
  if (n == "Rigid Body") { if (keep) e.body = RigidBody2DData{}; else e.body.reset(); }
  if (n == "Joint") { if (keep) e.joint = Joint2DData{}; else e.joint.reset(); }
  if (n == "Animator") { if (keep) e.animator = AnimatorData{}; else e.animator.reset(); }
  if (n == "Follow") {
    if (keep) { const std::uint64_t tgt = e.follow ? e.follow->target : 0; e.follow = FollowData{}; e.follow->target = tgt; }
    else e.follow.reset();
  }
  if (n == "Particles") { if (keep) e.particles = fx::preset("campfire", fx::Space::TwoD); else e.particles.reset(); }
  if (n == "Light") { if (keep) e.light = fx::Light2DData{}; else e.light.reset(); }
  if (n == "Audio Source") { if (keep) e.audio = AudioSourceData{}; else e.audio.reset(); }
  normalize_components(e);
}
}  // namespace

void Editor2DScreen::component_header_menu_2d(std::uint64_t id, const char* name) {
  const Entity2D* e = workspace_.find(id);
  if (!e) return;
  const bool present = comp_has_2d(*e, name);
  const bool editable = !is_playing() && !e->locked;
  const std::string n = name;
  ux::HeaderOps ops;
  auto change = [this, id, n](const std::string& label, bool keep) {
    Workspace2D::Snapshot before = prepare_edit();
    if (Entity2D* t = workspace_.find(id)) comp_set_2d(*t, n, keep);
    commit_discrete(std::move(before), label, "");
  };
  if (present && editable) {
    ops.reset = [change, n] { change("Reset " + n, true); };
    ops.remove = [change, n] { change("Remove " + n, false); };
  }
  if (present)
    ops.copy = [this, id, n] {
      if (const Entity2D* t = workspace_.find(id)) {
        comp_clip_ = *t;
        comp_clip_name_ = n;
        note("Copied " + n + " values from " + t->name);
      }
    };
  ops.can_paste = editable && comp_clip_ && (comp_clip_name_ == n || comp_clip_name_.empty()) &&
                  comp_has_2d(*comp_clip_, n) && comp_clip_->id != id;
  if (comp_clip_) ops.paste_from = n + " on " + comp_clip_->name;
  ops.paste = [this, n] { paste_components_2d(n); };
  // The Inspector is mid-draw (it holds references into the entity): apply
  // after it finishes (draw_inspector runs header_op_).
  auto later = [this](std::function<void()>& f) {
    if (!f) return;
    std::function<void()> g = f;
    f = [this, g] { header_op_ = g; };
  };
  later(ops.reset);
  later(ops.remove);
  later(ops.paste);
  ux::ComponentHeaderMenu(name, ops);
}

bool Editor2DScreen::component_header_2d(std::uint64_t id, const char* name) {
  const bool open = ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);
  component_header_menu_2d(id, name);
  return open;
}

void Editor2DScreen::draw_entity_context_menu(std::uint64_t clicked) {
  const Entity2D* e = workspace_.find(clicked);
  const std::size_t n = workspace_.selection_count();
  if (e) {
    ImGui::TextDisabled("%s%s", e->name.c_str(),
                        n > 1 ? ("  (+" + std::to_string(n - 1) + ")").c_str() : "");
    ImGui::Separator();
  } else if (context_in_viewport_) {
    // Empty viewport space: make something right here.
    const float wx = context_world_x_, wy = context_world_y_;
    if (ux::BeginMenu("Create here")) {
      auto make = [&](const char* name, const char* comp) {
        const std::uint64_t id = create_entity(name);
        if (!id) return;
        if (comp) add_component(id, comp);
        if (Entity2D* m = workspace_.find(id)) {
          m->x = wx - m->w * 0.5f;
          m->y = wy - m->h * 0.5f;
          mark_dirty();
        }
      };
      if (ux::MenuItem("Empty entity", nullptr, false, !is_playing())) make("Entity", nullptr);
      if (ux::MenuItem("Collider box", nullptr, false, !is_playing())) make("Wall", "Collider");
      if (ux::MenuItem("Player", nullptr, false, !is_playing())) make("Rider", "Player");
      if (ux::MenuItem("Camera", nullptr, false, !is_playing())) make("Camera", "Camera");
      if (ux::MenuItem("Spawn point", nullptr, false, !is_playing())) make("Spawn", "Spawn");
      ux::EndMenu();
    }
    if (ux::MenuItem("Paste here", "Ctrl+V", false, !clipboard_text().empty() && !is_playing())) {
      paste();
      // Centre what landed on the click.
      float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
      for (std::uint64_t id : workspace_.selection())
        if (const Entity2D* p = workspace_.find(id)) {
          minx = std::min(minx, p->x);
          miny = std::min(miny, p->y);
          maxx = std::max(maxx, p->x + p->w);
          maxy = std::max(maxy, p->y + p->h);
        }
      if (minx < maxx) {
        const float dx = wx - (minx + maxx) * 0.5f, dy = wy - (miny + maxy) * 0.5f;
        for (std::uint64_t id : workspace_.selection())
          if (Entity2D* p = workspace_.find(id)) {
            p->x += dx;
            p->y += dy;
          }
        mark_dirty();
      }
    }
    if (ux::MenuItem("Set 2D cursor here", "Shift+RMB")) set_cursor2d(wx, wy);
    if (ux::BeginMenu("View options")) {
      const bool grid = workspace_.show_grid();
      if (ux::MenuItem("Show grid", "'", grid, !is_playing())) {
        workspace_.set_show_grid(!grid);
        mark_dirty();
      }
      const bool snap = workspace_.snap_enabled();
      if (ux::MenuItem("Snap to grid", "Shift+G", snap, !is_playing())) {
        workspace_.set_snap_enabled(!snap);
        mark_dirty();
      }
      if (ux::MenuItem("Reset camera")) {
        workspace_.set_pan(0.0f, 0.0f);
        workspace_.set_zoom(1.0f);
      }
      ux::EndMenu();
    }
    ImGui::Separator();
  }
  const bool has = n > 0;
  if (ux::MenuItem("Rename", "F2", false, e != nullptr && n == 1)) begin_rename(e->id);
  if (ux::MenuItem("Duplicate", "Ctrl+D", false, has)) duplicate_selected();
  if (ux::MenuItem("Delete", "Del", false, has)) delete_selected();
  ImGui::Separator();
  if (ux::MenuItem("Copy", "Ctrl+C", false, has)) copy_selected();
  if (ux::MenuItem("Cut", "Ctrl+X", false, has)) cut_selected();
  if (ux::MenuItem("Paste", "Ctrl+V", false, !clipboard_text().empty())) paste();
  ImGui::Separator();
  if (ux::MenuItem("Group", "Ctrl+G", false, has)) group_selected();
  bool any_kids = false;
  for (std::uint64_t id : workspace_.selection()) {
    any_kids |= !workspace_.children_of(id).empty();
  }
  if (ux::MenuItem("Ungroup", "Ctrl+Shift+G", false, any_kids)) ungroup_selected();
  ImGui::Separator();
  draw_attach_menu_2d();
  if (e && e->camera && ux::MenuItem("Make active camera", nullptr, false, !is_playing()))
    make_active_camera_2d(e->id);
  ImGui::Separator();
  if (e) {
    if (ux::MenuItem(e->locked ? "Unlock" : "Lock", "Ctrl+L")) lock_selected(!e->locked);
    if (ux::MenuItem(e->hidden ? "Show" : "Hide", "H")) hide_selected(!e->hidden);
  }
  if (ux::MenuItem("Focus", "F", false, has)) frame_selected();
  if (has && ux::BeginMenu("Reset transform")) {
    if (ux::MenuItem("Location", "Alt+G")) reset_transform(XformOp::Move);
    if (ux::MenuItem("Rotation", "Alt+R")) reset_transform(XformOp::Rotate);
    if (ux::MenuItem("Size", "Alt+S")) reset_transform(XformOp::Scale);
    ux::EndMenu();
  }
  if (ux::MenuItem("Copy components", nullptr, false, e != nullptr)) {
    comp_clip_ = *e;
    comp_clip_name_.clear();
    note("Copied " + e->name + "'s components");
  }
  if (ux::MenuItem("Paste components", nullptr, false, has && comp_clip_ && !is_playing())) paste_components_2d({});
  if (n >= 2 && ux::BeginMenu("Align")) {
    if (ux::MenuItem("Left edges")) align_selected(Workspace2D::Align::Left);
    if (ux::MenuItem("Centres (horizontal)")) align_selected(Workspace2D::Align::CenterX);
    if (ux::MenuItem("Right edges")) align_selected(Workspace2D::Align::Right);
    ImGui::Separator();
    if (ux::MenuItem("Top edges")) align_selected(Workspace2D::Align::Top);
    if (ux::MenuItem("Middles (vertical)")) align_selected(Workspace2D::Align::CenterY);
    if (ux::MenuItem("Bottom edges")) align_selected(Workspace2D::Align::Bottom);
    ImGui::Separator();
    if (ux::MenuItem("Distribute horizontally", nullptr, false, n >= 3)) {
      distribute_selected(true);
    }
    if (ux::MenuItem("Distribute vertically", nullptr, false, n >= 3)) {
      distribute_selected(false);
    }
    ux::EndMenu();
  }
  if (e && ux::BeginMenu("Add component")) {
    const Entity2D* t = workspace_.find(clicked);
    if (ux::MenuItem("Collider", nullptr, false, t && !t->collider)) add_component(clicked, "Collider");
    if (ux::MenuItem("Player", nullptr, false, t && !t->player)) add_component(clicked, "Player");
    if (ux::MenuItem("Camera", nullptr, false, t && !t->camera)) add_component(clicked, "Camera");
    if (ux::MenuItem("Spawn Point", nullptr, false, t && !t->spawn)) add_component(clicked, "Spawn");
    ux::EndMenu();
  }
  // Always the last entries (the menu sizes to fit, so nothing is cut off).
  ImGui::Separator();
  if (ux::MenuItem("New Entity", nullptr, false, !is_playing())) create_entity("Entity");
  if (e && ux::MenuItem("New Child Entity", nullptr, false, !is_playing())) {
    const std::uint64_t parent = e->id;
    if (const std::uint64_t kid = create_entity("Entity")) reparent(kid, parent);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
