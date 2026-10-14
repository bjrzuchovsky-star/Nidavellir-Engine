#pragma once

// Mesh, material and image descriptions for ts_render3d. CPU-side data
// only; a backend turns them into GPU resources behind opaque handles.

#include "render3d/Math3D.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct Vertex3D {
  Vec3 position;
  Vec3 normal;
  float u = 0.0f, v = 0.0f;
  Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};  // xyz + handedness (w = +-1)
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
  Aabb bounds() const;
};

// Decoded RGBA8 pixels (top row first).
struct Image {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;
  bool valid() const {
    return width > 0 && height > 0 &&
           rgba.size() == static_cast<std::size_t>(width) * height * 4;
  }
};

// PBR-lite (metal / rough). Textures are project-relative paths; glTF
// models carry decoded images instead (the *_image pointers win).
struct MaterialDesc {
  std::string name;
  float base_color[4] = {0.8f, 0.6f, 0.4f, 1.0f};  // dusty copper
  float metallic = 0.0f;
  float roughness = 0.8f;
  float emissive[3] = {0.0f, 0.0f, 0.0f};
  std::string base_color_texture;
  std::string normal_texture;
  std::string metal_rough_texture;  // glTF layout: G = rough, B = metal
  std::shared_ptr<const Image> base_color_image;
  std::shared_ptr<const Image> normal_image;
  std::shared_ptr<const Image> metal_rough_image;
  bool double_sided = false;
  bool unlit = false;  // albedo straight out (UI panels)
};

// Opaque handles a renderer hands out (0 = none).
struct MeshHandle { std::uint32_t id = 0; };
struct MaterialHandle { std::uint32_t id = 0; };

// Built-in primitives, unit sized, centred on the origin (the plane lies
// in XZ facing +Y). Normals, UVs and tangents filled in.
MeshData make_cube(float size = 1.0f);
MeshData make_plane(float size = 1.0f, int divisions = 1);
MeshData make_sphere(float diameter = 1.0f, int segments = 32, int rings = 16);
// Capsule along Y (total height, radius), for player stand-ins.
MeshData make_capsule(float height = 2.0f, float radius = 0.4f,
                      int segments = 24);

// "primitive:cube" / "cube" -> mesh; false for unknown names.
bool make_primitive(const std::string& name, MeshData* out);
// Names make_primitive() knows, in UI order.
const std::vector<std::string>& primitive_names();

// Per-vertex tangents from positions / UVs (needs normals). Leaves the
// default tangent where UVs are degenerate.
void compute_tangents(MeshData& mesh);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
