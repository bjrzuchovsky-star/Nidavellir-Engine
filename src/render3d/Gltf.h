#pragma once

// glTF 2.0 (.gltf + .bin / embedded data URIs, and .glb) loading via
// cgltf. Node transforms are baked into the vertices, so a Model is a
// flat list of primitives in model space, each with its material.
// Textures (base color, normal, metallic-roughness) are decoded with
// stb_image into RGBA8 images shared by the materials.

#include "render3d/Mesh.h"
#include "render3d/Skeleton.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct ModelPrimitive {
  MeshData mesh;
  int material = -1;  // index into Model::materials (-1 = default)
  // Scene node path it came from ("Armature/Body", Assimp / glTF), used to
  // spot collision (UCX_...) and LOD (..._LOD1) meshes at import.
  std::string node;
  // Skinned primitives of skeletons with more joints than the renderer
  // skins (kMaxBones): local joint index -> skeleton joint. The mesh's
  // SkinVertex joints are local; palettes are gathered through this.
  // Empty = the joints index the skeleton directly.
  std::vector<int> joint_map;
};

// A per-bone hit box (Source $hbox / studiohdr hitbox sets): an oriented
// box in its bone's space. `bind` takes bone space to model space in the
// rest pose (import scale / up axis baked); posed, use the joint matrix.
struct ModelHitbox {
  std::string name;       // hitbox name (often empty in Source files)
  std::string bone_name;
  int bone = -1;          // Model::skeleton joint (-1 = rigid model)
  int group = 0;          // Source hit group (head = 1, chest = 2 ...)
  Vec3 min, max;          // bone space
  Mat4 bind;              // bone -> model at rest
};

// Source engine body group (render3d/ModelImport.h): one is drawn per
// group; `selected` is the option the import kept.
struct ModelBodyGroup {
  std::string name;
  std::vector<std::string> options;
  int selected = 0;
};

// Where an imported material came from (Source .mdl: the .vmt / .vtf
// files found, or where they were looked for). Saved in the model's .meta.
struct MaterialSource {
  std::string name;      // material name in the model file
  std::string status;    // "ok", "partial" (vmt but no texture), "missing", "override"
  std::string material;  // file used (.vmt / .vtf), "" when missing
  std::string texture;   // base texture file used
  std::string shader;    // VMT shader (lower case)
  std::string searched;  // where it was looked for (missing ones)
};

// Generated collision (render3d/HullFit.h), cached in the model's .meta.
// Empty until ensure_fitted or a matching cache is applied.
struct FittedCollision {
  std::uint64_t fingerprint = 0;
  int vertex_limit = 64;
  int decompose_max = 8;
  bool has_obb = false;
  Vec3 obb_center;
  Vec3 obb_extents{1, 1, 1};
  Quat obb_rotation;
  std::vector<std::vector<Vec3>> convex;
  std::vector<std::vector<Vec3>> pieces;
  bool valid() const { return fingerprint != 0 && !convex.empty(); }
  bool matches(std::uint64_t fp, int verts, int pieces_max) const {
    return valid() && fingerprint == fp && vertex_limit == verts && decompose_max == pieces_max;
  }
};

struct Model {
  std::string source;  // path it came from
  std::string format;  // "gltf", "source-mdl", "fbx", "obj", "blend"...
  // Imported extras (ModelImport): Source body groups / skin families (the
  // primitives hold the selected option and skin), LOD count, collision
  // hulls in model space (Source .phy; one convex piece each).
  std::vector<ModelBodyGroup> body_groups;
  int skin_families = 0;
  int skin = 0;
  int lods = 1;
  std::vector<std::vector<Vec3>> collision_hulls;
  // Generated hulls (oriented box, convex, decomposition). Filled from the
  // .meta cache or by ensure_fitted. Not part of the file format itself.
  FittedCollision fitted;
  // Source hitbox set 0 ($hbox), when the file has one.
  std::string hitbox_set;
  std::vector<ModelHitbox> hitboxes;
  std::vector<ModelPrimitive> primitives;
  std::vector<MaterialDesc> materials;
  std::vector<MaterialSource> material_sources;  // Source .mdl (one per material)
  std::size_t textures = 0;  // images decoded
  // Model-space bounds of what is drawn: rest-posed skinned vertices,
  // referenced vertices only, after the import scale / up axis
  // (ModelImport update_bounds). Collision / LOD > 0 meshes never count.
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
