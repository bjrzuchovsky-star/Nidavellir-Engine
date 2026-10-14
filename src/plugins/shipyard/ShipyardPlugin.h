#pragma once

// "Airship Works" (ts.shipyard): Cosmoteer-style ships for 2D projects.
// Glues ShipSim to the World: a system that flies the ships, draw layers
// (sky under the actors; ships, shot and the helm gauges over them), the
// "shipyard" net channel and the `ships` Lua table. Designs are
// .ship.json files under shipyard/designs, parts come from
// shipyard/parts.json, the encounter from shipyard/sky.json. Off by
// default; switch it on in Outfitter's Wagon.

#include "plugin/Plugin.h"
#include "plugins/shipyard/ShipSim.h"

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace shipyard {

class ShipyardPlugin : public plugin::Plugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }

  bool on_load(const plugin::PluginContext& ctx, std::string* error) override;
  void on_register(plugin::Registrar& reg) override;
  void on_play_begin(runtime::World& world) override;
  void on_play_end() override;
  void on_unload() override;

  ShipSim& sim() { return sim_; }
  const ShipSim& sim() const { return sim_; }
  const PartLibrary& lib() const { return lib_; }
  const SkySettings& sky() const { return sky_; }
  const std::string& project_dir() const { return project_dir_; }
  runtime::World* world() { return world_; }
  const std::vector<std::string>& data_warnings() const { return warnings_; }
  // Re-read parts.json and sky.json (the Airship Works after an edit).
  bool reload_data(std::string* error);

  // ts_editor registers the Airship Works panel through this (the runtime
  // library carries no ImGui).
  using EditorHook = std::function<void(ShipyardPlugin&, plugin::Registrar&)>;
  static void set_editor_hook(EditorHook hook);
  static ShipyardPlugin* current();

 private:
  void draw_sky(const runtime::World& w, const runtime::WorldRect& view,
                std::vector<runtime::DrawQuad>* out) const;
  void draw_ships(const runtime::World& w, const runtime::WorldRect& view, float alpha,
                  std::vector<runtime::DrawQuad>* out) const;

  ShipSim sim_;
  PartLibrary lib_;
  SkySettings sky_;
  std::vector<std::string> warnings_;
  runtime::World* world_ = nullptr;
  std::string project_dir_;
  std::string host_;
};

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
