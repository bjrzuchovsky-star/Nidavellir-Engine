// 3D editor cameras (docs/camera.md): camera entities you can see, select
// and move (icon + frustum / view cone in the edit view, hidden in Play),
// Create > Camera, make active, align to view (Ctrl+Alt+Numpad0), look
// through (Numpad0), a picture-in-picture preview of the selected camera
// and the v18 migration of the old built-in follow camera on open.
#include "editor/editor3d/Editor3DScreen.h"

#include "render3d/CameraMigrate3D.h"
#include "render3d/GlRenderer3D.h"
#include "scene/CameraMigrate.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

using render3d::Vec3;
using scene3d::Entity3D;
using runtime::LogLevel;

namespace {

constexpr float kCamLine[4] = {0.95f, 0.78f, 0.35f, 1.0f};     // brass
constexpr float kCamSelected[4] = {1.0f, 0.55f, 0.15f, 1.0f};  // lantern orange
constexpr float kCamIdle[4] = {0.62f, 0.56f, 0.48f, 1.0f};     // weathered pine

std::string unique_camera_name(const scene3d::Scene3D& s) {
  if (!s.find_named("Camera")) return "Camera";
  for (int n = 2;; ++n) {
    const std::string name = "Camera " + std::to_string(n);
    if (!s.find_named(name)) return name;
  }
}

}  // namespace

render3d::Camera3D Editor3DScreen::camera_view(const Entity3D& e) const {
  render3d::Camera3D cam;
  const scene3d::PlayCamera3D pc = e.camera ? *e.camera : scene3d::PlayCamera3D{};
  const render3d::Quat q = e.transform.rotation;
  cam.position = e.transform.position;
  cam.target = cam.position + q.rotate(Vec3{0, 0, -1});
  cam.up = q.rotate(Vec3{0, 1, 0});
  cam.fov_y_degrees = pc.fov;
  cam.z_near = pc.near_clip;
  cam.z_far = pc.far_clip;
  cam.orthographic = pc.projection == scene3d::PlayCamera3D::Projection::Orthographic;
  cam.ortho_size = pc.ortho_size;
  return cam;
}

// ---- Migration ---------------------------------------------------------------------

void Editor3DScreen::migrate_cameras_on_open(const std::string& path) {
  camera_backup_.clear();
  if (!scene_.legacy_cameras) return;
  scene3d::Scene3D migrated = scene_;
  std::vector<std::string> lines;
  const int changed = scene3d::migrate_cameras(&migrated, &lines);
  if (changed == 0) {
    // Nothing to add (no rider): the flag goes, the file is rewritten on
    // the next save as v18.
    scene_.legacy_cameras = false;
    return;
  }
  std::string err;
  const std::string rel = camera_migrate::backup_scene(project_.path, path, &err);
  if (rel.empty()) {
    // No backup, no migration: Play keeps the old follow camera.
    note("Camera migration skipped (could not back up the scene: " + err + ")", LogLevel::Warn);
    return;
  }
  scene_ = std::move(migrated);
  if (!scene3d::save_file(path, scene_, &err)) {
    note("Camera migration could not save the scene: " + err, LogLevel::Error);
  }
  camera_backup_ = rel;
  std::vector<std::string> log{"scene.json (3D): cameras (v18), backup " + rel};
  for (const std::string& l : lines) log.push_back("  " + l);
  camera_migrate::append_log(project_.path, log);
  for (const std::string& l : lines) note("Camera migration: " + l);
  note("The follow camera is now a scene object (backup: " + rel + ")");
}

// ---- Edits -------------------------------------------------------------------------

std::uint64_t Editor3DScreen::create_camera() {
  if (playing()) return 0;
  const scene3d::Scene3D before = scene_;
  Entity3D e = scene3d::make_camera_entity(scene_.next_id(), unique_camera_name(scene_));
  const render3d::Camera3D view = editor_camera();
  e.transform.position = view.position;
  e.transform.rotation = scene3d::look_rotation(view.forward(), {0, 1, 0});
  scene_.entities.push_back(e);
  scene_.selection = {e.id};
  push_undo("Create " + e.name, before);
  return e.id;
}

