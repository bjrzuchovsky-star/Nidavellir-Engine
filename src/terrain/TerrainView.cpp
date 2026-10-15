#include "terrain/TerrainView.h"

#include "render3d/Gltf.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>

namespace ts {
namespace tombstone {
namespace terrain {

namespace fs = std::filesystem;
using render3d::Aabb;
using render3d::MaterialDesc;
using render3d::MeshData;

void normalize(Terrain3D& c) {
  if (!valid_resolution(c.resolution)) c.resolution = 129;
  c.size = std::clamp(c.size, 4.0f, 8192.0f);
  c.chunk = std::clamp(c.chunk, 8, 128);
  c.lod_distance = std::clamp(c.lod_distance, 4.0f, 4000.0f);
  c.skirt = std::clamp(c.skirt, 0.0f, 50.0f);
  c.roughness = std::clamp(c.roughness, 0.04f, 1.0f);
  c.foliage_distance = std::clamp(c.foliage_distance, 0.1f, 8.0f);
  c.friction = std::clamp(c.friction, 0.0f, 2.0f);
  for (TerrainLayer& l : c.layers) {
    l.tiling = std::clamp(l.tiling, 0.1f, 512.0f);
    for (float& t : l.tint) t = std::clamp(t, 0.0f, 4.0f);
  }
  if (c.foliage.size() > 32) c.foliage.resize(32);
  for (FoliageType& f : c.foliage) normalize(f);
}

void lod_distances(const Terrain3D& c, int lods, float* out) {
  float d = c.lod_distance;
  for (int i = 0; i + 1 < lods; ++i) {
    out[i] = d;
    d *= 2.0f;
  }
}

// ---- Store -----------------------------------------------------------------

namespace {
std::mutex g_mu;
std::map<std::string, std::shared_ptr<TerrainData>> g_store;

std::string key_for(const std::string& dir, const Terrain3D& c) {
  if (c.data.empty()) return dir + "|flat:" + std::to_string(c.resolution) + ":" + std::to_string(c.size);
  return dir + "|" + c.data;
}
}  // namespace

std::shared_ptr<TerrainData> find_loaded(const std::string& dir, const Terrain3D& c) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_store.find(key_for(dir, c));
  return it == g_store.end() ? nullptr : it->second;
}

std::shared_ptr<TerrainData> acquire(const std::string& dir, const Terrain3D& c) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto& slot = g_store[key_for(dir, c)];
  if (!slot) {
    slot = std::make_shared<TerrainData>();
    bool loaded = false;
    if (!c.data.empty()) {
      std::error_code ec;
      const fs::path p = fs::path(dir) / fs::path(c.data);
      if (fs::exists(p, ec)) loaded = load_file(p.string(), slot.get(), nullptr);
    }
    if (!loaded) reset(*slot, c.resolution, c.size);
  }
  if (slot->valid() && std::fabs(slot->size - c.size) > 1e-4f && c.size > 0) {
    slot->size = c.size;
    for (FoliageInstance& f : slot->foliage) f.y = height_at(*slot, f.x, f.z);
    note_edit(*slot, full_rect(*slot), true, false, true);
  }
  return slot;
}

bool save(const std::string& dir, const Terrain3D& c, std::string* error) {
  if (c.data.empty()) {
    if (error) *error = "terrain has no data file";
    return false;
  }
  std::shared_ptr<TerrainData> d = find_loaded(dir, c);
  if (!d) return true;  // never loaded: the file on disk is current
  const fs::path p = fs::path(dir) / fs::path(c.data);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  return save_file(p.string(), *d, error);
}

void forget(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (dir.empty()) {
    g_store.clear();
    return;
  }
  for (auto it = g_store.begin(); it != g_store.end();)
    it = it->first.rfind(dir + "|", 0) == 0 ? g_store.erase(it) : std::next(it);
}

// ---- View ------------------------------------------------------------------

void TerrainView::drop(Entry& e) {
  if (!r_) return;
  for (Chunk& c : e.chunks)
    for (render3d::MeshHandle& h : c.lod)
      if (h.id) r_->destroy_mesh(h), h = {};
  e.chunks.clear();
  if (e.material.id) r_->destroy_material(e.material);
  e.material = {};
  e.material_key.clear();
}

void TerrainView::clear() {
  for (auto& [id, e] : entries_) drop(e);
  entries_.clear();
  if (r_)
    for (auto& [n, m] : foliage_meshes_)
      if (m.mesh.id) r_->destroy_mesh(m.mesh);
  foliage_meshes_.clear();
}

void TerrainView::prune() {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (!it->second.seen) {
      drop(it->second);
      it = entries_.erase(it);
    } else {
      it->second.seen = false;
      ++it;
    }
  }
}

