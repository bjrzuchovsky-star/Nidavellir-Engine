#include "editor/editor3d/Editor3DScreen.h"
#include "terrain/TerrainView.h"
#include "plugins/profiler/ProfilerPlugin.h"
#include "profiler/Profiler.h"
#include "debugtools/ProfilerWorld3D.h"

#include "ai3d/AiWorld3D.h"
#include "plugins/story/StoryPlugins.h"
#include "story3d/StoryWorld3D.h"
#include "weather/Weather3D.h"

#include "ui/UiScript.h"
#include "editor/assets/AssetLibrary.h"
#include "editor/audio/EditorAudio.h"
#include "world3d/VrOverlay.h"

#include "render3d/GlRenderer3D.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;
using render3d::Vec3;
using runtime::LogLevel;
using scene3d::Entity3D;

Editor3DScreen::Editor3DScreen(ProjectInfo project) : project_(std::move(project)) {}

Editor3DScreen::~Editor3DScreen() {
  world_.stop();
  view_.reset();
  if (renderer_) renderer_->shutdown();
  plugins_.close();
}

void Editor3DScreen::note(const std::string& text, LogLevel level) {
  runtime::LogEntry e;
  e.level = level;
  e.channel = "editor";
  e.text = text;
  log_.push_back(std::move(e));
  while (log_.size() > 400) log_.pop_front();
}

void Editor3DScreen::on_enter() {
  back_requested_ = quit_requested_ = false;
  log_.clear();
  plugins_.open(&project_, "editor3d", this,
                [this](runtime::LogLevel level, const std::string& text) {
                  runtime::LogEntry e;
                  e.level = level;
                  e.channel = "plugins";
                  e.text = text;
                  log_.push_back(std::move(e));
                });
  std::string err;
  const std::string path = scene3d::scene_path(project_.path);
  if (!fs::exists(path)) {
    scene_ = vr_project() ? scene3d::make_vr_starter_scene() : scene3d::make_starter_scene();
    if (!scene3d::save_file(path, scene_, &err)) {
      note("Could not write the starter scene: " + err, LogLevel::Error);
    } else {
      note(vr_project() ? "Staked a fresh VR claim: ground, crate, lamp, a table of things to "
                          "grab and the VR rig"
                        : "Staked a fresh 3D claim: ground, crate, lamp and a rider");
    }
  } else if (!scene3d::load_file(path, &scene_, &err)) {
    note(err, LogLevel::Error);
    scene_ = scene3d::Scene3D{};
  }
  undo_.clear();
  redo_.clear();
  prefabs_.set_project(project_.path);
  prefab_sync_all_on_load();
  open_asset_browser();
  rescan_assets();
  ensure_renderer();
  note("Telegraph open for " + project_.name + " (" + std::to_string(scene_.entities.size()) +
       " entities, " + std::to_string(assets_.size()) + " assets)");
  std::cout << "[Editor3D] opened \"" << project_.name << "\" (" << scene_.entities.size()
            << " entities, renderer " << renderer_backend() << ")\n";
}

void Editor3DScreen::on_exit() {
  if (playing()) play_stop();
  std::string err;
  save_scene(&err);
  plugins_.close();
  view_.reset();
  if (renderer_) renderer_->shutdown();
  renderer_.reset();
  std::cout << "[Editor3D] closed \"" << project_.name << "\"\n";
}

void Editor3DScreen::ensure_renderer() {
  const bool want_gl = ImGui::GetCurrentContext() != nullptr && glfwGetCurrentContext() != nullptr;
  if (renderer_ && gl_ == want_gl) return;
  view_.reset();
  if (renderer_) renderer_->shutdown();
  renderer_.reset();
  gl_ = false;
  if (want_gl) {
    auto gl = render3d::make_gl_renderer3d();
    std::string err;
    if (gl->init(&err)) {
      renderer_ = std::move(gl);
      gl_ = true;
    } else {
      note("GL 3.3 renderer would not start (" + err + "); drawing nothing", LogLevel::Error);
    }
  }
  if (!renderer_) {
    renderer_ = render3d::make_null_renderer3d();
    std::string err;
    renderer_->init(&err);
  }
  renderer_->set_offscreen(true);
  view_ = std::make_unique<render3d::SceneView>(renderer_.get());
  view_->set_project_dir(project_.path);
}

