#pragma once

// Draw-side terrain: a process-wide store of loaded TerrainData (shared by
// the editor, SceneView and World3D so an edit shows everywhere at once)
// and TerrainView, the GPU cache SceneView uses: LOD chunk meshes rebuilt
// only where edits landed, a splat material, foliage meshes and per-frame
// instance culling.

#include "render3d/Renderer3D.h"
#include "terrain/TerrainComponent.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
struct Model;
}
namespace terrain {

// The data for a component: loaded from project_dir / c.data on first use
// (flat at the component's resolution / size when the file is missing),
// then shared. The component's size wins over the file's.
std::shared_ptr<TerrainData> acquire(const std::string& project_dir, const Terrain3D& c);
// Already-loaded data only (nullptr if never acquired).
std::shared_ptr<TerrainData> find_loaded(const std::string& project_dir, const Terrain3D& c);
// Write the shared data to its file (creates folders).
bool save(const std::string& project_dir, const Terrain3D& c, std::string* error);
// Forget loaded data (project closed / reverted). "" = everything.
void forget(const std::string& project_dir = "");

struct ViewStats {
  int chunks = 0;
  int lod_counts[5] = {0, 0, 0, 0, 0};
  std::size_t foliage = 0;
  int rebuilt = 0;  // chunk meshes built this frame
};

class TerrainView {
 public:
  using ImageFn = std::function<std::shared_ptr<const render3d::Image>(const std::string&)>;
  using ModelFn = std::function<const render3d::Model*(const std::string&)>;

  explicit TerrainView(render3d::Renderer3D* r) : r_(r) {}
  ~TerrainView() { clear(); }
  void set_sources(ImageFn images, ModelFn models) {
    images_ = std::move(images);
    models_ = std::move(models);
  }
  void clear();
  // Draw one terrain entity (origin = its position) for the frame.
  void draw(std::uint64_t entity, const Terrain3D& c, const std::string& project_dir, const Vec3& origin,
            const Vec3& camera, float time);
  // Entities not drawn since the last call lose their cache.
  void prune();
  const ViewStats& stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }

 private:
  struct Chunk {
    render3d::MeshHandle lod[5];
    render3d::Aabb bounds;
  };
  struct FoliageMesh {
    render3d::MeshHandle mesh;
    float height = 0;
    bool builtin = true;
  };
  struct Entry {
    std::shared_ptr<TerrainData> data;
    std::uint64_t revision = ~0ull;
    ChunkLayout layout;
    int chunk_setting = 0;
    float skirt = -1;
    std::vector<Chunk> chunks;
    render3d::MaterialHandle material;
    std::string material_key;
    FoliageIndex index;
    std::vector<std::vector<FoliageGpuInstance>> scratch;
    bool seen = true;
  };
  void drop(Entry& e);
  void refresh(Entry& e, const Terrain3D& c);
  void ensure_material(Entry& e, const Terrain3D& c);
  FoliageMesh& foliage_mesh(const std::string& name);

  render3d::Renderer3D* r_;
  ImageFn images_;
  ModelFn models_;
  std::map<std::uint64_t, Entry> entries_;
  std::map<std::string, FoliageMesh> foliage_meshes_;
  ViewStats stats_;
};

}  // namespace terrain
}  // namespace tombstone
}  // namespace ts
