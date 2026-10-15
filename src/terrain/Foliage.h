#pragma once

// Terrain foliage: instance types (grass, trees, rocks...), deterministic
// paint / erase brushes, per-cell culling with distance fade, wind sway
// (the CPU twin of the GLSL in render3d/GlTerrainFoliage.cpp) and the
// built-in low-poly meshes.
//
// Built-in meshes encode two things in the texture coordinates: u is the
// colour blend between the type's color_a (0) and color_b (1) and v is
// the sway weight (0 rooted .. 1 free). Project glTF meshes work too; they
// are tinted by color_b and sway by their height.

#include "render3d/Renderer3D.h"
#include "terrain/Terrain.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace terrain {

struct FoliageType {
  std::string name = "grass";
  std::string mesh = "foliage:grass";  // built-in name or a project .gltf / .glb
  float density = 4.0f;       // instances per square metre at full fill
  float scale_min = 0.8f;
  float scale_max = 1.25f;
  float sway = 1.0f;          // wind response (0 = rigid)
  float fade_start = 35.0f;   // metres: starts thinning out
  float fade_end = 50.0f;     // metres: gone
  float color_a[3] = {0.20f, 0.33f, 0.10f};
  float color_b[3] = {0.55f, 0.66f, 0.25f};
  float slope_max = 40.0f;    // degrees; steeper ground stays bare
  int layer = -1;             // only where this splat layer dominates (-1 = anywhere)
  bool cast_shadows = false;
  bool collide = false;       // static capsule per instance (trees)
  float collide_radius = 0.3f;
  float collide_height = 4.0f;
  bool operator==(const FoliageType& o) const = default;
};
void normalize(FoliageType& t);
// Presets: "grass", "tall_grass", "pine", "oak", "bush", "rock", "cactus".
const std::vector<std::string>& foliage_preset_names();
bool foliage_preset(const std::string& name, FoliageType* out);

// ---- Wind ---------------------------------------------------------------

struct Wind {
  float dir_x = 1.0f, dir_z = 0.0f;  // unit direction (xz)
  float strength = 0.3f;             // 0 calm .. 1 gale (and beyond)
  float gust = 0.2f;                 // 0..1 extra turbulence
};
// Bend amount for a plant rooted at (x, z) at `time` seconds.
float wind_bend(float x, float z, float time, const Wind& w);
// Offset of a vertex with sway `weight` (0..1) for that bend.
Vec3 sway_offset(float bend, float weight, float sway, float scale, const Wind& w);

// ---- Brushes ------------------------------------------------------------

struct FoliageBrush {
  int type = 0;            // painting: the type; erasing: -1 = every type
  float radius = 5.0f;
  float fill = 1.0f;       // 0..1 fraction of the type's density
  std::uint32_t seed = 1;
};
// Adds instances on a jittered grid (one slot per 1/density m^2) inside the
// brush; deterministic and idempotent for the same inputs. Returns the
// number added.
int paint_foliage(TerrainData& t, const std::vector<FoliageType>& types, const FoliageBrush& b,
                  float x, float z);
// Removes `fill` (0..1) of the matching instances inside the brush.
int erase_foliage(TerrainData& t, const FoliageBrush& b, float x, float z);
// Drop instances of a removed type and shift higher type indices down.
void remove_foliage_type(TerrainData& t, int type);
int count_foliage(const TerrainData& t, int type);

// ---- Culling ------------------------------------------------------------

using FoliageGpuInstance = render3d::FoliageInstance3D;

// Buckets instances into square cells so a frame only looks at cells near
// the camera. Rebuilds itself when the terrain revision changes.
class FoliageIndex {
 public:
  static constexpr float kCell = 16.0f;
  void gather(const TerrainData& t, const std::vector<FoliageType>& types, const Vec3& origin,
              const Vec3& camera_world, float distance_scale,
              std::vector<std::vector<FoliageGpuInstance>>* per_type);
  std::size_t visited() const { return visited_; }

 private:
  void rebuild(const TerrainData& t);
  std::uint64_t revision_ = ~0ull;
  std::size_t count_ = 0;
  int n_ = 0;
  std::vector<std::vector<std::uint32_t>> cells_;
  std::size_t visited_ = 0;
};

// ---- Meshes -------------------------------------------------------------

const std::vector<std::string>& foliage_mesh_names();  // "foliage:grass" ...
bool make_foliage_mesh(const std::string& name, render3d::MeshData* out);
bool is_builtin_foliage_mesh(const std::string& name);

}  // namespace terrain
}  // namespace tombstone
}  // namespace ts