const char* Editor3DScreen::renderer_backend() const {
  return renderer_ ? renderer_->backend_name() : "none";
}

std::uint64_t Editor3DScreen::viewport_texture() const {
  return renderer_ ? renderer_->color_texture() : 0;
}

void Editor3DScreen::collect_problems() {
  if (!view_) return;
  for (const std::string& p : view_->take_problems()) note(p, LogLevel::Warn);
}

AppState Editor3DScreen::on_update(float delta_seconds) {
  if (vr_previewing()) {
    vr_preview_frame(delta_seconds);
    for (const world3d::Toast3D& t : world_.take_toasts()) toasts_.push_back({t.text, t.seconds});
  } else if (playing()) {
    world_.update(delta_seconds, ImGui::GetCurrentContext() && !play_ui_.wants_input()
                                     ? poll_play_input()
                                     : world3d::Input3D{});
    ui::apply_ui_commands(play_ui_.system(), world_.take_ui_commands());
    for (const world3d::Toast3D& t : world_.take_toasts()) toasts_.push_back({t.text, t.seconds});
  }
  if (!playing()) anim_preview_tick(delta_seconds);
  prefab_poll_clock_ += delta_seconds;
  if (prefab_poll_clock_ > 1.0 && !playing()) {
    prefab_poll_clock_ = 0.0;
    prefab_poll();
  }
  asset_poll_clock_ += delta_seconds;
  if (asset_poll_clock_ > 1.5 && !playing()) {
    asset_poll_clock_ = 0.0;
    asset_browser_.rescan();
  }
  update_sound(delta_seconds);
  for (auto& t : toasts_) t.second -= delta_seconds;
  while (!toasts_.empty() && toasts_.front().second <= 0) toasts_.pop_front();
  if (ImGui::GetCurrentContext() != nullptr) draw_ui();
  if (quit_requested_) return AppState::Quit;
  return back_requested_ ? AppState::ProjectManager : AppState::Editor3D;
}

// ---- Selection -------------------------------------------------------------

std::uint64_t Editor3DScreen::primary() const {
  return scene_.selection.empty() ? 0 : scene_.selection.back();
}

void Editor3DScreen::select(std::uint64_t id, bool additive) {
  if (!scene_.find(id)) {
    if (!additive) scene_.selection.clear();
    return;
  }
  auto& sel = scene_.selection;
  const auto it = std::find(sel.begin(), sel.end(), id);
  if (additive) {
    if (it != sel.end()) sel.erase(it);
    else sel.push_back(id);
  } else {
    sel.assign(1, id);
  }
}

void Editor3DScreen::clear_selection() { scene_.selection.clear(); }

// ---- Undo ---------------------------------------------------------------------

void Editor3DScreen::push_undo(const std::string& label, const scene3d::Scene3D& before) {
  undo_.push_back({label, before, {}});
  if (undo_.size() > 200) undo_.erase(undo_.begin());
  redo_.clear();
  autosave();
}

void Editor3DScreen::autosave() {
  std::string err;
  if (!save_scene(&err)) note("Autosave failed: " + err, LogLevel::Error);
}

const std::string& Editor3DScreen::undo_label() const {
  static const std::string none;
  return undo_.empty() ? none : undo_.back().label;
}
const std::string& Editor3DScreen::redo_label() const {
  static const std::string none;
  return redo_.empty() ? none : redo_.back().label;
}

