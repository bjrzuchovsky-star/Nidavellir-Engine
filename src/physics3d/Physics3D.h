#pragma once

// ts_physics3d: the 3D rigid-body world (Jolt Physics underneath, hidden
// behind this header). Bodies (static / kinematic / dynamic) built from box,
// sphere, capsule, convex-hull and triangle-mesh shapes or a compound of
// them, eight collision layers with a symmetric collision matrix, sensors
// (triggers), ray / shape casts, five joint kinds (fixed, hinge, slider,
// distance, ball), contact events carrying the approach speed and an
// impulse estimate, and a character controller.
//
// Single-threaded and fixed-step: the same calls in the same order give the
// same simulation (Jolt is built cross-platform deterministic), so a
// server can own the bodies and clients just replicate transforms.

#include "render3d/Math3D.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace physics3d {

using render3d::Quat;
using render3d::Vec3;

inline constexpr int kLayerCount = 8;
// Default layer roles (names are editable per scene).
enum Layer : std::uint8_t {
  kLayerStatic = 0,
  kLayerDefault = 1,
  kLayerHeld = 2,
  kLayerPlayer = 3,
  kLayerDebris = 4,
  kLayerTrigger = 5,
  kLayerCustomA = 6,
  kLayerCustomB = 7,
};
const char* default_layer_name(int layer);

enum class Motion : std::uint8_t { Static, Kinematic, Dynamic };
const char* motion_name(Motion m);

enum class ShapeType : std::uint8_t { Box, Sphere, Capsule, ConvexHull, Mesh };
const char* shape_name(ShapeType s);

struct ShapeDesc {
  ShapeType type = ShapeType::Box;
  // Box: full extents. Sphere: x = radius. Capsule: x = radius, y = total
  // height (caps included).
  Vec3 size{1.0f, 1.0f, 1.0f};
  Vec3 offset;      // in body space
  Quat rotation;    // in body space
  // Convex hull / mesh: body-space points (and triangles for a mesh).
  std::vector<Vec3> points;
  std::vector<std::uint32_t> indices;
};

struct BodyDesc {
  std::uint64_t user = 0;  // the entity id
  Motion motion = Motion::Static;
  std::vector<ShapeDesc> shapes;  // more than one -> compound
  Vec3 position;
  Quat rotation;
  float mass = 0.0f;  // kg; 0 = from the shape volume (water density)
  float friction = 0.5f;
  float restitution = 0.1f;
  float linear_damping = 0.05f;
  float angular_damping = 0.05f;
  float gravity_scale = 1.0f;
  bool ccd = false;      // swept motion for fast, small things
  bool sensor = false;   // trigger: reports overlaps, never blocks
  bool allow_sleep = true;
  std::uint8_t layer = kLayerDefault;
};

using BodyId = std::uint32_t;  // 0 = none
using JointId = std::uint32_t;

struct RayHit {
  BodyId body = 0;
  std::uint64_t user = 0;
  float distance = 0.0f;
  Vec3 point;
  Vec3 normal;
};

struct Contact {
  BodyId a = 0, b = 0;
  std::uint64_t user_a = 0, user_b = 0;
  Vec3 point;
  Vec3 normal;           // from a to b
  float speed = 0.0f;    // approach speed along the normal, m/s
  float impulse = 0.0f;  // speed * reduced mass, N s (an estimate)
};

struct SensorEvent {
  BodyId sensor = 0, other = 0;
  std::uint64_t sensor_user = 0, other_user = 0;
  bool enter = true;
};

enum class JointType : std::uint8_t { Fixed, Hinge, Slider, Distance, Ball };
const char* joint_name(JointType t);

struct JointDesc {
  JointType type = JointType::Fixed;
  BodyId a = 0;  // 0 = the world
  BodyId b = 0;
  Vec3 anchor;            // world space (distance: the end on a)
  Vec3 anchor_b;          // distance only: the end on b
  Vec3 axis{0, 1, 0};     // hinge / slider axis, world space
  bool limits = false;
  float min = 0.0f;       // hinge: radians, slider: metres, distance: metres
  float max = 0.0f;
};

struct Settings {
  Vec3 gravity{0.0f, -9.81f, 0.0f};
  int collision_steps = 1;  // sub-steps per step()
  // collide[i] bit j: layers i and j touch (kept symmetric).
  std::array<std::uint8_t, kLayerCount> collide{};
  Settings();
  bool collides(int a, int b) const { return (collide[a & 7] >> (b & 7)) & 1u; }
  void set_collides(int a, int b, bool on);
};

