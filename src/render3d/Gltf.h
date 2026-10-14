#pragma once

// glTF 2.0 (.gltf + .bin / embedded data URIs, and .glb) loading via
// cgltf. Node transforms are baked into the vertices, so a Model is a
// flat list of primitives in model space, each with its material.
// Textures (base color, normal, metallic-roughness) are decoded with
// stb_image into RGBA8 images shared by the materials.

#include "render3d/Mesh.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct ModelPrimitive {
  MeshData mesh;
  int material = -1;  // index into Model::materials (-1 = default)
};

struct Model {
  std::string source;  // path it came from
  std::vector<ModelPrimitive> primitives;
  std::vector<MaterialDesc> materials;
  std::size_t textures = 0;  // images decoded
  Aabb bounds;
  std::size_t triangles() const;
};

bool load_gltf(const std::string& path, Model* out, std::string* error);

// Decode an image file (PNG / JPG / TGA / BMP...) to RGBA8.
bool load_image_file(const std::string& path, Image* out, std::string* error);
bool load_image_memory(const unsigned char* data, std::size_t size, Image* out,
                       std::string* error);

// A small glTF with everything in one file (base64 buffer): one mesh per
// part, metal/rough materials. Used for the samples and tests.
struct GltfPart {
  MeshData mesh;
  MaterialDesc material;  // colors / factors only
};
bool write_gltf_embedded(const std::string& path,
                         const std::vector<GltfPart>& parts,
                         std::string* error);

// The ghost-town lantern post: iron post + arm, glowing glass box.
bool write_sample_lantern_gltf(const std::string& path, std::string* error);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