bool Editor3DScreen::undo() {
  if (undo_.empty() || playing()) return false;
  gizmo_cancel();
  prefabs_.on_undo(undo_.size(), undo_.back().label);
  Step s = std::move(undo_.back());
  undo_.pop_back();
  redo_.push_back({s.label, scene_, swap_terrains(std::move(s.terrains))});
  scene_ = std::move(s.scene);
  autosave();
  note("Undo " + redo_.back().label);
  return true;
}

bool Editor3DScreen::redo() {
  if (redo_.empty() || playing()) return false;
  gizmo_cancel();
  Step s = std::move(redo_.back());
  redo_.pop_back();
  undo_.push_back({s.label, scene_, swap_terrains(std::move(s.terrains))});
  prefabs_.on_redo(undo_.size(), s.label);
  scene_ = std::move(s.scene);
  autosave();
  note("Redo " + undo_.back().label);
  return true;
}

bool Editor3DScreen::save_scene(std::string* error) {
  refresh_prefab_overrides();
  if (!scene3d::save_file(scene3d::scene_path(project_.path), scene_, error)) return false;
  return save_terrains(error);
}

bool Editor3DScreen::reload_scene(std::string* error) {
  scene3d::Scene3D s;
  if (!scene3d::load_file(scene3d::scene_path(project_.path), &s, error)) return false;
  scene_ = std::move(s);
  undo_.clear();
  redo_.clear();
  terrain::forget(project_.path);  // re-read heightfields from disk
  if (view_) view_->clear();
  return true;
}

// ---- Edits -------------------------------------------------------------------

std::uint64_t Editor3DScreen::add_entity(Entity3D e, const std::string& label) {
  if (playing()) return 0;
  const scene3d::Scene3D before = scene_;
  e.id = scene_.next_id();
  // Unique-ish name: "Cube", "Cube 2", ...
  const std::string base = e.name.empty() ? "Entity" : e.name;
  std::string name = base;
  for (int n = 2; scene_.find_named(name); ++n) name = base + " " + std::to_string(n);
  e.name = name;
  scene_.entities.push_back(e);
  scene_.selection.assign(1, e.id);
  push_undo(label, before);
  return e.id;
}

std::uint64_t Editor3DScreen::create_primitive(const std::string& shape, const Vec3& pos) {
  render3d::MeshData probe;
  if (!render3d::make_primitive(shape, &probe)) return 0;
  const std::string name = shape.rfind("primitive:", 0) == 0 ? shape.substr(10) : shape;
  Entity3D e;
  e.name = name;
  e.name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(e.name[0])));
  e.transform.position = pos;
  if (name == "plane") e.transform.scale = {4, 1, 4};
  // Sit on the ground: lift by half the height.
  if (name == "cube" || name == "sphere") e.transform.position.y = std::max(pos.y, 0.5f);
  if (name == "capsule") e.transform.position.y = std::max(pos.y, 1.0f);
  e.mesh = scene3d::MeshRef{"primitive:" + name, true};
  e.material = scene3d::Material3D{};
  if (name != "plane") {
    scene3d::Collider3D c;
    if (name == "capsule") {
      c.shape = scene3d::Collider3D::Shape::Capsule;
      c.size = {0.4f, 2.0f, 0.4f};
    }
    e.collider = c;
  }
  return add_entity(std::move(e), "Create " + name);
}

std::uint64_t Editor3DScreen::create_light(bool directional, const Vec3& pos) {
  Entity3D e;
  e.name = directional ? "Sun" : "Lamp";
  e.transform.position = pos;
  scene3d::Light3D l;
  if (directional) {
    l.type = scene3d::Light3D::Type::Directional;
    l.color[0] = 1.0f;
    l.color[1] = 0.88f;
    l.color[2] = 0.7f;
    l.intensity = 2.6f;
    e.transform.rotation = render3d::Quat::from_euler_deg({-50.0f, 30.0f, 0.0f});
  }
  e.light = l;
  return add_entity(std::move(e), directional ? "Create sun" : "Create lamp");
}

