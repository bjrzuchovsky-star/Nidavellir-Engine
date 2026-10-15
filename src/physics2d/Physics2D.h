#pragma once

// ts_physics2d: 2D rigid bodies on Box2D v3 (hidden behind this header) in
// the engine's own units: pixels, y down, angles in degrees clockwise on
// screen (Box2D works in metres; pixels_per_metre converts). Bodies are
// static / kinematic / dynamic with box (rotated), circle, capsule and
// convex polygon shapes; joints (weld, revolute, prismatic, distance) with
// limits; ray casts; sensors; hit events with the approach speed; and a
// state hash (Box2D v3 is cross-platform deterministic).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace physics2d {

struct Vec2 {
  float x = 0.0f, y = 0.0f;
  bool operator==(const Vec2& o) const = default;
};

enum class Motion : std::uint8_t { Static, Kinematic, Dynamic };
enum class ShapeType : std::uint8_t { Box, Circle, Capsule, Polygon };

struct ShapeDesc {
  ShapeType type = ShapeType::Box;
  float w = 32.0f, h = 32.0f;  // box: full size; capsule: w x h (round ends on the long axis)
  float radius = 16.0f;        // circle
  Vec2 offset;                 // centre, body space (px)
  float angle = 0.0f;          // box: degrees in body space
  std::vector<Vec2> points;    // polygon (convex hull of them, <= 8)
};

struct BodyDesc {
  std::uint64_t user = 0;
  Motion motion = Motion::Dynamic;
  std::vector<ShapeDesc> shapes;
  Vec2 position;  // body origin (px)
  float angle = 0.0f;
  float density = 1.0f;  // kg / m^2
  float friction = 0.6f;
  float restitution = 0.0f;
  float linear_damping = 0.0f;
  float angular_damping = 0.05f;
  float gravity_scale = 1.0f;
  bool fixed_rotation = false;
  bool bullet = false;
  bool sensor = false;
  bool events = true;  // sensor visits + hit events
  std::uint32_t category = 1, mask = 0xffffffffu;
};

using BodyId = std::uint32_t;  // 0 = none
using JointId = std::uint32_t;

struct RayHit {
  BodyId body = 0;
  std::uint64_t user = 0;
  Vec2 point, normal;
  float distance = 0.0f;
};

struct Contact {
  std::uint64_t a = 0, b = 0;
  Vec2 point, normal;
  float speed = 0.0f;  // px / s
};

struct SensorEvent {
  std::uint64_t sensor = 0, other = 0;
  bool enter = true;
};

enum class JointType : std::uint8_t { Weld, Revolute, Prismatic, Distance };

struct JointDesc {
  JointType type = JointType::Revolute;
  BodyId a = 0, b = 0;  // a = 0: pinned to the world
  Vec2 anchor;          // world px (distance: the end on a)
  Vec2 anchor_b;        // distance: the end on b
  Vec2 axis{1.0f, 0.0f};  // prismatic
  bool limits = false;
  float min = 0.0f, max = 0.0f;  // revolute: degrees; prismatic / distance: px
};

class World {
 public:
  // gravity in px / s^2 (y down).
  explicit World(Vec2 gravity = {0.0f, 980.0f}, float pixels_per_metre = 32.0f);
  ~World();
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  float pixels_per_metre() const;
  void set_gravity(Vec2 g);

  BodyId create(const BodyDesc& desc, std::string* error = nullptr);
  void destroy(BodyId id);
  bool exists(BodyId id) const;
  std::size_t body_count() const;
  std::uint64_t user(BodyId id) const;
  Motion motion(BodyId id) const;

  Vec2 position(BodyId id) const;
  float angle(BodyId id) const;
  void set_transform(BodyId id, Vec2 p, float angle);
  Vec2 velocity(BodyId id) const;
  float angular_velocity(BodyId id) const;  // deg / s
  void set_velocity(BodyId id, Vec2 v);
  void set_angular_velocity(BodyId id, float deg_per_s);
  // Kinematic: arrive at p / angle after dt (pushes dynamic bodies).
  void move_kinematic(BodyId id, Vec2 p, float angle, float dt);
  void add_force(BodyId id, Vec2 f);     // kg px / s^2
  void add_impulse(BodyId id, Vec2 j);   // kg px / s
  void add_torque(BodyId id, float t);   // kg px^2 / s^2
  // Override mass (kg) and rotational inertia (kg px^2) about the origin.
  void set_mass(BodyId id, float mass, float inertia);
  float mass(BodyId id) const;
  bool awake(BodyId id) const;

  bool raycast(Vec2 from, Vec2 dir, float max_dist, RayHit* hit) const;

  JointId add_joint(const JointDesc& desc, std::string* error = nullptr);
  void remove_joint(JointId id);
  std::size_t joint_count() const;
  // Revolute: degrees; prismatic: px; distance: px.
  float joint_value(JointId id) const;

  void step(float dt, int sub_steps = 4);
  const std::vector<Contact>& contacts() const;
  const std::vector<SensorEvent>& sensor_events() const;
  std::uint64_t state_hash() const;

  struct DebugShape {
    std::vector<Vec2> points;  // closed outline, world px
    Motion motion = Motion::Static;
    bool sensor = false;
  };
  std::vector<DebugShape> debug_shapes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace physics2d
}  // namespace tombstone
}  // namespace ts
