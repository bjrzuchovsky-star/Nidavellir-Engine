#pragma once

// Mesh + material descriptions for the ts_render3d sketch. CPU-side data
// only; a backend turns them into GPU resources behind opaque handles.

#include "render3d/Math3D.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct Vertex3D {
  Vec3 position;
  Vec3 normal;
  float u = 0.0f, v = 0.0f;
};

struct MeshData {
  std::string name;
  std::vector<Vertex3D> vertices;
  std::vector<std::uint32_t> indices;  // triangles
  bool valid() const {
    if (vertices.empty() || indices.size() % 3 != 0) return false;
    for (std::uint32_t i : indices) {
      if (i >= vertices.size()) return false;
    }
    return true;
  }
};

// Unlit / simple PBR-ish parameters; textures are project-relative paths.
struct MaterialDesc {
  std::string name;
  float base_color[4] = {0.8f, 0.6f, 0.4f, 1.0f};  // dusty copper
  float metallic = 0.0f;
  float roughness = 0.8f;
  std::string base_color_texture;
  bool double_sided = false;
};

// Opaque handles a renderer hands out (0 = none).
struct MeshHandle { std::uint32_t id = 0; };
struct MaterialHandle { std::uint32_t id = 0; };

// A unit cube (24 vertices, 36 indices) for placeholders and tests.
MeshData make_cube(float size = 1.0f);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