std::uint64_t Editor3DScreen::create_empty(const Vec3& pos) {
  Entity3D e;
  e.name = "Empty";
  e.transform.position = pos;
  return add_entity(std::move(e), "Create empty");
}

std::uint64_t Editor3DScreen::drop_asset(const std::string& rel, const Vec3& pos,
                                         std::uint64_t target) {
  if (rel.rfind("primitive:", 0) == 0) return create_primitive(rel, pos);
  if (rel.rfind("prefab:", 0) == 0) return place_prefab(rel.substr(7), pos);
  std::string ext = fs::path(rel).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext == ".gltf" || ext == ".glb") {
    if (view_ && !view_->model(rel)) {
      collect_problems();
      note("Could not place " + rel + " (see above)", LogLevel::Error);
      return 0;
    }
    Entity3D e;
    e.name = fs::path(rel).stem().string();
    e.transform.position = pos;
    e.mesh = scene3d::MeshRef{rel, true};
    // Import settings from the .meta (Asset Browser).
    float scale = 1.0f;
    bool collider = true;
    if (const auto* a = asset_browser_.db().by_path(rel)) {
      scale = std::max(0.001f, a->meta.settings.flt("scale", 1.0f));
      collider = a->meta.settings.flag("generate_collider", true);
    }
    e.transform.scale = {scale, scale, scale};
    // Collider hugging the model's bounds.
    if (view_ && collider) {
      const render3d::Aabb b = view_->mesh_bounds(rel);
      scene3d::Collider3D c;
      c.size = b.extent();
      c.offset = b.center();
      e.collider = c;
    }
    return add_entity(std::move(e), "Place " + fs::path(rel).filename().string());
  }
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
    const Entity3D* t = scene_.find(target);
    if (!t) {
      note("Drop an image onto an entity to make it the albedo texture", LogLevel::Warn);
      return 0;
    }
    Entity3D e = *t;
    if (!e.material) e.material = scene3d::Material3D{};
    e.material->albedo_texture = rel;
    if (e.mesh && !e.mesh->is_primitive()) e.material->override_model = true;
    replace_entity(e, "Texture " + e.name);
    return e.id;
  }
  if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
    // A sound: onto an entity sets its Audio Source, elsewhere makes an
    // emitter. Bus / volume / loop come from the asset's import settings.
    AudioSourceData src = scene3d::audio_3d();
    src.clip = rel;
    if (const auto* a = asset_browser_.db().by_path(rel)) {
      src.bus = a->meta.settings.str("bus", src.bus);
      src.volume = a->meta.settings.flt("volume", src.volume);
      src.loop = a->meta.settings.flag("loop", src.loop);
    }
    if (const Entity3D* t = scene_.find(target)) {
      Entity3D e = *t;
      e.audio = src;
      replace_entity(e, "Sound " + e.name);
      return e.id;
    }
    Entity3D e;
    e.name = fs::path(rel).stem().string();
    e.transform.position = pos;
    e.audio = src;
    return add_entity(std::move(e), "Place " + fs::path(rel).filename().string());
  }
  note("Can't place " + rel + " (models: .gltf / .glb, sounds, or images onto an entity)",
       LogLevel::Warn);
  return 0;
}

std::size_t Editor3DScreen::duplicate_selected() {
  if (playing() || scene_.selection.empty()) return 0;
  const scene3d::Scene3D before = scene_;
  const std::size_t n = duplicate_roots(true);  // subtrees come along
  if (n == 0) return 0;
  push_undo("Duplicate", before);
  return n;
}

std::size_t Editor3DScreen::delete_selected() {
  if (playing() || scene_.selection.empty()) return 0;
  const scene3d::Scene3D before = scene_;
  std::vector<std::uint64_t> doomed;
  for (std::uint64_t id : selection_roots()) {
    doomed.push_back(id);
    for (std::uint64_t d : scene3d::descendants_of(scene_.entities, id)) doomed.push_back(d);
  }
  const std::size_t was = scene_.entities.size();
  scene_.entities.erase(
      std::remove_if(scene_.entities.begin(), scene_.entities.end(),
                     [&](const Entity3D& e) {
                       return std::find(doomed.begin(), doomed.end(), e.id) != doomed.end();
                     }),
      scene_.entities.end());
  const std::size_t n = was - scene_.entities.size();
  scene_.selection.clear();
  push_undo("Delete " + std::to_string(n), before);
  return n;
}

