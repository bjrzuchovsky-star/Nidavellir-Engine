#pragma once

// JoltWorld: physics3d::World on Jolt Physics (the ts.physics.jolt Trading
// Post plugin). Construct it directly in tests; the engine gets it through
// physics3d::create_world() once the plugin is on for the claim.

#include "physics3d/Physics3D.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace physics3d {

class JoltWorld final : public World {
 public:
  explicit JoltWorld(const Settings& settings = Settings{});
  ~JoltWorld() override;

  const Settings& settings() const override;
  void set_settings(const Settings& s) override;
  BodyId create(const BodyDesc& desc, std::string* error = nullptr) override;
  void destroy(BodyId id) override;
  bool exists(BodyId id) const override;
  std::size_t body_count() const override;
  std::uint64_t user(BodyId id) const override;
  BodyId find_user(std::uint64_t user) const override;
  Motion motion(BodyId id) const override;
  bool sensor(BodyId id) const override;
  std::uint8_t layer(BodyId id) const override;
  void set_layer(BodyId id, std::uint8_t layer) override;
  float mass(BodyId id) const override;
  Vec3 position(BodyId id) const override;
  Quat rotation(BodyId id) const override;
  void set_transform(BodyId id, const Vec3& p, const Quat& q, bool wake = true) override;
  void move_kinematic(BodyId id, const Vec3& p, const Quat& q, float dt) override;
  Vec3 velocity(BodyId id) const override;
  Vec3 angular_velocity(BodyId id) const override;
  void set_velocity(BodyId id, const Vec3& v) override;
  void set_angular_velocity(BodyId id, const Vec3& w) override;
  void add_force(BodyId id, const Vec3& f) override;
  void add_force_at(BodyId id, const Vec3& f, const Vec3& point) override;
  void add_torque(BodyId id, const Vec3& t) override;
  void add_impulse(BodyId id, const Vec3& j) override;
  void add_impulse_at(BodyId id, const Vec3& j, const Vec3& point) override;
  void set_gravity_scale(BodyId id, float s) override;
  bool awake(BodyId id) const override;
  void wake(BodyId id) override;
  bool raycast(const Vec3& from, const Vec3& dir, float max_dist, RayHit* hit, std::uint8_t mask = 0xff, BodyId ignore = 0) const override;
  bool sphere_cast(const Vec3& from, float radius, const Vec3& dir, float max_dist, RayHit* hit, std::uint8_t mask = 0xff, BodyId ignore = 0) const override;
  bool box_cast(const Vec3& from, const Vec3& half, const Quat& rot, const Vec3& dir, float max_dist, RayHit* hit, std::uint8_t mask = 0xff, BodyId ignore = 0) const override;
  std::vector<BodyId> overlap_sphere(const Vec3& centre, float radius, bool sensors = false) const override;
  std::vector<BodyId> overlap_box(const Vec3& centre, const Vec3& half, const Quat& rot, bool sensors = false) const override;
  JointId add_joint(const JointDesc& desc, std::string* error = nullptr) override;
  void remove_joint(JointId id) override;
  std::size_t joint_count() const override;
  float joint_value(JointId id) const override;
  bool create_character(const CharacterDesc& desc, const Vec3& position) override;
  bool has_character() const override;
  void destroy_character() override;
  CharacterState move_character(const Vec3& velocity, float dt) override;
  void set_character_position(const Vec3& p) override;
  CharacterState character() const override;
  void step(float dt) override;
  std::uint64_t steps() const override;
  const std::vector<Contact>& contacts() const override;
  const std::vector<SensorEvent>& sensor_events() const override;
  std::uint64_t state_hash() const override;
  std::vector<DebugBox> debug_boxes() const override;
  std::vector<BodyId> bodies() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Makes the backend available to create_world() under kJoltPluginId (it
// only answers while the plugin holds the id). Idempotent.
void offer_jolt();

}  // namespace physics3d
}  // namespace tombstone
}  // namespace ts
