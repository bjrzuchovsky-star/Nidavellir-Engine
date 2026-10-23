#pragma once

// Box2DWorld: physics2d::World on Box2D v3 (the ts.physics.box2d Trading
// Post plugin). Construct it directly in tests; the engine gets it through
// physics2d::create_world() once the plugin is on for the claim.

#include "physics2d/Physics2D.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace physics2d {

class Box2DWorld final : public World {
 public:
  // gravity in px / s^2 (y down).
  explicit Box2DWorld(Vec2 gravity = {0.0f, 980.0f}, float pixels_per_metre = 32.0f);
  ~Box2DWorld() override;

  float pixels_per_metre() const override;
  void set_gravity(Vec2 g) override;
  BodyId create(const BodyDesc& desc, std::string* error = nullptr) override;
  void destroy(BodyId id) override;
  bool exists(BodyId id) const override;
  std::size_t body_count() const override;
  std::uint64_t user(BodyId id) const override;
  Motion motion(BodyId id) const override;
  Vec2 position(BodyId id) const override;
  float angle(BodyId id) const override;
  void set_transform(BodyId id, Vec2 p, float angle) override;
  Vec2 velocity(BodyId id) const override;
  float angular_velocity(BodyId id) const override;
  void set_velocity(BodyId id, Vec2 v) override;
  void set_angular_velocity(BodyId id, float deg_per_s) override;
  void move_kinematic(BodyId id, Vec2 p, float angle, float dt) override;
  void add_force(BodyId id, Vec2 f) override;
  void add_impulse(BodyId id, Vec2 j) override;
  void add_torque(BodyId id, float t) override;
  void set_mass(BodyId id, float mass, float inertia) override;
  float mass(BodyId id) const override;
  bool awake(BodyId id) const override;
  bool raycast(Vec2 from, Vec2 dir, float max_dist, RayHit* hit) const override;
  JointId add_joint(const JointDesc& desc, std::string* error = nullptr) override;
  void remove_joint(JointId id) override;
  std::size_t joint_count() const override;
  float joint_value(JointId id) const override;
  void step(float dt, int sub_steps = 4) override;
  const std::vector<Contact>& contacts() const override;
  const std::vector<SensorEvent>& sensor_events() const override;
  std::uint64_t state_hash() const override;
  std::vector<DebugShape> debug_shapes() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Makes the backend available to create_world() under kBox2DPluginId (it
// only answers while the plugin holds the id). Idempotent.
void offer_box2d();

}  // namespace physics2d
}  // namespace tombstone
}  // namespace ts