bool Editor3DScreen::set_transform(std::uint64_t id, const render3d::Transform& t,
                                   const std::string& label) {
  Entity3D* e = scene_.find(id);
  if (!e || playing() || e->transform == t) return false;
  const scene3d::Scene3D before = scene_;
  const render3d::Transform prev = e->transform;
  e->transform = t;
  carry_descendants(id, prev);  // children ride along
  push_undo(label + " " + scene_.find(id)->name, before);
  return true;
}

bool Editor3DScreen::replace_entity(const Entity3D& updated, const std::string& label) {
  Entity3D* e = scene_.find(updated.id);
  if (!e || playing() || *e == updated) return false;
  const scene3d::Scene3D before = scene_;
  *e = updated;
  push_undo(label, before);
  return true;
}

bool Editor3DScreen::set_environment(const render3d::Environment& env, const std::string& label) {
  if (playing() || scene_.environment == env) return false;
  const scene3d::Scene3D before = scene_;
  scene_.environment = env;
  push_undo(label, before);
  return true;
}

bool Editor3DScreen::rename(std::uint64_t id, const std::string& name) {
  const Entity3D* e = scene_.find(id);
  if (!e || name.empty()) return false;
  Entity3D copy = *e;
  copy.name = name;
  return replace_entity(copy, "Rename " + name);
}

void Editor3DScreen::set_snapping(bool on) {
  scene_.snap = on;
  autosave();
}

GizmoSnap Editor3DScreen::snap_steps() const {
  GizmoSnap s;
  s.translate = scene_.snap_translate;
  s.rotate = scene_.snap_rotate;
  s.scale = scene_.snap_scale;
  return s;
}

// ---- Camera + picking -------------------------------------------------------------

render3d::Camera3D Editor3DScreen::editor_camera() const {
  const scene3d::EditorCamera3D& c = scene_.editor_camera;
  render3d::Camera3D cam;
  const float yaw = render3d::radians(c.yaw), pitch = render3d::radians(c.pitch);
  cam.target = c.target;
  cam.position = c.target + Vec3{std::sin(yaw) * std::cos(pitch) * c.distance,
                                 std::sin(pitch) * c.distance,
                                 std::cos(yaw) * std::cos(pitch) * c.distance};
  cam.z_far = 2000.0f;
  return cam;
}

void Editor3DScreen::orbit(float dyaw, float dpitch) {
  scene_.editor_camera.yaw = std::fmod(scene_.editor_camera.yaw + dyaw, 360.0f);
  scene_.editor_camera.pitch = std::clamp(scene_.editor_camera.pitch + dpitch, -89.0f, 89.0f);
}

void Editor3DScreen::dolly(float factor) {
  scene_.editor_camera.distance = std::clamp(scene_.editor_camera.distance * factor, 0.5f, 500.0f);
}

void Editor3DScreen::pan(float dx, float dy, float vh) {
  const render3d::Camera3D cam = editor_camera();
  const Vec3 f = cam.forward();
  const Vec3 right = render3d::normalize(render3d::cross(f, {0, 1, 0}));
  const Vec3 up = render3d::cross(right, f);
  const float per_px = 2.0f * scene_.editor_camera.distance *
                       std::tan(render3d::radians(cam.fov_y_degrees) * 0.5f) / std::max(1.0f, vh);
  scene_.editor_camera.target += right * (-dx * per_px) + up * (dy * per_px);
}

