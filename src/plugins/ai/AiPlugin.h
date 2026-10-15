#pragma once

// ts.ai "Pathfinder": the AI toolkit as an optional plugin. 2D worlds get
// agents (grid A* / JPS / HPA* paths, flow fields, steering), behaviour
// trees (*.bt.json), perception (sight cone + hearing from sound events) and
// the Lua `ai` table, which graphs, C++ and C# reach through the plugin
// bridge. Runs on Local / Server worlds only (clients see replicated
// positions). The 3D side lives in ts_ai3d (World3D extension).

#include "ai/AgentBrain.h"
#include "ai/GridPath.h"
#include "plugin/Plugin.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugins {

class AiPlugin : public plugin::Plugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }
  void on_register(plugin::Registrar& reg) override;
  void on_play_begin(runtime::World& world) override;
  void on_play_end() override;
  ~AiPlugin() override;

  struct Body2D;
  struct Agent {
    std::uint64_t id = 0;
    std::string bt_path;
    std::unique_ptr<Body2D> body;
    std::unique_ptr<ai::AgentBrain> brain;
  };
  struct WorldAi;

  // ---- C++ API (also what the Lua table calls) -----------------------------
  // Every function takes the world it acts on; nothing happens on a client.
  bool add_agent(runtime::World& w, std::uint64_t id, const ai::AgentParams& p,
                 const ai::PerceptionConfig& senses = {});
  void remove_agent(runtime::World& w, std::uint64_t id);
  Agent* agent(const runtime::World& w, std::uint64_t id);
  std::vector<const Agent*> agents(const runtime::World& w) const;
  bool load_tree(runtime::World& w, std::uint64_t id, const std::string& rel_path, std::string* error = nullptr);
  bool move_to(runtime::World& w, std::uint64_t id, float x, float y);
  // World-px path (cell centres, smoothed); false when unreachable.
  bool find_path(runtime::World& w, float x0, float y0, float x1, float y1, std::vector<ai::V3>* out);
  bool flow(runtime::World& w, float gx, float gy, float x, float y, float* dx, float* dy);
  void noise(runtime::World& w, float x, float y, float loudness, const std::string& what);
  void invalidate(runtime::World& w);
  void set_cell(runtime::World& w, float px);
  void set_debug(bool on) { debug_ = on; }
  bool debug() const { return debug_; }
  // Smokes / editor.
  std::size_t agent_count(const runtime::World& w) const;
  bool active(const runtime::World& w) const;

  // Frontier coyote hook body (see FrontierSim::set_mob_steer).
  using WalkPx = std::function<bool(float x, float y)>;
  bool steer_around(const runtime::World& w, std::uint32_t key, float x, float y, float tx, float ty,
                    float tile, const WalkPx& walk, float* dx, float* dy);
  std::uint64_t coyote_plans() const { return coyote_plans_; }

  static AiPlugin* instance();

 private:
  WorldAi& state(runtime::World& w);
  const WorldAi* find_state(const runtime::World& w) const;
  void run(runtime::World& w, double dt);
  void draw(const runtime::World& w, const runtime::WorldRect& view, std::vector<runtime::DrawQuad>* out);

  std::map<const runtime::World*, std::unique_ptr<WorldAi>> worlds_;
  runtime::World* current_ = nullptr;
  bool debug_ = false;
  std::uint64_t coyote_plans_ = 0;
  struct CoyotePlan {
    std::vector<ai::Cell> cells;
    std::size_t next = 0;
    int goal_x = 0, goal_y = 0;
    std::uint64_t made = 0;
  };
  std::map<std::pair<const runtime::World*, std::uint32_t>, CoyotePlan> coyotes_;
  std::uint64_t coyote_tick_ = 0;
};

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