bool Editor3DScreen::set_camera(std::uint64_t id, const scene3d::PlayCamera3D& c,
                                const std::string& label) {
  const Entity3D* e = scene_.find(id);
  if (playing() || !e) return false;
  Entity3D copy = *e;
  copy.camera = c;
  return replace_entity(copy, label);
}

bool Editor3DScreen::make_active_camera(std::uint64_t id) {
  const Entity3D* e = scene_.find(id);
  if (playing() || !e || !e->camera) return false;
  int top = e->camera->priority;
  for (const Entity3D& o : scene_.entities) {
    if (o.id != id && o.camera && o.camera->active) top = std::max(top, o.camera->priority + 1);
  }
  if (e->camera->active && top == e->camera->priority && play_camera_id() == id) return false;
  scene3d::PlayCamera3D c = *e->camera;
  c.active = true;
  c.priority = top;
  return set_camera(id, c, "Make " + e->name + " the active camera");
}

std::uint64_t Editor3DScreen::play_camera_id() const {
  std::uint64_t best = 0;
  int best_p = 0;
  for (const Entity3D& e : scene_.entities) {
    if (!e.camera || !e.camera->active) continue;
    if (best == 0 || e.camera->priority > best_p) {
      best = e.id;
      best_p = e.camera->priority;
    }
  }
  return best;
}

bool Editor3DScreen::align_camera_to_view(std::uint64_t id) {
  if (playing()) return false;
  if (id == 0) {
    const Entity3D* sel = scene_.find(primary());
    id = sel && sel->camera ? sel->id : play_camera_id();
  }
  const Entity3D* e = scene_.find(id);
  if (!e || !e->camera) {
    note("Align camera to view: no camera (select one or add Create > Camera)", LogLevel::Warn);
    return false;
  }
  look_through_ = 0;
  const render3d::Camera3D view = editor_camera();
  render3d::Transform t = e->transform;
  t.position = view.position;
  t.rotation = scene3d::look_rotation(view.forward(), {0, 1, 0});
  const scene3d::Scene3D before = scene_;
  scene3d::move_with_children(scene_.entities, id, t);
  push_undo("Align " + e->name + " to view", before);
  return true;
}

bool Editor3DScreen::look_through_camera(std::uint64_t id) {
  if (id == 0) {
    const Entity3D* sel = scene_.find(primary());
    id = sel && sel->camera ? sel->id : play_camera_id();
  }
  const Entity3D* e = scene_.find(id);
  if (!e || !e->camera) {
    note("Look through camera: no camera in the scene", LogLevel::Warn);
    return false;
  }
  if (look_through_ == id) {
    look_through_ = 0;  // Numpad0 again: back to the editor view
    return true;
  }
  look_through_ = id;
  note("Looking through " + e->name + " (orbit, pan or Numpad0 to leave)");
  return true;
}

// ---- Drawing -----------------------------------------------------------------------