void Editor3DScreen::frame_selection() {
  const Entity3D* e = scene_.find(primary());
  if (!e) return;
  scene_.editor_camera.target = e->transform.position;
  float size = 2.0f;
  if (view_) {
    const render3d::Aabb b = view_->local_bounds(*e).transformed(e->transform.matrix());
    size = std::max(1.0f, render3d::length(b.extent()));
    scene_.editor_camera.target = b.center();
  }
  scene_.editor_camera.distance = std::clamp(size * 1.8f, 2.0f, 200.0f);
}

std::uint64_t Editor3DScreen::pick(float px, float py, float vw, float vh) {
  const render3d::Ray ray = editor_camera().screen_ray(px, py, vw, vh);
  std::uint64_t best = 0;
  float best_t = 1e30f;
  for (const Entity3D& e : scene_.entities) {
    if (!e.visible || effectively_hidden(e) || effectively_locked(e)) continue;
    if (e.terrain) {
      // Terrains pick on the heightfield, so things standing on them win.
      std::shared_ptr<terrain::TerrainData> d = terrain::acquire(project_.path, *e.terrain);
      float t = 0.0f;
      if (d && terrain::raycast(*d, ray.origin - e.transform.position, ray.dir, 2000.0f, &t) && t < best_t) {
        best_t = t;
        best = e.id;
      }
      continue;
    }
    render3d::Aabb local = view_ ? view_->local_bounds(e) : render3d::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    float t = 0.0f;
    if (render3d::ray_obb(ray, local, e.transform.matrix(), &t) && t < best_t) {
      // Big ground slabs lose to anything standing on them.
      best_t = t;
      best = e.id;
    }
  }
  return best;
}

bool Editor3DScreen::ground_point(float px, float py, float vw, float vh, Vec3* out) const {
  const render3d::Ray ray = editor_camera().screen_ray(px, py, vw, vh);
  float t = 0.0f;
  std::uint64_t tid = 0;
  if (terrain_ray(ray, &t, &tid)) {
    *out = ray.origin + ray.dir * t;
    return true;
  }
  if (!render3d::ray_plane(ray, {0, 1, 0}, 0.0f, &t) || t > 1000.0f) return false;
  *out = ray.origin + ray.dir * t;
  return true;
}

// ---- Gizmo -----------------------------------------------------------------------

bool Editor3DScreen::gizmo_begin(float px, float py, float vw, float vh) {
  const Entity3D* e = scene_.find(primary());
  if (!e || playing() || effectively_locked(*e)) return false;
  const int axis = gizmo_.hit(editor_camera(), vw, vh, e->transform.position, px, py);
  if (axis < 0) return false;
  return gizmo_begin_axis(axis, px, py, vw, vh);
}

bool Editor3DScreen::gizmo_begin_axis(int axis, float px, float py, float vw, float vh) {
  const Entity3D* e = scene_.find(primary());
  if (!e || playing() || axis < 0 || axis > 2) return false;
  gizmo_before_ = scene_;
  gizmo_.begin(axis, e->transform, editor_camera(), vw, vh, px, py);
  return true;
}

void Editor3DScreen::gizmo_move(float px, float py, float vw, float vh) {
  if (!gizmo_.active()) return;
  Entity3D* e = scene_.find(primary());
  if (!e) return;
  const GizmoSnap steps = snap_steps();
  const render3d::Transform prev = e->transform;
  e->transform = gizmo_.drag(editor_camera(), vw, vh, px, py, scene_.snap ? &steps : nullptr);
  if (surface_snap_ && gizmo_.mode == GizmoMode::Translate) {
    // Stick the bounds' bottom onto whatever is under the mouse.
    std::vector<std::uint64_t> skip{e->id};
    for (std::uint64_t d : scene3d::descendants_of(scene_.entities, e->id)) skip.push_back(d);
    Vec3 hit;
    if (surface_point(px, py, vw, vh, skip, &hit)) {
      const render3d::Aabb b = world_bounds(*e);
      const Vec3 foot{b.center().x, b.min.y, b.center().z};
      e->transform.position += hit - foot;
    }
    e = scene_.find(primary());
  }
  carry_descendants(e->id, prev);
}

