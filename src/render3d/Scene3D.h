#pragma once

// 3D scene data (scene.json v7, "dimension": "3d"). A flat list of
// entities, each with a transform (position, rotation quaternion, scale)
// and optional components. 2D scenes keep their own format (v1..v6,
// scene/SceneJson.h); the 2D reader refuses a 3D file and vice versa.

#include "render3d/Math3D.h"
#include "render3d/Renderer3D.h"
#include "scene/SceneData.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace scene3d {

using render3d::Quat;
using render3d::Transform;
using render3d::Vec3;

// v8 added the VR components (vr_rig, grabbable); v9 parenting (parent)
// and the editor-only locked / editor_hidden flags; v10 the sound source
// (audio, distances in metres). v7 / v8 / v9 files still load.
inline constexpr int kScene3DVersion = 10;

// What to draw: "primitive:cube|plane|sphere|capsule" or a project-relative
// .gltf / .glb path.
struct MeshRef {
  std::string source = "primitive:cube";
  bool cast_shadows = true;
  bool operator==(const MeshRef& o) const = default;
  bool is_primitive() const { return source.rfind("primitive:", 0) == 0; }
};

struct Material3D {
  float albedo[4] = {0.75f, 0.62f, 0.48f, 1.0f};
  float metallic = 0.0f;
  float roughness = 0.75f;
  float emissive[3] = {0.0f, 0.0f, 0.0f};
  std::string albedo_texture;       // project-relative image paths
  std::string normal_texture;
  std::string metal_rough_texture;
  // glTF meshes keep their own materials unless this is set.
  bool override_model = false;
  bool operator==(const Material3D& o) const = default;
};

struct Light3D {
  enum class Type : std::uint8_t { Point, Directional };
  Type type = Type::Point;
  float color[3] = {1.0f, 0.78f, 0.5f};
  float intensity = 6.0f;
  float range = 8.0f;  // point only
  bool operator==(const Light3D& o) const = default;
};

struct Collider3D {
  enum class Shape : std::uint8_t { Box, Capsule };
  Shape shape = Shape::Box;
  Vec3 size{1.0f, 1.0f, 1.0f};  // box: full extents (local, times scale);
                                // capsule: x = radius, y = height
  Vec3 offset;                   // local centre offset
  bool trigger = false;          // reports overlaps, never blocks
  bool operator==(const Collider3D& o) const = default;
};

// Third-person rider: WASD relative to the camera, jump, gravity.
struct Player3D {
  float speed = 4.5f;        // m/s
  float jump_speed = 5.5f;   // m/s up
  float camera_distance = 6.0f;
  float camera_height = 2.5f;
  bool operator==(const Player3D& o) const = default;
};

// VR player rig ("dimension": "vr"): the headset's floor origin starts at
// the entity (yaw included) and moves with it. Comfort options live here
// and can be changed in the headset from the comfort menu.
struct VrRig3D {
  enum class Locomotion : std::uint8_t { Teleport, Smooth, Both };
  Locomotion locomotion = Locomotion::Both;
  float move_speed = 2.0f;        // m/s, left stick
  float snap_turn_deg = 45.0f;    // right stick; 0 = smooth turn
  float smooth_turn_speed = 120.0f;  // deg/s when snap_turn_deg is 0
  float teleport_range = 9.0f;    // m, arc reach
  bool vignette = true;           // darken the edges while moving / turning
  bool seated = false;            // lift the head by seated_offset
  float seated_offset = 0.45f;    // m
  bool operator==(const VrRig3D& o) const = default;
};

// Can be picked up with the grip (squeeze) and thrown. Falls with gravity
// until it lands on a static collider or the ground plane.
struct Grabbable3D {
  bool throwable = true;    // keep the hand's velocity on release
  bool snap_to_hand = false;  // centre on the hand instead of keeping the offset
  float throw_scale = 1.0f;   // release velocity multiplier
  bool operator==(const Grabbable3D& o) const = default;
};

