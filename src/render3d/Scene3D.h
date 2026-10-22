#pragma once

// 3D scene data (scene.json v7, "dimension": "3d"). A flat list of
// entities, each with a transform (position, rotation quaternion, scale)
// and optional components. 2D scenes keep their own format (v1..v6,
// scene/SceneJson.h); the 2D reader refuses a 3D file and vice versa.

#include "fx/Particles.h"
#include "render3d/Math3D.h"
#include "render3d/Renderer3D.h"
#include "scene/JsonTree.h"
#include "scene/Prefab.h"
#include "scene/SceneData.h"
#include "terrain/TerrainComponent.h"

#include <cstdint>
#include <memory>
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
// (audio, distances in metres). v11 adds "particles" (emitters), v12 the
// skeletal "animator" and the VR rig's skinned "hand_model".
// v13 adds the AI "agent" (ts.ai navmesh agent) and "nav_link" (off-mesh
// link from the entity to `to`).
// v14 adds physics: rigid_body, joint, sphere / convex / mesh collider
// shapes, compound parts, collision layers and the scene "physics" block.
// v15 adds the prefab instance link ("prefab": asset, instance, source,
// overrides; see scene/Prefab.h).
// v16 adds the heightmap terrain component ("terrain": data file, size,
// LOD, splat layers, foliage types, collider) and environment fog / wind.
// v7 .. v15 files still load.
inline constexpr int kScene3DVersion = 16;

// ts.ai navmesh agent (v13): walks the baked navmesh with crowd avoidance,
// optionally driven by a behaviour tree (*.bt.json).
struct Agent3D {
  float speed = 2.0f;     // m/s
  float radius = 0.4f;    // m
  float height = 1.8f;    // m
  bool avoidance = true;  // crowd separation + obstacle avoidance
  std::string bt;         // project-relative .bt.json ("" = scripted only)
  float wander_radius = 6.0f;
  bool operator==(const Agent3D& o) const = default;
};

// Off-mesh link (v13): agents may jump / climb from the entity to `to`
// (world position). Baked into the navmesh.
struct NavLink3D {
  Vec3 to;
  float radius = 0.5f;
  bool bidirectional = true;
  bool operator==(const NavLink3D& o) const = default;
};

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
  // Runtime only (not saved): show the albedo as is, no lighting -- the VR
  // UI panel.
  bool unlit = false;
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

// Collider shapes. Sizes are local (times the entity scale):
//   box: full extents; capsule: x = radius, y = total height;
//   sphere: x = radius; convex: hull of the entity's mesh (size unused);
//   mesh: the entity's triangles (static bodies only; size unused).
enum class ColliderShape3D : std::uint8_t { Box, Capsule, Sphere, ConvexHull, Mesh };
const char* collider_shape_name(ColliderShape3D s);
ColliderShape3D collider_shape_from(const std::string& name);

// An extra primitive in a compound collider (offset / rotation local).
struct ColliderPart3D {
  ColliderShape3D shape = ColliderShape3D::Box;
  Vec3 size{0.5f, 0.5f, 0.5f};
  Vec3 offset;
  Vec3 rotation_deg;  // euler, degrees
  bool operator==(const ColliderPart3D& o) const = default;
};

struct Collider3D {
  using Shape = ColliderShape3D;
  Shape shape = Shape::Box;
  Vec3 size{1.0f, 1.0f, 1.0f};
  Vec3 offset;                   // local centre offset
  bool trigger = false;          // reports overlaps, never blocks
  int layer = -1;                // collision layer 0..7; -1 = by role
  std::vector<ColliderPart3D> parts;  // v14: compound extras
  bool operator==(const Collider3D& o) const = default;
};

// v14: a simulated body (Jolt). Without one, colliders are static and
// grabbables are dynamic with defaults.
struct RigidBody3D {
  enum class Motion : std::uint8_t { Static, Kinematic, Dynamic };
  Motion motion = Motion::Dynamic;
  float mass = 0.0f;            // kg; 0 = from the volume (water density)
  float friction = 0.5f;
  float restitution = 0.1f;     // bounciness 0..1
  float linear_damping = 0.05f;
  float angular_damping = 0.05f;
  float gravity_scale = 1.0f;
  bool ccd = false;             // swept motion (fast, small things)
  bool impact_events = true;    // on_impact + impact sound / dust
  bool operator==(const RigidBody3D& o) const = default;
};
const char* motion_name(RigidBody3D::Motion m);