void Editor3DScreen::draw_camera_lines() {
  if (!renderer_) return;
  const float aspect = viewport_h_ > 0 ? viewport_w_ / viewport_h_ : 16.0f / 9.0f;
  const std::uint64_t active = play_camera_id();
  for (const Entity3D& e : scene_.entities) {
    if (!e.camera || e.id == look_through_ || effectively_hidden(e)) continue;
    const bool sel = std::find(scene_.selection.begin(), scene_.selection.end(), e.id) !=
                     scene_.selection.end();
    const float* col = sel ? kCamSelected : (e.id == active ? kCamLine : kCamIdle);
    const scene3d::PlayCamera3D& c = *e.camera;
    const render3d::Quat q = e.transform.rotation;
    const Vec3 p = e.transform.position;
    const Vec3 f = q.rotate(Vec3{0, 0, -1}), r = q.rotate(Vec3{1, 0, 0}), u = q.rotate(Vec3{0, 1, 0});
    // A short view cone (selected: out to a few metres, the far plane
    // clipped so big far values do not cover the map).
    const float depth = std::min(c.far_clip, sel ? 6.0f : 1.6f);
    const bool ortho = c.projection == scene3d::PlayCamera3D::Projection::Orthographic;
    auto corners = [&](float d, Vec3 out[4]) {
      float hh = ortho ? c.ortho_size : std::tan(render3d::radians(c.fov) * 0.5f) * d;
      if (ortho && !sel) hh = std::min(hh, 1.0f);
      const float hw = hh * aspect * (c.viewport[3] > 0 ? c.viewport[2] / c.viewport[3] : 1.0f);
      const Vec3 m = p + f * d;
      out[0] = m + r * hw + u * hh;
      out[1] = m - r * hw + u * hh;
      out[2] = m - r * hw - u * hh;
      out[3] = m + r * hw - u * hh;
    };
    Vec3 n[4], fr[4];
    corners(std::max(c.near_clip, 0.05f), n);
    corners(depth, fr);
    for (int i = 0; i < 4; ++i) {
      renderer_->draw_line(fr[i], fr[(i + 1) % 4], col, false);
      renderer_->draw_line(n[i], n[(i + 1) % 4], col, false);
      renderer_->draw_line(n[i], fr[i], col, false);
    }
    // Up marker: a little triangle on top of the far rectangle.
    const Vec3 top = (fr[0] + fr[1]) * 0.5f + u * (0.15f * depth);
    renderer_->draw_line(fr[0] * 0.8f + fr[1] * 0.2f, top, col, false);
    renderer_->draw_line(fr[1] * 0.8f + fr[0] * 0.2f, top, col, false);
    // Body: a small box behind the lens.
    const float b = 0.18f;
    const Vec3 back = p - f * (b * 1.6f);
    const Vec3 bc[4] = {back + r * b + u * b, back - r * b + u * b, back - r * b - u * b, back + r * b - u * b};
    const Vec3 fc[4] = {p + r * b + u * b, p - r * b + u * b, p - r * b - u * b, p + r * b - u * b};
    for (int i = 0; i < 4; ++i) {
      renderer_->draw_line(bc[i], bc[(i + 1) % 4], col, false);
      renderer_->draw_line(fc[i], fc[(i + 1) % 4], col, false);
      renderer_->draw_line(bc[i], fc[i], col, false);
    }
    // Rig: a dotted line to what it follows.
    if (c.rig != scene3d::PlayCamera3D::Rig::Fixed) {
      const Entity3D* tgt = c.follow ? scene_.find(c.follow) : nullptr;
      if (!tgt) {
        for (const Entity3D& o : scene_.entities) {
          if (o.player) { tgt = &o; break; }
        }
      }
      if (tgt) {
        const Vec3 a = p, d = tgt->transform.position - p;
        for (int k = 0; k < 10; k += 2)
          renderer_->draw_line(a + d * (k / 10.0f), a + d * ((k + 1) / 10.0f), kCamIdle, false);
      }
    }
  }
}

bool Editor3DScreen::render_camera_preview(int width, int height) {
  const Entity3D* e = scene_.find(primary());
  if (!e || !e->camera || playing() || look_through_ == e->id) return false;
  if (!preview_renderer_) {
    if (gl_) {
      auto gl = render3d::make_gl_renderer3d();
      std::string err;
      if (gl->init(&err)) preview_renderer_ = std::move(gl);
    }
    if (!preview_renderer_) {
      preview_renderer_ = render3d::make_null_renderer3d();
      std::string err;
      preview_renderer_->init(&err);
    }
    preview_renderer_->set_offscreen(true);
    preview_view_ = std::make_unique<render3d::SceneView>(preview_renderer_.get());
    preview_view_->set_project_dir(project_.path);
  }
  const scene3d::PlayCamera3D& c = *e->camera;
  render3d::Environment env = scene_.environment;
  if (c.use_clear) {
    for (int i = 0; i < 3; ++i) env.sky_top[i] = env.sky_horizon[i] = env.ground[i] = c.clear_color[i];
  }
  std::vector<Entity3D> shown;
  for (const Entity3D& o : scene_.entities) {
    if (!effectively_hidden(o)) shown.push_back(o);
  }
  render3d::SceneViewOptions opt;  // what Play shows: no grid, gizmos or colliders
  preview_view_->begin(env, shown, camera_view(*e), width, height, opt);
  preview_view_->end();
  return true;
}

