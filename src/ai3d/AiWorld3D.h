#pragma once

// ts.ai in 3D: a World3D extension. On start it bakes a Recast navmesh from
// the scene's static colliders (plus "nav_link" off-mesh links), puts every
// entity with an "agent" component into a Detour crowd (speed, radius,
// avoidance) and runs its behaviour tree through AgentBrain. Perception: the
// rider is seen through a sight cone (navmesh raycast for line of sight),
// positional sounds are heard. Lua gets the `ai` table; the editor gets
// debug lines (navmesh, paths, BT state). Hosts attach it only when the
// project enables ts.ai. World3D has no network roles yet; `authority`
// false (a future client) keeps it from simulating.

#include "ai/AgentBrain.h"
#include "ai/NavMesh.h"
#include "world3d/World3D.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace ai3d {

class AiWorld3D final : public world3d::World3DExtension {
 public:
  static constexpr const char* kName = "ts.ai";
  AiWorld3D();
  ~AiWorld3D() override;

  const char* name() const override { return kName; }
  void on_start(world3d::World3D& w) override;
  void on_tick(world3d::World3D& w, double dt) override;
  void on_stop(world3d::World3D& w) override;
  void on_sound(world3d::World3D& w, const runtime::SoundEvent& ev) override;
  void open_lua(world3d::World3D& w, lua_State* L, int sandbox) override;
  void debug_lines(const world3d::World3D& w, std::vector<world3d::World3DExtension::Line>* out) const override;

  void set_authority(bool on) { authority_ = on; }
  void set_debug(bool on) { debug_ = on; }
  bool debug() const { return debug_; }

  // C++ API (the Lua table calls these).
  bool add_agent(std::uint64_t id, const scene3d::Agent3D& cfg);
  void remove_agent(std::uint64_t id);
  bool load_tree(std::uint64_t id, const std::string& rel, std::string* error = nullptr);
  bool move_to(std::uint64_t id, const ai::V3& goal);
  void stop(std::uint64_t id);
  ai::NavPath find_path(const ai::V3& a, const ai::V3& b) const;
  void noise(const ai::V3& at, float loudness, const std::string& what);

  struct Agent;
  Agent* agent(std::uint64_t id);
  ai::AgentBrain* brain(std::uint64_t id);
  std::vector<std::uint64_t> agent_ids() const;
  const ai::NavMesh& navmesh() const { return *mesh_; }
  const ai::NavCrowd* crowd() const { return crowd_.get(); }
  const std::string& build_error() const { return build_error_; }
  double build_ms() const { return build_ms_; }
  world3d::World3D* world() const { return world_; }

  // The Editor BT panel reads live state through this (null when not riding).
  static AiWorld3D* of(const world3d::World3D& w);

 private:
  void rebuild();

  world3d::World3D* world_ = nullptr;
  std::unique_ptr<ai::NavMesh> mesh_;
  std::unique_ptr<ai::NavCrowd> crowd_;
  std::map<std::uint64_t, std::unique_ptr<Agent>> agents_;
  std::vector<ai::V3> edges_;  // cached navmesh debug edges
  std::string build_error_;
  double build_ms_ = 0.0;
  bool authority_ = true;
  bool debug_ = false;
  std::uint64_t seed_ = 0x51ED270B27C3A1F5ull;
};

// Attach to a World3D (before start) when `enabled`; returns the extension.
std::shared_ptr<AiWorld3D> attach(world3d::World3D& w, bool enabled);

}  // namespace ai3d
}  // namespace tombstone
}  // namespace ts