// v14: joins this entity's body to `target` (0 = the world). Anchor and
// axis are in this entity's local space.
struct Joint3D {
  enum class Type : std::uint8_t { Fixed, Hinge, Slider, Distance, Ball };
  Type type = Type::Hinge;
  std::uint64_t target = 0;
  Vec3 anchor;                  // local
  Vec3 axis{0.0f, 1.0f, 0.0f};  // local (hinge / slider)
  bool limits = false;
  float min = 0.0f;  // hinge: degrees, slider / distance: metres
  float max = 0.0f;
  bool operator==(const Joint3D& o) const = default;
};
const char* joint_type_name(Joint3D::Type t);

// v14: scene-wide physics settings.
struct PhysicsSettings3D {
  Vec3 gravity{0.0f, -9.81f, 0.0f};
  int collision_steps = 1;
  std::string layer_names[8] = {"Static", "Default", "Held",     "Player",
                                "Debris", "Trigger", "Custom A", "Custom B"};
  // collide[i] bit j: layers i and j touch (kept symmetric). Debris only
  // touches Static and Debris by default.
  std::uint8_t collide[8] = {0xff, 0xef, 0xef, 0xef, 0x11, 0xef, 0xef, 0xef};
  bool debug_draw = false;  // editor + play: collider wireframes
  bool operator==(const PhysicsSettings3D& o) const = default;
};

// The rider: WASD relative to the camera, jump, gravity. ThirdPerson: an
// orbit camera behind and above (camera_distance / camera_height; Q / Z or
// right-drag turn it). FirstPerson: the camera rides at eye_height above
// the rider's feet (or on a child entity with a `camera` component) and
// looks where the rider faces; right-drag / mouse looks up and down.
struct Player3D {
  enum class View : std::uint8_t { ThirdPerson, FirstPerson };
  float speed = 4.5f;        // m/s
  float jump_speed = 5.5f;   // m/s up
  float camera_distance = 6.0f;
  float camera_height = 2.5f;
  View view = View::ThirdPerson;  // "view": "first_person" in scene.json
  float eye_height = 1.6f;        // first person: metres above the feet
  // Looking about. Mouse (the default): the mouse turns the view (first
  // person: yaw on the body, pitch on the eyes; third person: it orbits the
  // camera) and Play captures the cursor. Keys: the old Q / Z turn, Page Up
  // / Page Down look and right-drag, cursor left alone. Q / Z, Page Up /
  // Page Down and a gamepad's right stick work in both modes.
  enum class Look : std::uint8_t { Mouse, Keys };
  static constexpr float kMinSensitivity = 0.01f;  // degrees per mouse count
  static constexpr float kMaxSensitivity = 2.0f;
  static constexpr float kMaxSmoothing = 0.9f;
  Look look = Look::Mouse;          // "look": "keys" in scene.json
  float mouse_sensitivity = 0.15f;  // degrees per mouse count
  bool invert_y = false;            // mouse / stick up looks down
  float smoothing = 0.0f;           // 0 = off .. 0.9 = heavy (per-tick ease)
  bool capture_cursor = true;       // hide + lock the cursor while riding
  bool operator==(const Player3D& o) const = default;
};

