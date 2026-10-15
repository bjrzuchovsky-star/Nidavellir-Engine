#include "render3d/SceneView.h"
#include "profiler/Profiler.h"

#include "core/MemImages.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <iterator>

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;

SceneView::SceneView(Renderer3D* renderer) : r_(renderer) {
  terrain_ = std::make_unique<terrain::TerrainView>(renderer);
  terrain_->set_sources([this](const std::string& rel) { return image(rel); },
                        [this](const std::string& src) { return model(src); });
}

SceneView::~SceneView() {
  clear();
  terrain_.reset();
}

float SceneView::time() const {
  if (fixed_time_) return time_;
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

void SceneView::set_project_dir(const std::string& dir) {
  if (dir == dir_) return;
  clear();
  dir_ = dir;
}

void SceneView::clear() {
  if (r_) {
    for (auto& [k, m] : models_) {
      for (MeshHandle h : m.meshes) r_->destroy_mesh(h);
      for (MaterialHandle h : m.materials) r_->destroy_material(h);
    }
    for (auto& [k, m] : materials_) r_->destroy_material(m.handle);
  }
  if (terrain_) terrain_->clear();
  models_.clear();
  materials_.clear();
  images_.clear();
  reported_.clear();
}

void SceneView::problem(const std::string& key, const std::string& text) {
  if (reported_[key]) return;
  reported_[key] = true;
  problems_.push_back(text);
}

std::vector<std::string> SceneView::take_problems() {
  std::vector<std::string> out;
  out.swap(problems_);
  return out;
}

SceneView::ModelEntry& SceneView::entry(const std::string& source) {
  ModelEntry& m = models_[source];
  if (m.tried) return m;
  m.tried = true;
  auto model = std::make_unique<Model>();
  MeshData prim;
  if (make_primitive(source, &prim)) {
    model->source = source;
    model->bounds = prim.bounds();
    ModelPrimitive p;
    p.mesh = std::move(prim);
    model->primitives.push_back(std::move(p));
    m.model = std::move(model);
    return m;
  }
  if (source.empty()) return m;
  const std::string path = (fs::path(dir_) / fs::path(source)).string();
  std::string err;
  if (!load_gltf(path, model.get(), &err)) {
    problem("model:" + source, "3D model " + source + " would not load: " + err);
    return m;
  }
  if (!err.empty()) problem("model-warn:" + source, source + ": " + err);
  m.model = std::move(model);
  return m;
}

const Model* SceneView::model(const std::string& source) {
  return entry(source).model.get();
}

Aabb SceneView::mesh_bounds(const std::string& source) {
  const Model* m = model(source);
  if (!m) return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  Aabb b = m->bounds;
  // Planes are flat; give them a sliver so rays and boxes find them.
  if (b.max.y - b.min.y < 0.02f) {
    b.min.y -= 0.01f;
    b.max.y += 0.01f;
  }
  return b;
}

Aabb SceneView::local_bounds(const scene3d::Entity3D& e) {
  if (e.terrain) {
    // Terrain: its heightfield box (cached per data revision).
    std::shared_ptr<terrain::TerrainData> d = terrain::acquire(dir_, *e.terrain);
    if (d && d->valid()) {
      auto& slot = terrain_bounds_[d.get()];
      if (slot.first != d->revision + 1) {
        float lo = 1e30f, hi = -1e30f;
        for (float h : d->heights) {
          lo = std::min(lo, h);
          hi = std::max(hi, h);
        }
        if (hi - lo < 0.02f) { lo -= 0.01f; hi += 0.01f; }
        slot = {d->revision + 1, Aabb{{-d->size * 0.5f, lo, -d->size * 0.5f}, {d->size * 0.5f, hi, d->size * 0.5f}}};
      }
      return slot.second;
    }
  }
  if (!e.mesh) return {{-0.25f, -0.25f, -0.25f}, {0.25f, 0.25f, 0.25f}};
  return mesh_bounds(e.mesh->source);
}

void SceneView::upload(ModelEntry& m) {
  if (m.uploaded || !m.model || !r_) return;
  m.uploaded = true;
  if (m.model->skinned()) skin_palette(m.model->skeleton, rest_pose(m.model->skeleton), &m.rest_palette);
  for (const ModelPrimitive& p : m.model->primitives) m.meshes.push_back(r_->create_mesh(p.mesh));
  for (const MaterialDesc& d : m.model->materials) m.materials.push_back(r_->create_material(d));
  m.materials.push_back(r_->create_material(MaterialDesc{}));
}

std::shared_ptr<const Image> SceneView::image(const std::string& rel) {
  if (rel.empty()) return nullptr;
  if (mem_images::is_key(rel)) {
    // In-memory raster (the VR UI panel): cached per version.
    const mem_images::MemImage* m = mem_images::get(rel);
    if (!m) return nullptr;
    const std::string k = rel + "#v" + std::to_string(m->version);
    auto mit = images_.find(k);
    if (mit != images_.end()) return mit->second;
    for (auto i = images_.begin(); i != images_.end();) {
      i = i->first.rfind(rel + "#v", 0) == 0 ? images_.erase(i) : std::next(i);
    }
    auto img = std::make_shared<Image>();
    img->width = m->width;
    img->height = m->height;
    img->rgba = m->rgba;
    images_[k] = img;
    return img;
  }
  auto it = images_.find(rel);
  if (it != images_.end()) return it->second;
  auto img = std::make_shared<Image>();
  std::string err;
  if (!load_image_file((fs::path(dir_) / fs::path(rel)).string(), img.get(), &err)) {
    problem("tex:" + rel, "Texture " + rel + " would not load: " + err);
    img.reset();
  }
  images_[rel] = img;
  return img;
}

MaterialHandle SceneView::material_for(const scene3d::Material3D& m) {
  char key[512];
  std::snprintf(key, sizeof(key), "%g,%g,%g,%g|%g|%g|%g,%g,%g|", m.albedo[0], m.albedo[1],
                m.albedo[2], m.albedo[3], m.metallic, m.roughness, m.emissive[0],
                m.emissive[1], m.emissive[2]);
  std::string k = std::string(key) + m.albedo_texture + "|" + m.normal_texture + "|" +
                  m.metal_rough_texture + (m.unlit ? "|unlit" : "");
  if (mem_images::is_key(m.albedo_texture)) {
    // A new raster version replaces the old material (and its texture).
    const mem_images::MemImage* mi = mem_images::get(m.albedo_texture);
    const std::string tag = m.albedo_texture + "#v";
    k += "#v" + std::to_string(mi ? mi->version : 0);
    if (materials_.find(k) == materials_.end()) {
      for (auto i = materials_.begin(); i != materials_.end();) {
        if (i->first.find(tag) != std::string::npos) {
          if (r_) r_->destroy_material(i->second.handle);
          i = materials_.erase(i);
        } else {
          ++i;
        }
      }
    }
  }
  auto it = materials_.find(k);
  if (it != materials_.end()) return it->second.handle;
  MaterialDesc d;
  for (int i = 0; i < 4; ++i) d.base_color[i] = m.albedo[i];
  d.metallic = m.metallic;
  d.roughness = m.roughness;
  for (int i = 0; i < 3; ++i) d.emissive[i] = m.emissive[i];
  d.base_color_texture = m.albedo_texture;
  d.normal_texture = m.normal_texture;
  d.metal_rough_texture = m.metal_rough_texture;
  d.unlit = m.unlit;
  d.base_color_image = image(m.albedo_texture);
  d.normal_image = image(m.normal_texture);
  d.metal_rough_image = image(m.metal_rough_texture);
  MaterialEntry e;
  e.handle = r_ ? r_->create_material(d) : MaterialHandle{};
  materials_[k] = e;
  // Keep the cache from growing without bound while sliders drag.
  if (materials_.size() > 512 && r_) {
    for (auto& [kk, mm] : materials_) r_->destroy_material(mm.handle);
    materials_.clear();
    return material_for(m);
  }
  return e.handle;
}

void SceneView::draw_box(const Mat4& world, const Aabb& b, const float* rgba, bool on_top) {
  if (!r_) return;
  Vec3 c[8];
  for (int i = 0; i < 8; ++i) {
    c[i] = world.transform_point({(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y,
                                  (i & 4) ? b.max.z : b.min.z});
  }
  const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                            {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& e : edges) r_->draw_line(c[e[0]], c[e[1]], rgba, !on_top);
}

void SceneView::draw_circle(const Mat4& world, const Vec3& c, float r, int plane, const float* rgba) {
  constexpr int kSeg = 20;
  Vec3 prev;
  for (int i = 0; i <= kSeg; ++i) {
    const float a = 6.2831853f * static_cast<float>(i) / kSeg;
    const float u = std::cos(a) * r, v = std::sin(a) * r;
    const Vec3 p = plane == 0 ? Vec3{c.x + u, c.y, c.z + v}    // XZ
                   : plane == 1 ? Vec3{c.x + u, c.y + v, c.z}  // XY
                                : Vec3{c.x, c.y + u, c.z + v}; // YZ
    const Vec3 w = world.transform_point(p);
    if (i > 0) r_->draw_line(prev, w, rgba, true);
    prev = w;
  }
}

void SceneView::draw_collider(const Mat4& world, const scene3d::Entity3D& e) {
  if (!r_) return;
  const scene3d::Collider3D& c = *e.collider;
  float col[4] = {0.35f, 0.95f, 0.45f, 0.9f};  // static: green
  if (e.rigid_body) {
    using M = scene3d::RigidBody3D::Motion;
    if (e.rigid_body->motion == M::Dynamic) {
      col[0] = 1.0f, col[1] = 0.62f, col[2] = 0.2f;  // dynamic: orange
    } else if (e.rigid_body->motion == M::Kinematic) {
      col[0] = 0.4f, col[1] = 0.62f, col[2] = 1.0f;  // kinematic: blue
    }
  }
  if (c.trigger) col[0] = 1.0f, col[1] = 0.85f, col[2] = 0.3f, col[3] = 0.55f;
  auto prim = [&](const Mat4& m, scene3d::ColliderShape3D shape, const Vec3& size) {
    using S = scene3d::ColliderShape3D;
    switch (shape) {
      case S::Sphere:
        for (int p = 0; p < 3; ++p) draw_circle(m, {}, size.x, p, col);
        break;
      case S::Capsule: {
        const float r = size.x, half = std::max(0.0f, size.y * 0.5f - r);
        for (float y : {-half, half}) {
          draw_circle(m, {0, y, 0}, r, 0, col);
          draw_circle(m, {0, y, 0}, r, 1, col);
          draw_circle(m, {0, y, 0}, r, 2, col);
        }
        for (const Vec3& d : {Vec3{r, 0, 0}, Vec3{-r, 0, 0}, Vec3{0, 0, r}, Vec3{0, 0, -r}}) {
          r_->draw_line(m.transform_point(d + Vec3{0, -half, 0}), m.transform_point(d + Vec3{0, half, 0}), col, true);
        }
        break;
      }
      case S::ConvexHull:
      case S::Mesh: {
        // The model's bounds stand in for the hull / triangles.
        const float hull[4] = {0.8f, 0.5f, 1.0f, 0.85f};
        draw_box(m, local_bounds(e), hull, false);
        break;
      }
      default:
        draw_box(m, {size * -0.5f, size * 0.5f}, col, false);
        break;
    }
  };
  Transform t;
  t.position = c.offset;
  prim(world * t.matrix(), c.shape, c.size);
  for (const scene3d::ColliderPart3D& p : c.parts) {
    Transform pt;
    pt.position = p.offset;
    pt.rotation = Quat::from_euler_deg(p.rotation_deg);
    prim(world * pt.matrix(), p.shape, p.size);
  }
}

void SceneView::draw_joint(const Mat4& world, const scene3d::Entity3D& e,
                           const std::vector<scene3d::Entity3D>& entities) {
  if (!r_) return;
  const scene3d::Joint3D& j = *e.joint;
  const float col[4] = {1.0f, 0.35f, 0.85f, 0.95f};
  const Vec3 a = world.transform_point(j.anchor);
  Vec3 to = a;
  for (const scene3d::Entity3D& o : entities) {
    if (j.target && o.id == j.target) to = o.transform.position;
  }
  if (j.target) {
    r_->draw_line(a, to, col, true);
  } else {
    const float s = 0.12f;  // pinned to the world: a small cross
    r_->draw_line(a - Vec3{s, 0, 0}, a + Vec3{s, 0, 0}, col, true);
    r_->draw_line(a - Vec3{0, s, 0}, a + Vec3{0, s, 0}, col, true);
    r_->draw_line(a - Vec3{0, 0, s}, a + Vec3{0, 0, s}, col, true);
  }
  using T = scene3d::Joint3D::Type;
  if (j.type == T::Hinge || j.type == T::Slider) {
    const Vec3 ax = e.transform.rotation.rotate(normalize(j.axis)) * 0.4f;
    r_->draw_line(a - ax, a + ax, col, true);
  }
}

void SceneView::begin(const Environment& env, const std::vector<scene3d::Entity3D>& entities,
                      const Camera3D& camera, int width, int height,
                      const SceneViewOptions& opt) {
  if (!r_) return;
  r_->set_environment(env);
  r_->begin_frame(camera, width, height);
  terrain_->reset_stats();
  // Nearest point lights first when there are more than the shader takes.
  std::vector<std::pair<float, PointLight>> points;
  for (const scene3d::Entity3D& e : entities) {
    if (!e.visible || !e.light || e.light->type != scene3d::Light3D::Type::Point) continue;
    PointLight l;
    l.position = e.transform.position;
    // glTF lanterns: the light sits at the glass when the model has one.
    if (e.mesh && !e.mesh->is_primitive()) {
      const Aabb b = mesh_bounds(e.mesh->source);
      l.position = e.transform.matrix().transform_point({b.min.x + 0.1f, b.max.y - 0.35f, 0.0f});
    }
    for (int i = 0; i < 3; ++i) l.color[i] = e.light->color[i];
    l.intensity = e.light->intensity;
    l.range = e.light->range;
    points.push_back({length(l.position - camera.target), l});
  }
  std::sort(points.begin(), points.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [d, l] : points) r_->add_point_light(l);
  for (const scene3d::Entity3D& e : entities) {
    if (!e.visible) continue;
    if (e.light && e.light->type == scene3d::Light3D::Type::Directional) {
      // A directional light entity steers the sun from its rotation.
      Environment env2 = env;
      env2.sun_direction = e.transform.rotation.rotate({0, 0, -1});
      for (int i = 0; i < 3; ++i) env2.sun_color[i] = e.light->color[i];
      env2.sun_intensity = e.light->intensity;
      r_->set_environment(env2);
    }
    if (e.terrain) {
      terrain_->draw(e.id, *e.terrain, dir_, e.transform.position, camera.position, time());
    }
    if (!e.mesh) continue;
    ModelEntry& m = entry(e.mesh->source);
    if (!m.model) continue;
    upload(m);
    const Mat4 world = e.transform.matrix();
    const bool own_material = e.material && (e.mesh->is_primitive() || e.material->override_model);
    const MaterialHandle override_mat = own_material ? material_for(*e.material) : MaterialHandle{};
    for (std::size_t i = 0; i < m.meshes.size(); ++i) {
      MaterialHandle mat = override_mat;
      if (!own_material) {
        const int mi = m.model->primitives[i].material;
        mat = (mi >= 0 && static_cast<std::size_t>(mi) + 1 < m.materials.size())
                  ? m.materials[static_cast<std::size_t>(mi)]
                  : m.materials.back();
      }
      if (m.model->primitives[i].mesh.skinned()) {
        // Posed by the animator this tick, else the rest pose.
        const std::vector<Mat4>& pal =
            (e.skin_palette && !e.skin_palette->empty()) ? *e.skin_palette : m.rest_palette;
        r_->draw_skinned(m.meshes[i], mat, world, pal.data(), pal.size(), e.mesh->cast_shadows);
      } else {
        r_->draw_matrix(m.meshes[i], mat, world, e.mesh->cast_shadows);
      }
    }
  }
  terrain_->prune();
  if (opt.grid) {
    const float ext = opt.grid_extent, step = opt.grid_step > 0 ? opt.grid_step : 1.0f;
    const int n = static_cast<int>(ext / step);
    for (int i = -n; i <= n; ++i) {
      const float t = i * step;
      const bool axis = i == 0;
      const bool major = (i % 5) == 0;
      const float col[4] = {0.85f, 0.72f, 0.55f, axis ? 0.0f : (major ? 0.32f : 0.14f)};
      if (!axis) {
        r_->draw_line({t, 0.002f, -ext}, {t, 0.002f, ext}, col, true);
        r_->draw_line({-ext, 0.002f, t}, {ext, 0.002f, t}, col, true);
      }
    }
    const float xr[4] = {0.86f, 0.30f, 0.25f, 0.8f}, zb[4] = {0.30f, 0.50f, 0.90f, 0.8f};
    r_->draw_line({-ext, 0.003f, 0}, {ext, 0.003f, 0}, xr, true);
    r_->draw_line({0, 0.003f, -ext}, {0, 0.003f, ext}, zb, true);
  }
  for (const scene3d::Entity3D& e : entities) {
    const Mat4 world = e.transform.matrix();
    const bool selected = std::find(opt.highlight.begin(), opt.highlight.end(), e.id) != opt.highlight.end();
    if (selected) {
      const float sel[4] = {1.0f, 0.72f, 0.25f, 1.0f};
      draw_box(world, local_bounds(e), sel, true);
    }
    if (opt.colliders && e.collider) draw_collider(world, e);
    if (opt.colliders && e.joint) draw_joint(world, e, entities);
    if (opt.light_gizmos && e.light) {
      const float col[4] = {1.0f, 0.85f, 0.35f, 0.9f};
      const Vec3 p = e.transform.position;
      const float s = 0.3f;
      r_->draw_line(p - Vec3{s, 0, 0}, p + Vec3{s, 0, 0}, col, false);
      r_->draw_line(p - Vec3{0, s, 0}, p + Vec3{0, s, 0}, col, false);
      r_->draw_line(p - Vec3{0, 0, s}, p + Vec3{0, 0, s}, col, false);
      if (e.light->type == scene3d::Light3D::Type::Directional) {
        r_->draw_line(p, p + e.transform.rotation.rotate({0, 0, -1}) * 1.5f, col, false);
      }
    }
  }
}

FrameStats SceneView::end() {
  TS_PROFILE_SCOPE("render3d.frame");
  const FrameStats st = r_ ? r_->end_frame() : FrameStats{};
  report_frame_stats(st);
  return st;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