std::uint64_t Editor3DScreen::camera_preview_texture() const {
  return preview_renderer_ ? preview_renderer_->color_texture() : 0;
}

void Editor3DScreen::draw_camera_overlay(float x0, float y0, float vw, float vh) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const render3d::Camera3D view = editor_camera();
  const std::uint64_t active = play_camera_id();
  // Icons: a little film camera at each camera entity.
  for (const Entity3D& e : scene_.entities) {
    if (!e.camera || e.id == look_through_ || effectively_hidden(e)) continue;
    float sx = 0, sy = 0;
    if (!view.project(e.transform.position, vw, vh, &sx, &sy)) continue;
    if (sx < -20 || sy < -20 || sx > vw + 20 || sy > vh + 20) continue;
    const bool sel = std::find(scene_.selection.begin(), scene_.selection.end(), e.id) !=
                     scene_.selection.end();
    const ImU32 col = sel ? IM_COL32(255, 140, 40, 255)
                          : (e.id == active ? IM_COL32(242, 200, 90, 255) : IM_COL32(170, 150, 125, 230));
    const ImVec2 c(x0 + sx, y0 + sy - 16.0f);
    dl->AddRectFilled(ImVec2(c.x - 9, c.y - 6), ImVec2(c.x + 5, c.y + 6), IM_COL32(28, 22, 16, 210), 2.0f);
    dl->AddRect(ImVec2(c.x - 9, c.y - 6), ImVec2(c.x + 5, c.y + 6), col, 2.0f, 0, 1.5f);
    dl->AddTriangleFilled(ImVec2(c.x + 5, c.y), ImVec2(c.x + 11, c.y - 5), ImVec2(c.x + 11, c.y + 5), col);
    dl->AddCircleFilled(ImVec2(c.x - 5, c.y - 9), 3.0f, col);
    dl->AddCircleFilled(ImVec2(c.x + 1, c.y - 9), 3.0f, col);
    if (e.id == active) dl->AddText(ImVec2(c.x + 14, c.y - 7), col, "Play");
  }
  if (look_through_) {
    if (const Entity3D* e = scene_.find(look_through_)) {
      const std::string label = "Looking through " + e->name + "  (Numpad0 to leave)";
      dl->AddRect(ImVec2(x0 + 2, y0 + 2), ImVec2(x0 + vw - 2, y0 + vh - 2), IM_COL32(242, 200, 90, 200), 0.0f, 0, 2.0f);
      dl->AddText(ImVec2(x0 + 12, y0 + vh - 44), IM_COL32(242, 200, 90, 255), label.c_str());
    }
  }
  // Preview: a quarter-size picture of the selected camera, bottom right.
  const Entity3D* sel = scene_.find(primary());
  if (!sel || !sel->camera || look_through_ == sel->id) return;
  const float pw = std::clamp(vw * 0.28f, 160.0f, 420.0f);
  const float ph = pw * 9.0f / 16.0f;
  const float scale = ImGui::GetIO().DisplayFramebufferScale.x > 0 ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
  if (!render_camera_preview(static_cast<int>(pw * scale), static_cast<int>(ph * scale))) return;
  const ImVec2 a(x0 + vw - pw - 12, y0 + vh - ph - 34), b(a.x + pw, a.y + ph);
  dl->AddRectFilled(ImVec2(a.x - 3, a.y - 20), ImVec2(b.x + 3, b.y + 3), IM_COL32(28, 22, 16, 235), 3.0f);
  const std::string title = sel->name + " preview";
  dl->AddText(ImVec2(a.x + 2, a.y - 17), IM_COL32(242, 200, 90, 255), title.c_str());
  if (const std::uint64_t tex = camera_preview_texture()) {
    dl->AddImage(static_cast<ImTextureID>(tex), a, b, ImVec2(0, 1), ImVec2(1, 0));
  } else {
    dl->AddRectFilled(a, b, IM_COL32(40, 34, 28, 255));
    dl->AddText(ImVec2(a.x + 8, a.y + 8), IM_COL32(170, 150, 125, 255), "(no GL preview)");
  }
  dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(242, 200, 90, 200));
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