// A play camera (scene.json "camera"). With no rider in the scene, Play
// looks through the first one (its position, looking down its -Z). A
// child of a first-person rider marks the eye point instead.
struct PlayCamera3D {
  float fov = 60.0f;  // vertical, degrees
  bool operator==(const PlayCamera3D& o) const = default;
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
  // v12: a skinned right-hand glTF (its "grip" clip follows the squeeze;
  // mirrored for the left hand). Empty = the controller stand-ins.
  std::string hand_model;
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

// ---- Skeletal animation (v12) ------------------------------------------------
// Parameters drive transitions and blend spaces. A rider's animator gets
// "speed" (m/s across the ground), "grounded", "vertical_speed" and the
// "jump" trigger from the controller when they exist.
struct AnimParam3D {
  enum class Type : std::uint8_t { Float, Bool, Trigger };
  std::string name;
  Type type = Type::Float;
  float value = 0.0f;  // bool / trigger: 0 or 1
  bool operator==(const AnimParam3D& o) const = default;
};

// A 1D blend-space point: `clip` at parameter value `at`.
struct AnimBlendPoint3D {
  std::string clip;
  float at = 0.0f;
  bool operator==(const AnimBlendPoint3D& o) const = default;
};

// One clip, or a 1D blend space (blend + blend_param) whose clips play in
// step (normalised time).
struct AnimState3D {
  std::string name;
  std::string clip;
  std::vector<AnimBlendPoint3D> blend;
  std::string blend_param;
  float speed = 1.0f;
  bool loop = true;
  bool operator==(const AnimState3D& o) const = default;
};

struct AnimTransition3D {
  enum class Op : std::uint8_t { Greater, Less, True, False, Trigger, Always };
  std::string from = "*";  // state name, "*" = any
  std::string to;
  std::string param;
  Op op = Op::Greater;
  float value = 0.0f;
  float duration = 0.2f;   // crossfade seconds
  bool at_end = false;     // only once a non-looping state finished
  bool operator==(const AnimTransition3D& o) const = default;
};

// Layers stack bottom to top. A layer's mask (bone names; each takes its
// whole subtree, empty = all bones) limits what it overrides: "chest" on
// an upper-body layer waves while the legs keep walking.
struct AnimLayer3D {
  std::string name = "Base";
  std::vector<std::string> mask;
  float weight = 1.0f;
  std::string entry;  // first state ("" = the first listed)
  std::vector<AnimState3D> states;
  std::vector<AnimTransition3D> transitions;
  bool operator==(const AnimLayer3D& o) const = default;
};

// Named moments in a clip (footsteps, ...): scripts get
// on_anim_event(name) when playback crosses `time` (seconds).
struct AnimEvent3D {
  std::string clip;
  float time = 0.0f;
  std::string name;
  bool operator==(const AnimEvent3D& o) const = default;
};

struct Animator3D {
  std::string model;  // skinned glTF; "" = the entity's mesh
  std::vector<AnimParam3D> params;
  std::vector<AnimLayer3D> layers;
  std::vector<AnimEvent3D> events;
  float speed = 1.0f;
  // Root motion: the root bone's ground travel moves the entity (the bone
  // stays put). Off for riders (the controller moves them).
  bool root_motion = false;
  std::string root_bone;  // "" = the skeleton's first joint
  // The rider controller feeds speed / grounded / jump / vertical_speed.
  bool drive_from_player = true;
  bool operator==(const Animator3D& o) const = default;
};

// idle / walk / run blend space by "speed", a "jump" state, and an
// "Upper Body" layer (mask: chest) for one-shots like "wave".
Animator3D default_locomotion_animator();

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
  std::optional<fx::EmitterDesc> particles;  // v11, metres, y up
  std::optional<Animator3D> animator;        // v12
  std::optional<Agent3D> agent;              // v13
  std::optional<NavLink3D> nav_link;         // v13
  // Runtime only (never saved): the skinning palette the animator posed
  // this tick; SceneView draws the skinned mesh with it (rest pose when
  // null). Shared so world copies stay cheap.
  std::shared_ptr<const std::vector<render3d::Mat4>> skin_palette;
  std::optional<RigidBody3D> rigid_body;     // v14
  std::optional<Joint3D> joint;              // v14
  std::optional<prefab::Link> prefab;        // v15
  std::optional<terrain::Terrain3D> terrain; // v16
  std::optional<PlayCamera3D> camera;        // v16 (optional, additive)
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
  PhysicsSettings3D physics;  // v14
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

// One entity as its scene.json object and back (prefab tools).
json::Value entity_to_json(const Entity3D& e);
bool entity_from_json(const json::Value& v, Entity3D* out, std::string* error);
std::vector<json::Value> entities_to_json(const std::vector<Entity3D>& entities);
bool entities_from_json(const std::vector<json::Value>& values,
                        std::vector<Entity3D>* out, std::string* error);

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
