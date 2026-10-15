#pragma once

// "Frontier Survival" (ts.frontier): the gameplay plugin gluing FrontierSim
// to the World: a system that simulates, draw layers (tiles under the
// actors; creatures, shots, night and the hotbar over them), a blocker (solid
// tiles), the "frontier" net channel and the `frontier` Lua table. 2D only,
// off by default; switch it on in Outfitter's Wagon.

#include "plugin/Plugin.h"
#include "plugins/survival/FrontierSim.h"

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace survival {

class SurvivalPlugin : public plugin::Plugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }

  bool on_load(const plugin::PluginContext& ctx, std::string* error) override;
  void on_register(plugin::Registrar& reg) override;
  void on_play_begin(runtime::World& world) override;
  void on_play_end() override;
  void on_unload() override;

  FrontierSim& sim() { return sim_; }
  const FrontierSim& sim() const { return sim_; }
  const std::string& project_dir() const { return project_dir_; }
  const std::string& host() const { return host_; }
  // Smokes: keep rides in memory (no saves/ folder writes).
  void set_saving(bool on) { saving_ = on; }
  // Reload the data files (the Prospector after an edit).
  bool reload_data(std::string* error);
  const GameData& data() const { return data_; }
  runtime::World* world() { return world_; }
  const std::vector<std::string>& data_warnings() const { return data_warnings_; }

  // The editor registers its panels through this hook (ts_editor sets it;
  // the runtime library carries no ImGui).
  using EditorHook = std::function<void(SurvivalPlugin&, plugin::Registrar&)>;
  static void set_editor_hook(EditorHook hook);

  // The most recently loaded instance (editor panels, smokes).
  static SurvivalPlugin* current();

 private:
  void draw_under(const runtime::World& w, const runtime::WorldRect& view,
                  std::vector<runtime::DrawQuad>* out);
  void draw_over(const runtime::World& w, const runtime::WorldRect& view,
                 std::vector<runtime::DrawQuad>* out);
  void draw_hud(const runtime::World& w, const runtime::WorldRect& view,
                std::vector<runtime::DrawQuad>* out);
  void ensure_client(const runtime::World& w);

  FrontierSim sim_;
  runtime::World* world_ = nullptr;  // during a ride
  GameData data_;
  std::vector<std::string> data_warnings_;
  std::string project_dir_;
  std::string host_;
  bool saving_ = true;
};

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