struct CharacterDesc {
  float radius = 0.4f;
  float height = 2.0f;   // total
  Vec3 offset;           // shape centre relative to the character position
  float max_slope_deg = 50.0f;
  float mass = 80.0f;
  float push_strength = 300.0f;  // N, how hard it shoves dynamic bodies
  float step_up = 0.35f;         // stairs
};

struct CharacterState {
  Vec3 position;
  Vec3 velocity;
  bool grounded = false;
  Vec3 ground_normal{0, 1, 0};
  int contacts = 0;
};

class World {
 public:
  explicit World(const Settings& settings = Settings{});
  ~World();
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  const Settings& settings() const;
  void set_settings(const Settings& s);  // gravity + matrix, live

  // Bodies. create() returns 0 and the reason when the shape is bad
  // (degenerate hull, empty mesh, a dynamic mesh).
  BodyId create(const BodyDesc& desc, std::string* error = nullptr);
  void destroy(BodyId id);
  bool exists(BodyId id) const;
  std::size_t body_count() const;
  std::uint64_t user(BodyId id) const;
  BodyId find_user(std::uint64_t user) const;
  Motion motion(BodyId id) const;
  bool sensor(BodyId id) const;
  std::uint8_t layer(BodyId id) const;
  void set_layer(BodyId id, std::uint8_t layer);
  float mass(BodyId id) const;  // 0 for static / kinematic

  Vec3 position(BodyId id) const;
  Quat rotation(BodyId id) const;
  void set_transform(BodyId id, const Vec3& p, const Quat& q, bool wake = true);
  // Kinematic: reach p / q at the end of the next step (pushes dynamics).
  void move_kinematic(BodyId id, const Vec3& p, const Quat& q, float dt);
  Vec3 velocity(BodyId id) const;
  Vec3 angular_velocity(BodyId id) const;
  void set_velocity(BodyId id, const Vec3& v);
  void set_angular_velocity(BodyId id, const Vec3& w);
  void add_force(BodyId id, const Vec3& f);          // until the next step
  void add_force_at(BodyId id, const Vec3& f, const Vec3& point);
  void add_torque(BodyId id, const Vec3& t);
  void add_impulse(BodyId id, const Vec3& j);
  void add_impulse_at(BodyId id, const Vec3& j, const Vec3& point);
  void set_gravity_scale(BodyId id, float s);
  bool awake(BodyId id) const;
  void wake(BodyId id);

  // Queries (sensors are skipped). mask: layers to hit (bit per layer).
  bool raycast(const Vec3& from, const Vec3& dir, float max_dist, RayHit* hit,
               std::uint8_t mask = 0xff, BodyId ignore = 0) const;
  bool sphere_cast(const Vec3& from, float radius, const Vec3& dir, float max_dist,
                   RayHit* hit, std::uint8_t mask = 0xff, BodyId ignore = 0) const;
  bool box_cast(const Vec3& from, const Vec3& half, const Quat& rot, const Vec3& dir,
                float max_dist, RayHit* hit, std::uint8_t mask = 0xff,
                BodyId ignore = 0) const;
  // Bodies (sensors included when asked) overlapping a sphere / box.
  std::vector<BodyId> overlap_sphere(const Vec3& centre, float radius,
                                     bool sensors = false) const;
  std::vector<BodyId> overlap_box(const Vec3& centre, const Vec3& half, const Quat& rot,
                                  bool sensors = false) const;

  // Joints between two bodies (a or b = 0: pinned to the world).
  JointId add_joint(const JointDesc& desc, std::string* error = nullptr);
  void remove_joint(JointId id);
  std::size_t joint_count() const;
  // Hinge: angle (rad); slider: position (m); distance: length (m).
  float joint_value(JointId id) const;

  // Character controller (one per world is the common case).
  bool create_character(const CharacterDesc& desc, const Vec3& position);
  bool has_character() const;
  void destroy_character();
  // Move with the wanted velocity (gravity is the caller's) for dt seconds.
  CharacterState move_character(const Vec3& velocity, float dt);
  void set_character_position(const Vec3& p);
  CharacterState character() const;

  // Advance the world by dt (fixed step).
  void step(float dt);
  std::uint64_t steps() const;
  // Events from the last step().
  const std::vector<Contact>& contacts() const;
  const std::vector<SensorEvent>& sensor_events() const;

  // Order-independent hash of every body's position / rotation / velocity
  // bits (determinism checks; same inputs -> same hash on every platform).
  std::uint64_t state_hash() const;

  // Debug draw: wire boxes for each body's world bounds, coloured by kind.
  struct DebugBox {
    Vec3 min, max;
    Motion motion = Motion::Static;
    bool sensor = false;
    bool sleeping = false;
  };
  std::vector<DebugBox> debug_boxes() const;
  std::vector<BodyId> bodies() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace physics3d
}  // namespace tombstone
}  // namespace ts