void TerrainView::refresh(Entry& e, const Terrain3D& c) {
  const TerrainData& t = *e.data;
  const ChunkLayout l = chunk_layout(t, c.chunk);
  const bool layout_changed = l.quads != e.layout.quads || l.count != e.layout.count ||
                              static_cast<int>(e.chunks.size()) != l.count * l.count || e.skirt != c.skirt;
  Rect dirty;
  bool splat = false, foliage = false;
  const bool incremental = !layout_changed && edits_since(t, e.revision, &dirty, &splat, &foliage);
  if (!incremental) {
    for (Chunk& ch : e.chunks)
      for (render3d::MeshHandle& h : ch.lod)
        if (h.id) r_->destroy_mesh(h), h = {};
    e.layout = l;
    e.skirt = c.skirt;
    e.chunks.assign(static_cast<std::size_t>(l.count) * l.count, {});
    for (int cz = 0; cz < l.count; ++cz)
      for (int cx = 0; cx < l.count; ++cx)
        e.chunks[static_cast<std::size_t>(cz) * l.count + cx].bounds = chunk_bounds(t, l, cx, cz);
    if (e.material.id) r_->update_splat(e.material, splat_image(t));
  } else {
    std::vector<int> touched;
    chunks_in(l, dirty, &touched);
    for (int k : touched) {
      Chunk& ch = e.chunks[static_cast<std::size_t>(k)];
      for (render3d::MeshHandle& h : ch.lod)
        if (h.id) r_->destroy_mesh(h), h = {};
      ch.bounds = chunk_bounds(t, l, k % l.count, k / l.count);
    }
    if (splat && e.material.id) r_->update_splat(e.material, splat_image(t));
  }
  e.revision = t.revision;
}

void TerrainView::ensure_material(Entry& e, const Terrain3D& c) {
  std::string key = std::to_string(e.data->res) + "|" + std::to_string(c.roughness);
  for (const TerrainLayer& l : c.layers) {
    key += "|" + l.texture + ":" + std::to_string(l.tint[0]) + "," + std::to_string(l.tint[1]) + "," +
           std::to_string(l.tint[2]) + "@" + std::to_string(l.tiling);
  }
  if (e.material.id && key == e.material_key) return;
  if (e.material.id) r_->destroy_material(e.material);
  MaterialDesc d;
  d.name = "terrain";
  d.base_color[0] = d.base_color[1] = d.base_color[2] = d.base_color[3] = 1.0f;
  d.roughness = c.roughness;
  d.metallic = 0.0f;
  d.splat_image = std::make_shared<render3d::Image>(splat_image(*e.data));
  for (int i = 0; i < kLayers; ++i) {
    const TerrainLayer& l = c.layers[i];
    std::shared_ptr<const render3d::Image> img;
    if (l.texture.rfind("builtin:", 0) == 0) {
      static std::map<std::string, std::shared_ptr<const render3d::Image>> builtin;
      auto& slot = builtin[l.texture];
      if (!slot) {
        auto im = std::make_shared<render3d::Image>();
        if (builtin_layer_image(l.texture, im.get())) slot = im;
      }
      img = slot;
    } else if (!l.texture.empty() && images_) {
      img = images_(l.texture);
    }
    d.layer_images[i] = img;
    for (int k = 0; k < 3; ++k) d.layer_tint[i][k] = l.tint[k];
    d.layer_tint[i][3] = 1.0f;
    d.layer_tiling[i] = l.tiling;
  }
  e.material = r_->create_material(d);
  e.material_key = key;
}

