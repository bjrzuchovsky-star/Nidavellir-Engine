#pragma once

// Draws a Scene3D's entities through a Renderer3D: loads and caches
// primitive meshes, glTF models and textures (project-relative paths),
// keeps one GPU material per entity material, and turns light components
// into renderer lights. Also answers "how big is this mesh" for picking,
// colliders and drag-drop (CPU only, works with the null backend).
//
// Problems (missing model, bad texture) are reported once per path via
// take_problems() so callers can send them to the Telegraph.

#include "render3d/Gltf.h"
#include "render3d/Renderer3D.h"
#include "render3d/Scene3D.h"
#include "terrain/TerrainView.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct SceneViewOptions {
  bool grid = false;
  float grid_extent = 30.0f;
  float grid_step = 1.0f;
  bool colliders = false;       // wire boxes for colliders
  bool light_gizmos = false;    // small crosses at point lights
  std::vector<std::uint64_t> highlight;  // selection outlines
};

class SceneView {
 public:
  explicit SceneView(Renderer3D* renderer);
  ~SceneView();
  SceneView(const SceneView&) = delete;
  SceneView& operator=(const SceneView&) = delete;

  void set_project_dir(const std::string& dir);
  const std::string& project_dir() const { return dir_; }
  Renderer3D* renderer() const { return r_; }

  // Drop every cached GPU / CPU resource (files changed on disk).
  void clear();

  // Model-space bounds of an entity's mesh (unit cube when none / unknown).
  Aabb local_bounds(const scene3d::Entity3D& e);
  Aabb mesh_bounds(const std::string& source);
  // CPU model for a source (nullptr when it does not load).
  const Model* model(const std::string& source);

  // begin_frame + environment + lights + every visible mesh entity (+
  // optional editor overlays). Caller may add lines, then end().
  void begin(const Environment& env, const std::vector<scene3d::Entity3D>& entities,
             const Camera3D& camera, int width, int height,
             const SceneViewOptions& options = {});
  FrameStats end();

  std::vector<std::string> take_problems();

  // Seconds for wind animation (foliage sway). Unset: a steady clock.
  void set_time(float seconds) {
    time_ = seconds;
    fixed_time_ = true;
  }
  float time() const;
  // Terrain chunk / foliage cache (stats, editor picking).
  terrain::TerrainView& terrain_view() { return *terrain_; }

  // World matrix of an entity.
  static Mat4 world_matrix(const scene3d::Entity3D& e) { return e.transform.matrix(); }
  // Particles for this frame (call between begin() and end()).
  void billboards(const std::vector<Renderer3D::Billboard>& b) {
    if (r_) r_->draw_billboards(b);
  }
  // World-space box lines.
  void draw_box(const Mat4& world, const Aabb& local, const float* rgba, bool on_top);
  void draw_circle(const Mat4& world, const Vec3& c, float r, int plane, const float* rgba);
  // Physics gizmos: collider shapes (compound parts included) coloured by
  // body motion, and joints (anchor -> target, hinge / slider axis).
  void draw_collider(const Mat4& world, const scene3d::Entity3D& e);
  void draw_joint(const Mat4& world, const scene3d::Entity3D& e,
                  const std::vector<scene3d::Entity3D>& entities);

 private:
  struct ModelEntry {
    bool tried = false;
    std::unique_ptr<Model> model;
    std::vector<MeshHandle> meshes;      // per primitive
    std::vector<MaterialHandle> materials;  // per model material (+1 default)
    std::vector<Mat4> rest_palette;         // skinned models: the rest pose
    bool uploaded = false;
  };
  struct MaterialEntry {
    MaterialHandle handle;
    bool used = true;
  };

  ModelEntry& entry(const std::string& source);
  void upload(ModelEntry& m);
  MaterialHandle material_for(const scene3d::Material3D& m);
  std::shared_ptr<const Image> image(const std::string& rel);
  void problem(const std::string& key, const std::string& text);

  Renderer3D* r_;
  std::string dir_;
  std::map<const void*, std::pair<std::uint64_t, Aabb>> terrain_bounds_;
  std::map<std::string, ModelEntry> models_;
  std::map<std::string, MaterialEntry> materials_;  // key = serialized material
  std::map<std::string, std::shared_ptr<const Image>> images_;
  std::map<std::string, bool> reported_;
  std::vector<std::string> problems_;
  std::unique_ptr<terrain::TerrainView> terrain_;
  float time_ = 0.0f;
  bool fixed_time_ = false;
};

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
