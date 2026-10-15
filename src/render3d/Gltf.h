#pragma once

// glTF 2.0 (.gltf + .bin / embedded data URIs, and .glb) loading via
// cgltf. Node transforms are baked into the vertices, so a Model is a
// flat list of primitives in model space, each with its material.
// Textures (base color, normal, metallic-roughness) are decoded with
// stb_image into RGBA8 images shared by the materials.

#include "render3d/Mesh.h"
#include "render3d/Skeleton.h"

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
  // Skinned models (the first glTF skin): skinned primitives keep their
  // vertices in bind (mesh) space with MeshData::skin weights; the
  // skeleton's palette poses them. Clips are the file's animations that
  // touch the skin's joints.
  Skeleton skeleton;
  std::vector<Clip> clips;
  bool skinned() const { return !skeleton.empty(); }
  int find_clip(const std::string& name) const;
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

// A skinned glTF (one skin, its clips) with everything embedded. The parts'
// meshes carry MeshData::skin; joints' rest TRS and parents come from
// `skeleton` (inverse binds are recomputed from the rest pose).
bool write_skinned_gltf(const std::string& path, const std::vector<GltfPart>& parts,
                        const Skeleton& skeleton, const std::vector<Clip>& clips,
                        std::string* error);

// The bundled CC0 rider ("Drifter"): a low-poly cowpoke of boxes on a
// 15-joint skeleton with idle / walk / run / jump / wave clips (wave only
// moves the right arm: an upper-body layer). Generated here, so it is ours
// to give away (CC0).
bool write_sample_character_gltf(const std::string& path, std::string* error);

// A skinned right hand for VR (palm + four fingers + thumb) with a "grip"
// clip (0 = open, end = fist). Mirrored on x for the left hand at runtime.
bool write_sample_hand_gltf(const std::string& path, std::string* error);

// The ghost-town lantern post: iron post + arm, glowing glass box.
bool write_sample_lantern_gltf(const std::string& path, std::string* error);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