TerrainView::FoliageMesh& TerrainView::foliage_mesh(const std::string& name) {
  auto it = foliage_meshes_.find(name);
  if (it != foliage_meshes_.end()) return it->second;
  FoliageMesh fm;
  MeshData m;
  if (make_foliage_mesh(name, &m)) {
    fm.mesh = r_->create_mesh(m);
  } else if (models_) {
    if (const render3d::Model* model = models_(name)) {
      MeshData merged;
      for (const render3d::ModelPrimitive& p : model->primitives) {
        if (p.mesh.skinned()) continue;
        const std::uint32_t base = static_cast<std::uint32_t>(merged.vertices.size());
        merged.vertices.insert(merged.vertices.end(), p.mesh.vertices.begin(), p.mesh.vertices.end());
        for (std::uint32_t i : p.mesh.indices) merged.indices.push_back(base + i);
      }
      if (merged.valid()) {
        fm.mesh = r_->create_mesh(merged);
        fm.height = std::max(0.01f, merged.bounds().max.y);
        fm.builtin = false;
      }
    }
  }
  return foliage_meshes_[name] = fm;
}

void TerrainView::draw(std::uint64_t entity, const Terrain3D& c, const std::string& dir, const Vec3& origin,
                       const Vec3& camera, float time) {
  if (!r_) return;
  Entry& e = entries_[entity];
  e.seen = true;
  std::shared_ptr<TerrainData> data = acquire(dir, c);
  if (e.data != data) {
    drop(e);
    e.data = data;
    e.revision = ~0ull;
  }
  if (!e.data || !e.data->valid()) return;
  if (e.revision != e.data->revision || static_cast<int>(e.chunks.size()) == 0) refresh(e, c);
  ensure_material(e, c);
  const TerrainData& t = *e.data;
  const ChunkLayout& l = e.layout;
  float dists[4];
  lod_distances(c, l.lods, dists);
  const render3d::Mat4 world = render3d::Mat4::translate(origin);
  for (int cz = 0; cz < l.count; ++cz) {
    for (int cx = 0; cx < l.count; ++cx) {
      Chunk& ch = e.chunks[static_cast<std::size_t>(cz) * l.count + cx];
      const Vec3 centre = ch.bounds.center() + origin;
      const Vec3 half = ch.bounds.extent() * 0.5f;
      // Distance to the chunk box, not its centre: neighbours agree better.
      const Vec3 d{std::max(0.0f, std::fabs(camera.x - centre.x) - half.x),
                   std::max(0.0f, std::fabs(camera.y - centre.y) - half.y),
                   std::max(0.0f, std::fabs(camera.z - centre.z) - half.z)};
      const int lod = select_lod(render3d::length(d), dists, l.lods - 1, l.lods);
      render3d::MeshHandle& h = ch.lod[lod];
      if (!h.id) {
        h = r_->create_mesh(build_chunk(t, l, cx, cz, lod, c.skirt));
        ++stats_.rebuilt;
      }
      r_->draw_matrix(h, e.material, world, true);
      ++stats_.chunks;
      ++stats_.lod_counts[lod];
    }
  }
  if (c.foliage.empty() || t.foliage.empty()) return;
  e.index.gather(t, c.foliage, origin, camera, c.foliage_distance, &e.scratch);
  for (std::size_t i = 0; i < c.foliage.size(); ++i) {
    const std::vector<FoliageGpuInstance>& inst = e.scratch[i];
    if (inst.empty()) continue;
    const FoliageType& ft = c.foliage[i];
    FoliageMesh& fm = foliage_mesh(ft.mesh);
    if (!fm.mesh.id) continue;
    render3d::FoliageBatch b;
    b.mesh = fm.mesh;
    for (int k = 0; k < 3; ++k) {
      b.color_a[k] = ft.color_a[k];
      b.color_b[k] = ft.color_b[k];
    }
    b.sway = ft.sway;
    b.mesh_height = fm.builtin ? 0.0f : fm.height;
    b.part_colors = fm.builtin;
    b.cast_shadow = ft.cast_shadows;
    b.time = time;
    b.instances = inst.data();
    b.count = inst.size();
    r_->draw_foliage(b);
    stats_.foliage += inst.size();
  }
}

}  // namespace terrain
}  // namespace tombstone
}  // namespace ts