void Editor3DScreen::gizmo_end() {
  if (!gizmo_.active() || !gizmo_before_) return;
  const Entity3D* e = scene_.find(primary());
  const Entity3D* was = gizmo_before_->find(primary());
  gizmo_.cancel();
  if (dup_drag_) {
    // The copies stay even when they did not move.
    dup_drag_ = false;
    push_undo("Duplicate", *gizmo_before_);
  } else if (e && was && !(e->transform == was->transform)) {
    const char* what = gizmo_.mode == GizmoMode::Translate
                           ? "Move"
                           : (gizmo_.mode == GizmoMode::Rotate ? "Rotate" : "Scale");
    push_undo(std::string(what) + " " + e->name, *gizmo_before_);
  }
  gizmo_before_.reset();
}

void Editor3DScreen::gizmo_cancel() {
  if (gizmo_.active() && gizmo_before_) scene_ = *gizmo_before_;  // copies go too
  dup_drag_ = false;
  gizmo_.cancel();
  gizmo_before_.reset();
}

// ---- Rendering + play --------------------------------------------------------------

render3d::FrameStats Editor3DScreen::render_frame(int width, int height) {
  ensure_renderer();
  if (!view_) return {};
  render3d::SceneViewOptions opt;
  render3d::Camera3D cam;
  if (playing()) {
    cam = free_camera_in_play_ ? editor_camera() : world_.camera();
    opt.colliders = world_.authored().physics.debug_draw;  // Physics Settings
    if (world_.vr()) {
      // VR preview: the head's view plus the hands, lasers and menu.
      std::vector<scene3d::Entity3D> ents = world_.entities();
      world3d::vr_overlay_entities(world_, false, &ents);
      view_->begin(world_.environment(), ents, cam, width, height, opt);
      world3d::vr_overlay_lines(world_, renderer_.get());
    } else {
      view_->begin(world_.environment(), world_.entities(), cam, width, height, opt);
    }
    std::vector<render3d::Renderer3D::Billboard> bills;
    world_.billboards(&bills);
    view_->billboards(bills);
    // ts.ai debug (navmesh, paths, BT state, sight cones) when ai.debug(true)
    // or the BT panel's Debug toggle is on.
    std::vector<world3d::World3DExtension::Line> lines;
    world_.debug_lines(&lines);
    for (const auto& l : lines) renderer_->draw_line(l.a, l.b, l.rgba, false);
  } else {
    cam = editor_camera();
    opt.grid = scene_.show_grid;
    opt.colliders = show_colliders_;
    opt.light_gizmos = true;
    opt.highlight = scene_.selection;
    const bool any_hidden = std::any_of(scene_.entities.begin(), scene_.entities.end(),
                                        [](const Entity3D& e) { return e.editor_hidden; });
    const bool posing = anim_preview_.open && find(primary()) != nullptr;
    if (any_hidden || posing) {
      // Eye toggled off in the hierarchy: not drawn in the editor only.
      // The animation preview poses the selection in this copy only.
      std::vector<Entity3D> shown;
      for (const Entity3D& e : scene_.entities) {
        if (effectively_hidden(e)) continue;
        shown.push_back(e);
        if (posing && e.id == primary()) anim_preview_pose(&shown.back());
      }
      view_->begin(scene_.environment, shown, cam, width, height, opt);
    } else {
      view_->begin(scene_.environment, scene_.entities, cam, width, height, opt);
    }
  }
  last_stats_ = view_->end();
  collect_problems();
  return last_stats_;
}

