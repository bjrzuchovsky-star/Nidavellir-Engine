#include "render3d/SceneView.h"

#include <cstdio>
#include <algorithm>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;

SceneView::~SceneView() { clear(); }

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
  if (!e.mesh) return {{-0.25f, -0.25f, -0.25f}, {0.25f, 0.25f, 0.25f}};
  return mesh_bounds(e.mesh->source);
}

void SceneView::upload(ModelEntry& m) {
  if (m.uploaded || !m.model || !r_) return;
  m.uploaded = true;
  for (const ModelPrimitive& p : m.model->primitives) m.meshes.push_back(r_->create_mesh(p.mesh));
  for (const MaterialDesc& d : m.model->materials) m.materials.push_back(r_->create_material(d));
  m.materials.push_back(r_->create_material(MaterialDesc{}));
}

std::shared_ptr<const Image> SceneView::image(const std::string& rel) {
  if (rel.empty()) return nullptr;
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
  const std::string k = std::string(key) + m.albedo_texture + "|" + m.normal_texture + "|" +
                        m.metal_rough_texture;
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

void SceneView::begin(const Environment& env, const std::vector<scene3d::Entity3D>& entities,
                      const Camera3D& camera, int width, int height,
                      const SceneViewOptions& opt) {
  if (!r_) return;
  r_->set_environment(env);
  r_->begin_frame(camera, width, height);
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
      r_->draw_matrix(m.meshes[i], mat, world, e.mesh->cast_shadows);
    }
  }
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
    if (opt.colliders && e.collider) {
      const float col[4] = {0.35f, 0.95f, 0.45f, e.collider->trigger ? 0.5f : 0.9f};
      Aabb b;
      if (e.collider->shape == scene3d::Collider3D::Shape::Capsule) {
        const float r = e.collider->size.x, h = e.collider->size.y;
        b = {{-r, -h * 0.5f, -r}, {r, h * 0.5f, r}};
      } else {
        b = {e.collider->size * -0.5f, e.collider->size * 0.5f};
      }
      b.min += e.collider->offset;
      b.max += e.collider->offset;
      draw_box(world, b, col, false);
    }
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
  return r_ ? r_->end_frame() : FrameStats{};
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