struct Entity3D {
  std::uint64_t id = 0;
  std::string name;
  Transform transform;
  bool visible = true;
  std::optional<MeshRef> mesh;
  std::optional<Material3D> material;
  std::optional<Light3D> light;
  std::optional<Collider3D> collider;
  std::optional<Player3D> player;
  std::optional<ScriptData> script;
  std::optional<VrRig3D> vr_rig;           // v8
  std::optional<Grabbable3D> grabbable;    // v8
  // v9: parent entity id (0 = scene root). Transforms stay world-space;
  // children follow their parent's moves in the editor and at runtime.
  std::uint64_t parent = 0;
  bool locked = false;         // v9, editor only: not pickable / movable
  bool editor_hidden = false;  // v9, editor only: not drawn in the editor
  std::optional<AudioSourceData> audio;  // v10, 3D defaults: audio_3d()
  bool operator==(const Entity3D& o) const = default;
};

// An audio source with 3D (metre) distances: full volume within 2 m,
// silent past 40 m.
AudioSourceData audio_3d();

struct EditorCamera3D {
  Vec3 target{0.0f, 0.5f, 0.0f};
  float yaw = 35.0f;      // degrees around +Y
  float pitch = 28.0f;    // degrees above the horizon
  float distance = 14.0f;
  bool operator==(const EditorCamera3D& o) const = default;
};

struct Scene3D {
  int version = kScene3DVersion;
  render3d::Environment environment;
  std::vector<Entity3D> entities;
  EditorCamera3D editor_camera;
  bool show_grid = true;
  float snap_translate = 0.5f;
  float snap_rotate = 15.0f;
  float snap_scale = 0.25f;
  bool snap = true;
  std::vector<std::uint64_t> selection;
  bool operator==(const Scene3D& o) const = default;

  Entity3D* find(std::uint64_t id);
  const Entity3D* find(std::uint64_t id) const;
  const Entity3D* find_named(const std::string& name) const;
  std::uint64_t next_id() const;
};

// <project_dir>/scene.json
std::string scene_path(const std::string& project_dir);

// Text in, text out. parse() refuses 2D scenes with a clear message.
bool parse(const std::string& text, Scene3D* out, std::string* error);
std::string write(const Scene3D& scene);
// Parents that are missing, the entity itself or part of a loop become 0.
void repair_parents(std::vector<Entity3D>* entities);
// Entity ids under `id` (children, grandchildren, ...), depth-first.
std::vector<std::uint64_t> descendants_of(const std::vector<Entity3D>& entities,
                                          std::uint64_t id);
// Move `child` the way its parent went from `prev` to `now` (rigidly,
// with the parent's scale change). Children keep world-space transforms.
void carry_child(Entity3D* child, const Transform& prev, const Transform& now);
// Depth of an entity in the parent chain (0 = root).
int depth_of(const std::vector<Entity3D>& entities, std::uint64_t id);
bool load_file(const std::string& path, Scene3D* out, std::string* error);
bool save_file(const std::string& path, const Scene3D& scene,
               std::string* error);

// True when the text is a 3D scene ("dimension": "3d").
bool is_3d_scene_text(const std::string& text);

// A fresh 3D claim: ground, sun-lit crate, a lamp post, a rider.
Scene3D make_starter_scene();

// A fresh VR claim: ground, crate, lamp, a table with things to grab and
// the player rig.
Scene3D make_vr_starter_scene();

// The "haunted-saloon-vr" sample: a saloon interior of primitives (bar,
// tables, piano, swinging doors), bottles and a hat to grab and throw, a
// spittoon that counts what lands in it, a ghost that answers the laser,
// a piano that plays when pointed at and triggered, and the VR rig. Writes
// project.json ("dimension": "vr") too.
bool write_haunted_saloon_sample(const std::string& project_dir,
                                 const std::string& display_name,
                                 std::string* error);

// The "ghost-town-3d" sample: main street of primitives, a glTF lantern
// (assets/lantern.gltf, written alongside), a checker-boarded ground
// texture (assets/boards.tga), a windmill script, and a rider. Writes
// project.json too. Returns false with the reason.
bool write_ghost_town_sample(const std::string& project_dir,
                             const std::string& display_name,
                             std::string* error);

}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