bool Editor3DScreen::play_start() {
  if (playing()) return true;
  if (vr_project()) return vr_preview_start(true);
  world_.set_vr(false);
  gizmo_cancel();
  world_.set_log_sink([this](const runtime::LogEntry& e) {
    log_.push_back(e);
    while (log_.size() > 400) log_.pop_front();
  });
  std::string err;
  // ts.ai: navmesh agents ride along when the project enables the plugin.
  world_.clear_extensions();
  ai3d::attach(world_, plugins_.loaded("ts.ai"));
  // ts.quests / ts.dialogue / ts.saves: the story runtime (UI, hooks, Lua).
  story::StoryFeatures story;
  story.quests = plugins_.loaded("ts.quests");
  story.dialogue = plugins_.loaded("ts.dialogue");
  story.saves = plugins_.loaded("ts.saves");
  story3d::attach(world_, story);
  // ts.weather: sky, fog, wind and rain / snow / dust while riding.
  weather::attach(world_, plugins_.loaded("ts.weather"));
  // ts.profiler: record while riding, profiler / console Lua tables.
  debug_tools_on_ = plugins_.loaded("ts.profiler");
  if (debug_tools_on_) {
    profiler::attach_profiler(world_);
    profiler::Profiler::get().set_capture_dir(plugins::capture_dir_for(project_.path));
  }
  if (!world_.start(scene_, project_.path, &err)) {
    note("Play failed: " + err, LogLevel::Error);
    return false;
  }
  play_ui_.load(project_.path);
  if (debug_tools_on_) debug_tools_.attach(play_ui_.system());
  note("Riding \"" + project_.name + "\" (Stop puts everything back)");
  return true;
}

void Editor3DScreen::play_stop() {
  if (vr_session_) {
    vr_session_->end();
    vr_session_.reset();
    vr_presenter_.reset();
    vr_status_.clear();
  }
  end_sound();
  if (!playing()) return;
  world_.stop();
  toasts_.clear();
  if (debug_tools_.attached()) debug_tools_.detach();
  debug_tools_on_ = false;
  play_ui_.clear();
  note("Reined in; the scene is as you left it");
  if (plugins_.pending()) plugins_.apply();
}

// ---- Assets ------------------------------------------------------------------------

void Editor3DScreen::update_sound(double dt) {
  if (!playing()) {
    end_sound();
    return;
  }
  if (!sound_stage_) {
    sound_stage_ = std::make_unique<audio::SoundStage>(EditorAudio::get().engine());
    sound_stage_->begin_ride(project_.path, true);
    sound3d_ = std::make_unique<audio::SoundStage3D>(*sound_stage_);
  }
  // Ears on the camera, or the headset in VR.
  sound3d_->update(world_, dt);
}

void Editor3DScreen::end_sound() {
  if (!sound_stage_) return;
  sound3d_.reset();
  sound_stage_->end_ride();
  sound_stage_.reset();
}

void Editor3DScreen::rescan_assets() {
  assets_.clear();
  scripts_.clear();
  sounds_ = assets::list_sounds(project_.path);
  for (const std::string& p : render3d::primitive_names()) {
    AssetEntry a;
    a.rel = "primitive:" + p;
    a.label = p;
    a.model = true;
    assets_.push_back(a);
  }
  std::error_code ec;
  const fs::path root(project_.path);
  if (!fs::is_directory(root, ec)) return;
  std::vector<AssetEntry> files;
  for (auto it = fs::recursive_directory_iterator(root, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 4 || !it->is_regular_file(ec)) continue;
    std::string ext = it->path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string rel = fs::relative(it->path(), root, ec).generic_string();
    if (ext == ".lua") {
      scripts_.push_back(rel);
      continue;
    }
    AssetEntry a;
    a.rel = rel;
    a.label = rel;
    a.model = ext == ".gltf" || ext == ".glb";
    a.image = ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
    if (a.model || a.image) files.push_back(a);
  }
  std::sort(files.begin(), files.end(),
            [](const AssetEntry& a, const AssetEntry& b) { return a.rel < b.rel; });
  std::sort(scripts_.begin(), scripts_.end());
  assets_.insert(assets_.end(), files.begin(), files.end());
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
