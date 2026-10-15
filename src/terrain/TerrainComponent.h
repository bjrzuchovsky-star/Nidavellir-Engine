#pragma once

// The Terrain3D entity component (scene.json v16 "terrain"). Heights,
// splat weights and foliage instances live in the binary `data` file
// (.tsterrain, project-relative); the component holds the settings.

#include "terrain/Foliage.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace terrain {

struct TerrainLayer {
  std::string texture;            // project image or builtin:grass / dirt / rock / sand / snow
  float tint[3] = {1, 1, 1};
  float tiling = 4.0f;            // metres per texture repeat
  bool operator==(const TerrainLayer& o) const = default;
};

struct Terrain3D {
  std::string data;               // e.g. "terrain/valley.tsterrain"; "" = flat, unsaved
  int resolution = 129;           // vertices per side (2^n + 1)
  float size = 128.0f;            // metres across
  int chunk = 32;                 // grid quads per LOD chunk
  float lod_distance = 45.0f;     // LOD 1 starts here, each next level at 2x
  float skirt = 1.0f;             // metres of crack-hiding skirt
  TerrainLayer layers[kLayers] = {
      {"builtin:grass", {1, 1, 1}, 4.0f},
      {"builtin:dirt", {1, 1, 1}, 3.0f},
      {"builtin:rock", {1, 1, 1}, 6.0f},
      {"builtin:sand", {1, 1, 1}, 4.0f},
  };
  float roughness = 0.9f;
  std::vector<FoliageType> foliage;
  float foliage_distance = 1.0f;  // multiplies every type's fade distances
  bool collider = true;           // Jolt heightfield + foliage colliders
  float friction = 0.8f;
  bool operator==(const Terrain3D& o) const = default;
};

void normalize(Terrain3D& c);
// LOD switch distances (lods - 1 entries).
void lod_distances(const Terrain3D& c, int lods, float* out);

}  // namespace terrain
}  // namespace tombstone
}  // namespace ts
