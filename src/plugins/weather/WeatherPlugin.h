#pragma once

// ts.weather "Weather": rain, snow, dust storms, fog and thunder storms
// with lightning, gusting wind and an optional time-of-day link.
//   2D: this plugin draws the weather (streaks, flakes, dust and fog puffs
//       as world-space quads, a HUD tint and lightning flash), darkens /
//       flashes the 2D lighting ambient, throws a lightning light that casts
//       shadows, and pushes every particle emitter with the wind.
//   3D / VR: hosts attach weather::Weather3D (environment fog, sky, sun,
//       foliage wind, rain / snow / dust billboards).
// Both read weather.json and expose the same Lua `weather` table.

#include "plugin/Plugin.h"
#include "weather/Weather.h"

#include <functional>
#include <string>

namespace ts {
namespace tombstone {
namespace plugins {

class WeatherPlugin : public plugin::Plugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }
  bool on_load(const plugin::PluginContext& ctx, std::string* error) override;
  void on_register(plugin::Registrar& reg) override;
  void on_play_begin(runtime::World& world) override;
  void on_play_end() override;

  weather::Sim& sim() { return sim_; }
  const weather::Config& config() const { return cfg_; }
  void set_config(const weather::Config& c);
  bool save_config(std::string* error) const;
  const std::string& project_dir() const { return dir_; }
  bool playing() const { return playing_; }

  using EditorHook = std::function<void(WeatherPlugin&, plugin::Registrar&)>;
  static void set_editor_hook(EditorHook hook);
  static WeatherPlugin* active();  // the loaded instance (editor panels)

  // 2D drawing (also used by smokes): quads for a view rect at the sim state.
  static void draw_world(const weather::State& s, const runtime::WorldRect& view,
                         std::vector<runtime::DrawQuad>* out);
  static void draw_hud(const weather::State& s, const runtime::WorldRect& view,
                       std::vector<runtime::DrawQuad>* out);
  // Ambient (multiply) for the 2D lightmap given the scene's own.
  static void ambient(const weather::State& s, const weather::Config& c, const float scene[3],
                      bool scene_cycles, float out[3]);

  ~WeatherPlugin() override;

 private:
  weather::Sim sim_;
  weather::Config cfg_;
  std::string dir_;
  std::string host_;
  bool playing_ = false;
  std::uint64_t strikes_seen_ = 0;
  weather::Kind announced_ = weather::Kind::Clear;
  runtime::World* world_ = nullptr;
};

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
