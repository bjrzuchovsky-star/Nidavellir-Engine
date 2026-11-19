#include "render3d/SceneView.h"
#include "render3d/ClipFile.h"
#include "render3d/Retarget.h"
#include "render3d/HullFit.h"
#include "render3d/ImportLimits.h"
#include "render3d/ModelImport.h"
#include "render3d/UploadStage.h"
#include "profiler/Profiler.h"

#include "core/MemImages.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <unordered_map>

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

void SceneView::stop_load(ModelEntry& m) {
  if (m.pending) m.pending->cancel.store(true);
  if (m.loader.joinable()) m.loader.join();
  m.pending.reset();
  m.loading = false;
}

void SceneView::clear() {
  for (auto& [k, m] : models_) stop_load(m);
  if (r_) {
    for (auto& [k, m] : models_) {
      for (const GpuPiece& piece : m.pieces) {
        r_->destroy_mesh(piece.mesh);
        if (piece.lod_mid.id) r_->destroy_mesh(piece.lod_mid);
        if (piece.lod_far.id) r_->destroy_mesh(piece.lod_far);
      }
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
  if (m.loading) {
    if (m.pending && m.pending->done.load()) {
      std::unique_ptr<Model> got;
      std::string err;
      {
        std::lock_guard<std::mutex> lock(m.pending->mu);
        got = std::move(m.pending->model);
        err = std::move(m.pending->error);
      }
      stop_load(m);
      m.tried = true;
      if (!got) {
        problem("model:" + source, "3D model " + source + " would not load: " + err);
        return m;
      }
      if (!err.empty()) problem("model-warn:" + source, source + ": " + err);
      m.model = std::move(got);
    }
    return m;
  }
  if (m.tried) return m;
  auto model = std::make_unique<Model>();
  MeshData prim;
  if (make_primitive(source, &prim)) {
    m.tried = true;
    model->source = source;
    model->bounds = prim.bounds();
    ModelPrimitive p;
    p.mesh = std::move(prim);
    model->primitives.push_back(std::move(p));
    m.model = std::move(model);
    return m;
  }
  if (source.empty()) {
    m.tried = true;
    return m;
  }
  const std::string path = (fs::path(dir_) / fs::path(source)).string();
  std::error_code ec;
  const auto sz = fs::file_size(fs::u8path(path), ec);
  if (!ec && sz >= kAsyncModelBytes) {
    m.loading = true;
    m.pending = std::make_shared<PendingModel>();
    auto state = m.pending;
    problem("loading:" + source, "Loading " + source + "...");
    m.loader = std::thread([state, path] {
      if (state->cancel.load()) {
        state->done.store(true);
        return;
      }
      ImportMonitor mon;
      mon.user = state.get();
      mon.cancelled = [](void* p) {
        return static_cast<PendingModel*>(p)->cancel.load() || import_was_cancelled();
      };
      set_import_monitor(&mon);
      auto model = std::make_unique<Model>();
      std::string err;
      const bool ok = load_model(path, model.get(), &err);
      set_import_monitor(nullptr);
      std::lock_guard<std::mutex> lock(state->mu);
      if (ok && !state->cancel.load()) state->model = std::move(model);
      state->error = err;
      state->done.store(true);
    });
    return m;
  }
  m.tried = true;
  std::string err;
  if (!load_model(path, model.get(), &err)) {
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

namespace {

// Merge vertices that land in the same grid cell and drop degenerate
// triangles. Used as a cheap LOD when a model has no authored LOD chain.
MeshData cluster_lod(const MeshData& src, int target_tris) {
  MeshData out;
  const int tris = static_cast<int>(src.indices.size() / 3);
  if (tris <= target_tris || src.vertices.empty() || target_tris < 4) return src;
  const Aabb box = src.bounds();
  const Vec3 ext = box.extent();
  const float span = std::max(ext.x, std::max(ext.y, ext.z));
  if (span < 1e-5f) return src;
  const int cells = std::max(2, static_cast<int>(std::ceil(std::cbrt(static_cast<double>(target_tris) * 2.0))));
  const float cell = span / static_cast<float>(cells);
  auto key_of = [&](const Vec3& p) {
    const int x = static_cast<int>(std::floor((p.x - box.min.x) / cell));
    const int y = static_cast<int>(std::floor((p.y - box.min.y) / cell));
    const int z = static_cast<int>(std::floor((p.z - box.min.z) / cell));
    return (static_cast<std::uint64_t>(x) * 73856093ull) ^ (static_cast<std::uint64_t>(y) * 19349663ull) ^
           (static_cast<std::uint64_t>(z) * 83492791ull);
  };
  std::unordered_map<std::uint64_t, std::uint32_t> map;
  std::vector<std::uint32_t> remap(src.vertices.size(), 0);
  map.reserve(src.vertices.size());
  for (std::size_t i = 0; i < src.vertices.size(); ++i) {
    const std::uint64_t k = key_of(src.vertices[i].position);
    auto it = map.find(k);
    if (it == map.end()) {
      const auto id = static_cast<std::uint32_t>(out.vertices.size());
      map.emplace(k, id);
      out.vertices.push_back(src.vertices[i]);
      remap[i] = id;
    } else {
      remap[i] = it->second;
    }
  }
  out.indices.reserve(static_cast<std::size_t>(target_tris) * 3);
  for (std::size_t i = 0; i + 2 < src.indices.size(); i += 3) {
    const std::uint32_t a = src.indices[i], b = src.indices[i + 1], c = src.indices[i + 2];
    if (a >= remap.size() || b >= remap.size() || c >= remap.size()) continue;
    const std::uint32_t ra = remap[a], rb = remap[b], rc = remap[c];
    if (ra == rb || rb == rc || rc == ra) continue;
    out.indices.push_back(ra);
    out.indices.push_back(rb);
    out.indices.push_back(rc);
  }
  if (out.indices.size() < 12 || !out.valid()) return src;
  return out;
}

struct ClipPlane {
  Vec3 n;
  float d = 0;
};

Vec4 v4add(Vec4 a, Vec4 b, float s) { return {a.x + s * b.x, a.y + s * b.y, a.z + s * b.z, a.w + s * b.w}; }

void frustum_planes(const Mat4& vp, ClipPlane out[6]) {
  auto row = [&](int r) { return Vec4{vp.at(r, 0), vp.at(r, 1), vp.at(r, 2), vp.at(r, 3)}; };
  const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
  const Vec4 raw[6] = {v4add(r3, r0, 1), v4add(r3, r0, -1), v4add(r3, r1, 1), v4add(r3, r1, -1), v4add(r3, r2, 1),
                       v4add(r3, r2, -1)};
  for (int i = 0; i < 6; ++i) {
    const float len = std::sqrt(raw[i].x * raw[i].x + raw[i].y * raw[i].y + raw[i].z * raw[i].z);
    const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
    out[i].n = {raw[i].x * inv, raw[i].y * inv, raw[i].z * inv};
    out[i].d = raw[i].w * inv;
  }
}

bool outside_frustum(const Aabb& b, const ClipPlane planes[6]) {
  for (int i = 0; i < 6; ++i) {
    const Vec3 p{planes[i].n.x >= 0 ? b.max.x : b.min.x, planes[i].n.y >= 0 ? b.max.y : b.min.y,
                 planes[i].n.z >= 0 ? b.max.z : b.min.z};
    if (dot(planes[i].n, p) + planes[i].d < 0.0f) return true;
  }
  return false;
}

}  // namespace

void SceneView::upload(ModelEntry& m) {
  if (m.uploaded || m.upload_stopped || !m.model || !r_) return;
  if (import_was_cancelled()) {
    m.upload_stopped = true;
    problem("upload-cancel", "Model upload cancelled.");
    return;
  }
  const std::size_t budget = upload_budget_;
  // True when this many bytes still fit in the frame. A single resource
  // larger than the budget does not fit (the caller slices it), unless
  // `allow_over` is set so one oversized texture can still move.
  auto room = [&](std::size_t bytes, bool allow_over) {
    if (budget == 0) return true;
    if (bytes > budget) {
      if (!allow_over || upload_used_ > 0) return false;
      upload_used_ += bytes;
      return true;
    }
    if (upload_used_ > 0 && upload_used_ + bytes > budget) return false;
    upload_used_ += bytes;
    return true;
  };
  if (m.rest_palette.empty() && m.model->skinned()) {
    skin_palette(m.model->skeleton, rest_pose(m.model->skeleton), &m.rest_palette);
  }
  if (m.materials.empty()) {
    // A stand-in so the mesh can draw before its textures have uploaded.
    m.materials.push_back(r_->create_material(MaterialDesc{}));
  }
  while (m.mat_cursor < m.model->materials.size()) {
    const MaterialDesc& d = m.model->materials[m.mat_cursor];
    auto img_bytes = [](const std::shared_ptr<const Image>& im) {
      return im && im->valid() ? static_cast<std::size_t>(im->width) * static_cast<std::size_t>(im->height) * 4u : 0u;
    };
    const std::size_t bytes = img_bytes(d.base_color_image) + img_bytes(d.normal_image) + img_bytes(d.metal_rough_image);
    if (!room(bytes, true)) return;
    // Insert before the stand-in so material index matches the model.
    m.materials.insert(m.materials.end() - 1, r_->create_material(d));
    ++m.mat_cursor;
  }
  while (m.prim_cursor < m.model->primitives.size()) {
    const MeshData& mesh = m.model->primitives[m.prim_cursor].mesh;
    if (m.index_cursor == 0) {
      const std::size_t whole = mesh_upload_bytes(mesh);
      if (room(whole, false)) {
        const MeshHandle h = r_->create_mesh(mesh);
        if (h.id) {
          GpuPiece piece{h, static_cast<int>(m.prim_cursor)};
          piece.tris = static_cast<int>(mesh.indices.size() / 3);
          // Ordinary props stay on the mesh that was imported. LOD is for the
          // piles of high-poly copies (a crate is 1,682; a 10k-tri prop too).
          if (!mesh.skinned() && piece.tris >= 1500) {
            MeshData mid = cluster_lod(mesh, std::max(24, piece.tris / 8));
            MeshData far = cluster_lod(mesh, std::max(12, piece.tris / 48));
            if (static_cast<int>(mid.indices.size() / 3) + 8 < piece.tris) {
              piece.lod_mid = r_->create_mesh(mid);
              piece.tris_mid = static_cast<int>(mid.indices.size() / 3);
            }
            const int finer = piece.tris_mid > 0 ? piece.tris_mid : piece.tris;
            if (static_cast<int>(far.indices.size() / 3) + 4 < finer) {
              piece.lod_far = r_->create_mesh(far);
              piece.tris_far = static_cast<int>(far.indices.size() / 3);
            }
          }
          m.pieces.push_back(piece);
        }
        ++m.prim_cursor;
        continue;
      }
      if (budget == 0) return;
    }
    if (budget > 0 && upload_used_ >= budget) return;
    const std::size_t left = budget == 0 ? 0 : budget - upload_used_;
    const MeshSlice slice = plan_mesh_slice(mesh, m.index_cursor, left);
    if (slice.index_count == 0) {
      ++m.prim_cursor;
      m.index_cursor = 0;
      continue;
    }
    if (!room(slice.bytes, true) && !m.pieces.empty()) return;
    MeshData chunk;
    if (bake_mesh_slice(mesh, slice, &chunk)) {
      const MeshHandle h = r_->create_mesh(chunk);
      if (h.id) m.pieces.push_back({h, static_cast<int>(m.prim_cursor)});
    }
    m.index_cursor += slice.index_count;
    if (m.index_cursor >= mesh.indices.size()) {
      ++m.prim_cursor;
      m.index_cursor = 0;
    }
    if (budget > 0) return;  // one slice of a mesh that did not fit
  }
  m.uploaded = m.prim_cursor >= m.model->primitives.size() && m.mat_cursor >= m.model->materials.size();
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

void SceneView::draw_hitboxes(const scene3d::Entity3D& e) {
  if (!r_ || !e.mesh) return;
  const Model* m = model(e.mesh->source);
  if (!m || m->hitboxes.empty()) return;
  const std::vector<Mat4>* pal = (e.skin_palette && !e.skin_palette->empty()) ? e.skin_palette.get() : nullptr;
  std::vector<Mat4> mats;
  hitbox_matrices(*m, pal, &mats);
  const Mat4 world = e.transform.matrix();
  for (std::size_t i = 0; i < m->hitboxes.size(); ++i) {
    const ModelHitbox& h = m->hitboxes[i];
    // Head (group 1) red, everything else teal.
    const float head[4] = {1.0f, 0.3f, 0.25f, 0.95f}, body[4] = {0.3f, 0.9f, 0.95f, 0.85f};
    draw_box(world * mats[i], Aabb{h.min, h.max}, h.group == 1 ? head : body, true);
  }
}

void SceneView::draw_bones(const scene3d::Entity3D& e, int highlight) {
  if (!r_ || !e.mesh || e.mesh->is_primitive()) return;
  const Model* m = model(e.mesh->source);
  if (!m || !m->skinned()) return;
  const bool posed = e.skin_palette && e.skin_palette->size() == m->skeleton.joints.size();
  const std::vector<Vec3> at =
      posed ? joint_positions(m->skeleton, *e.skin_palette) : joint_positions(m->skeleton, rest_pose(m->skeleton));
  const Mat4 world = e.transform.matrix();
  const float bone[4] = {0.96f, 0.84f, 0.42f, 1.0f};
  const float hot[4] = {1.0f, 0.42f, 0.16f, 1.0f};
  const float mark[4] = {0.98f, 0.96f, 0.90f, 1.0f};
  // A Mixamo-sized rig is fine. Past kMaxDrawnBones the rest still skin;
  // drawing every joint marker would stall the viewport.
  const std::size_t budget = std::min(at.size(), static_cast<std::size_t>(kMaxDrawnBones));
  for (std::size_t i = 0; i < m->skeleton.joints.size() && i < at.size(); ++i) {
    if (i >= budget && static_cast<int>(i) != highlight) continue;
    const Vec3 w = world.transform_point(at[i]);
    const bool sel = highlight == static_cast<int>(i);
    const int parent = m->skeleton.joints[i].parent;
    if (parent >= 0 && static_cast<std::size_t>(parent) < at.size()) {
      r_->draw_line(world.transform_point(at[static_cast<std::size_t>(parent)]), w, sel ? hot : bone, false);
    }
    const float s = sel ? 0.055f : 0.028f;
    const float* col = sel ? hot : mark;
    r_->draw_line(w - Vec3{s, 0, 0}, w + Vec3{s, 0, 0}, col, false);
    r_->draw_line(w - Vec3{0, s, 0}, w + Vec3{0, s, 0}, col, false);
    r_->draw_line(w - Vec3{0, 0, s}, w + Vec3{0, 0, s}, col, false);
  }
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
        const float hull[4] = {0.8f, 0.5f, 1.0f, 0.85f};
        std::vector<std::pair<Vec3, Vec3>> edges;
        if (e.mesh && !e.mesh->is_primitive()) {
          if (const Model* model = this->model(e.mesh->source)) {
            const bool pieces = c.fit == "decompose" && !model->fitted.pieces.empty();
            const auto& hulls = pieces ? model->fitted.pieces
                                       : (!model->fitted.convex.empty() && shape == S::ConvexHull)
                                             ? model->fitted.convex
                                             : model->collision_hulls;
            if (shape == S::ConvexHull && !hulls.empty()) {
              for (const auto& h : hulls) hull_wire(h, &edges);
            } else if (shape == S::Mesh) {
              MeshData rest;
              rest_pose_mesh(*model, &rest);
              const std::size_t tris = rest.indices.size() / 3;
              const std::size_t stride = std::max<std::size_t>(1, tris / 1500);
              for (std::size_t t = 0; t < tris; t += stride) {
                const std::uint32_t ia = rest.indices[t * 3], ib = rest.indices[t * 3 + 1],
                                    ic = rest.indices[t * 3 + 2];
                if (ia >= rest.vertices.size() || ib >= rest.vertices.size() || ic >= rest.vertices.size()) continue;
                const Vec3 a = rest.vertices[ia].position, b = rest.vertices[ib].position,
                           cv = rest.vertices[ic].position;
                edges.push_back({a, b});
                edges.push_back({b, cv});
                edges.push_back({cv, a});
              }
            }
          }
        }
        if (edges.empty()) {
          draw_box(m, local_bounds(e), hull, false);
        } else {
          for (const auto& [a, b] : edges) {
            r_->draw_line(m.transform_point(a), m.transform_point(b), hull, true);
          }
        }
        break;
      }
      default:
        draw_box(m, {size * -0.5f, size * 0.5f}, col, false);
        break;
    }
  };
  Transform t;
  t.position = c.offset;
  t.rotation = Quat::from_euler_deg(c.rotation_deg);
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
  upload_used_ = 0;
  submit_ = {};
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
    if (!e.visible || !e.light) continue;
    if (e.light->type == scene3d::Light3D::Type::Spot) {
      SpotLight s;
      s.position = e.transform.position;
      s.direction = e.transform.rotation.rotate({0, 0, -1});
      for (int i = 0; i < 3; ++i) s.color[i] = e.light->color[i];
      s.intensity = e.light->intensity;
      s.range = e.light->range;
      const float outer = radians(std::max(1.0f, e.light->angle));
      s.outer_cos = std::cos(outer * 0.5f);
      s.inner_cos = std::cos(outer * 0.5f * std::clamp(e.light->inner, 0.05f, 0.99f));
      r_->add_spot_light(s);
    } else if (e.light->type == scene3d::Light3D::Type::Area) {
      AreaLight a;
      a.position = e.transform.position;
      a.axis_u = e.transform.rotation.rotate({e.light->size_x * 0.5f, 0, 0});
      a.axis_v = e.transform.rotation.rotate({0, 0, e.light->size_y * 0.5f});
      for (int i = 0; i < 3; ++i) a.color[i] = e.light->color[i];
      a.intensity = e.light->intensity;
      r_->add_area_light(a);
    } else if (e.light->type == scene3d::Light3D::Type::Emissive) {
      PointLight l;
      l.position = e.transform.position;
      for (int i = 0; i < 3; ++i) l.color[i] = e.light->color[i];
      l.intensity = e.light->intensity;
      l.range = e.light->range;
      r_->add_point_light(l);
    }
  }
  ClipPlane planes[6];
  const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
  frustum_planes(camera.projection(aspect) * camera.view(), planes);
  const float half_tan = std::tan(radians(std::max(1.0f, camera.fov_y_degrees)) * 0.5f);
  struct Batch {
    MeshHandle mesh;
    MaterialHandle mat;
    bool shadow = true;
    int tris = 0;
    std::vector<Mat4> worlds;
  };
  std::vector<Batch> batches;
  auto push_batch = [&](MeshHandle mesh, MaterialHandle mat, bool shadow, int tris, const Mat4& w) {
    for (Batch& b : batches) {
      if (b.mesh.id == mesh.id && b.mat.id == mat.id && b.shadow == shadow && b.tris == tris) {
        b.worlds.push_back(w);
        return;
      }
    }
    batches.push_back(Batch{mesh, mat, shadow, tris, {w}});
  };
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
    ++submit_.entities;
    const Mat4 world = e.transform.matrix();
    const Aabb world_box = local_bounds(e).transformed(world);
    if (outside_frustum(world_box, planes)) {
      ++submit_.culled;
      continue;
    }
    ++submit_.visible;
    const Vec3 center = world_box.center();
    const float dist = std::max(0.5f, length(center - camera.position));
    const Vec3 wext = world_box.extent();
    const float ext = std::max(wext.x, std::max(wext.y, wext.z));
    const float pixels = half_tan > 1e-4f ? (ext / dist) * (static_cast<float>(std::max(height, 1)) * 0.5f) / half_tan
                                          : 1000.0f;
    bool shadow = e.mesh->cast_shadows && env.shadows;
    if (shadow) {
      const float reach = std::max(1.0f, env.shadow_distance) + ext;
      const Vec3 flat{center.x - camera.target.x, 0.0f, center.z - camera.target.z};
      if (length(flat) > reach) shadow = false;
    }
    const bool own_material = e.material && (e.mesh->is_primitive() || e.material->override_model);
    const MaterialHandle override_mat = own_material ? material_for(*e.material) : MaterialHandle{};
    for (const GpuPiece& piece : m.pieces) {
      const std::size_t i = static_cast<std::size_t>(piece.prim);
      if (i >= m.model->primitives.size()) continue;
      MaterialHandle mat = override_mat;
      if (!own_material) {
        const int mi = m.model->primitives[i].material;
        // materials holds uploaded model materials, then the stand-in.
        mat = (mi >= 0 && static_cast<std::size_t>(mi) < m.mat_cursor &&
               static_cast<std::size_t>(mi) + 1 < m.materials.size())
                  ? m.materials[static_cast<std::size_t>(mi)]
                  : m.materials.back();
      }
      if (m.model->primitives[i].mesh.skinned()) {
        // Posed by the animator this tick, else the rest pose.
        const std::vector<Mat4>& full =
            (e.skin_palette && !e.skin_palette->empty()) ? *e.skin_palette : m.rest_palette;
        const ModelPrimitive& prim = m.model->primitives[i];
        if (prim.joint_map.empty()) {
          r_->draw_skinned(piece.mesh, mat, world, full.data(), full.size(), shadow);
        } else {
          // Big skeleton: this primitive's own <= kMaxBones palette.
          primitive_palette(prim, full, &prim_palette_);
          r_->draw_skinned(piece.mesh, mat, world, prim_palette_.data(), prim_palette_.size(), shadow);
        }
        const int tris = piece.tris > 0 ? piece.tris : static_cast<int>(prim.mesh.indices.size() / 3);
        submit_.main_triangles += static_cast<std::size_t>(tris);
        if (shadow) submit_.shadow_triangles += static_cast<std::size_t>(tris);
        ++submit_.draws;
        ++submit_.lod_full;
      } else {
        MeshHandle mesh = piece.mesh;
        int tris = piece.tris;
        if (pixels < 28.0f && piece.lod_far.id) {
          mesh = piece.lod_far;
          tris = piece.tris_far;
          ++submit_.lod_far;
        } else if (pixels < 96.0f && piece.lod_mid.id) {
          mesh = piece.lod_mid;
          tris = piece.tris_mid;
          ++submit_.lod_mid;
        } else {
          ++submit_.lod_full;
        }
        push_batch(mesh, mat, shadow, tris, world);
      }
    }
  }
  for (const Batch& b : batches) {
    if (b.worlds.empty()) continue;
    submit_.main_triangles += static_cast<std::size_t>(b.tris) * b.worlds.size();
    if (b.shadow) submit_.shadow_triangles += static_cast<std::size_t>(b.tris) * b.worlds.size();
    ++submit_.draws;
    if (b.worlds.size() == 1) r_->draw_matrix(b.mesh, b.mat, b.worlds[0], b.shadow);
    else r_->draw_instanced(b.mesh, b.mat, b.worlds.data(), b.worlds.size(), b.shadow);
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
    if (opt.hitboxes && e.mesh && !e.mesh->is_primitive()) draw_hitboxes(e);
    if (opt.bones && e.mesh && !e.mesh->is_primitive()) {
      draw_bones(e, opt.bone_entity == e.id ? opt.bone_joint : -1);
    }
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
