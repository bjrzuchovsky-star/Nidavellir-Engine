#include "editor/editor3d/Editor3DScreen.h"

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
    scene_ = scene3d::make_starter_scene();
    if (!scene3d::save_file(path, scene_, &err)) {
      note("Could not write the starter scene: " + err, LogLevel::Error);
    } else {
      note("Staked a fresh 3D claim: ground, crate, lamp and a rider");
    }
  } else if (!scene3d::load_file(path, &scene_, &err)) {
    note(err, LogLevel::Error);
    scene_ = scene3d::Scene3D{};
  }
  undo_.clear();
  redo_.clear();
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
  if (playing()) {
    world_.update(delta_seconds, ImGui::GetCurrentContext() ? poll_play_input()
                                                            : world3d::Input3D{});
    for (const world3d::Toast3D& t : world_.take_toasts()) toasts_.push_back({t.text, t.seconds});
  }
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
  undo_.push_back({label, before});
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
  Step s = std::move(undo_.back());
  undo_.pop_back();
  redo_.push_back({s.label, scene_});
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
  undo_.push_back({s.label, scene_});
  scene_ = std::move(s.scene);
  autosave();
  note("Redo " + undo_.back().label);
  return true;
}

bool Editor3DScreen::save_scene(std::string* error) {
  return scene3d::save_file(scene3d::scene_path(project_.path), scene_, error);
}

bool Editor3DScreen::reload_scene(std::string* error) {
  scene3d::Scene3D s;
  if (!scene3d::load_file(scene3d::scene_path(project_.path), &s, error)) return false;
  scene_ = std::move(s);
  undo_.clear();
  redo_.clear();
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
    // Collider hugging the model's bounds.
    if (view_) {
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
  note("Can't place " + rel + " (models: .gltf / .glb)", LogLevel::Warn);
  return 0;
}

std::size_t Editor3DScreen::duplicate_selected() {
  if (playing() || scene_.selection.empty()) return 0;
  const scene3d::Scene3D before = scene_;
  std::vector<std::uint64_t> fresh;
  for (std::uint64_t id : before.selection) {
    const Entity3D* src = before.find(id);
    if (!src) continue;
    Entity3D e = *src;
    e.id = scene_.next_id();
    e.transform.position.x += 1.0f;
    std::string name = src->name + " copy";
    for (int n = 2; scene_.find_named(name); ++n) name = src->name + " copy " + std::to_string(n);
    e.name = name;
    if (e.player) e.player.reset();  // one rider
    scene_.entities.push_back(e);
    fresh.push_back(e.id);
  }
  scene_.selection = fresh;
  push_undo("Duplicate", before);
  return fresh.size();
}

std::size_t Editor3DScreen::delete_selected() {
  if (playing() || scene_.selection.empty()) return 0;
  const scene3d::Scene3D before = scene_;
  std::size_t n = 0;
  for (std::uint64_t id : before.selection) {
    auto it = std::find_if(scene_.entities.begin(), scene_.entities.end(),
                           [&](const Entity3D& e) { return e.id == id; });
    if (it != scene_.entities.end()) {
      scene_.entities.erase(it);
      ++n;
    }
  }
  scene_.selection.clear();
  push_undo("Delete " + std::to_string(n), before);
  return n;
}

bool Editor3DScreen::set_transform(std::uint64_t id, const render3d::Transform& t,
                                   const std::string& label) {
  Entity3D* e = scene_.find(id);
  if (!e || playing() || e->transform == t) return false;
  const scene3d::Scene3D before = scene_;
  e->transform = t;
  push_undo(label + " " + e->name, before);
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
    if (!e.visible) continue;
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
  if (!render3d::ray_plane(ray, {0, 1, 0}, 0.0f, &t) || t > 1000.0f) return false;
  *out = ray.origin + ray.dir * t;
  return true;
}

// ---- Gizmo -----------------------------------------------------------------------

bool Editor3DScreen::gizmo_begin(float px, float py, float vw, float vh) {
  const Entity3D* e = scene_.find(primary());
  if (!e || playing()) return false;
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
  e->transform = gizmo_.drag(editor_camera(), vw, vh, px, py, scene_.snap ? &steps : nullptr);
}

void Editor3DScreen::gizmo_end() {
  if (!gizmo_.active() || !gizmo_before_) return;
  const Entity3D* e = scene_.find(primary());
  const Entity3D* was = gizmo_before_->find(primary());
  gizmo_.cancel();
  if (e && was && !(e->transform == was->transform)) {
    const char* what = gizmo_.mode == GizmoMode::Translate
                           ? "Move"
                           : (gizmo_.mode == GizmoMode::Rotate ? "Rotate" : "Scale");
    push_undo(std::string(what) + " " + e->name, *gizmo_before_);
  }
  gizmo_before_.reset();
}

void Editor3DScreen::gizmo_cancel() {
  if (gizmo_.active() && gizmo_before_) scene_ = *gizmo_before_;
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
    view_->begin(world_.authored().environment, world_.entities(), cam, width, height, opt);
  } else {
    cam = editor_camera();
    opt.grid = scene_.show_grid;
    opt.colliders = show_colliders_;
    opt.light_gizmos = true;
    opt.highlight = scene_.selection;
    view_->begin(scene_.environment, scene_.entities, cam, width, height, opt);
  }
  last_stats_ = view_->end();
  collect_problems();
  return last_stats_;
}

bool Editor3DScreen::play_start() {
  if (playing()) return true;
  gizmo_cancel();
  world_.set_log_sink([this](const runtime::LogEntry& e) {
    log_.push_back(e);
    while (log_.size() > 400) log_.pop_front();
  });
  std::string err;
  if (!world_.start(scene_, project_.path, &err)) {
    note("Play failed: " + err, LogLevel::Error);
    return false;
  }
  note("Riding \"" + project_.name + "\" (Stop puts everything back)");
  return true;
}

void Editor3DScreen::play_stop() {
  if (!playing()) return;
  world_.stop();
  toasts_.clear();
  note("Reined in; the scene is as you left it");
  if (plugins_.pending()) plugins_.apply();
}

// ---- Assets ------------------------------------------------------------------------

void Editor3DScreen::rescan_assets() {
  assets_.clear();
  scripts_.clear();
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
