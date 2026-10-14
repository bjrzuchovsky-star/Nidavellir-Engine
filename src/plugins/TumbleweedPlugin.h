#pragma once

// Sample built-in plugin: tumbleweeds. Any non-rider entity whose name
// starts with "Tumbleweed" rolls with the wind during a ride, bobbing as it
// goes and starting over after kRollDistance px. Scripts get a
// `tumbleweed` table (wind(), wind(px_per_s), count(), gusts()), the
// project gets a "Tumbleweed" component name and the ".tumble.json" asset
// type. Small on purpose: it is the reference for writing gameplay
// plugins (a Necesse-style crafting module or a Cosmoteer-style ship grid
// would follow the same shape).

#include "plugin/Plugin.h"

#include <cstdint>
#include <map>
#include <string>

namespace ts {
namespace tombstone {
namespace plugins {

class TumbleweedPlugin final : public plugin::Plugin {
 public:
  static constexpr float kDefaultWind = 40.0f;   // px / s
  static constexpr float kRollDistance = 480.0f;  // px before it starts over
  static constexpr float kBob = 3.0f;             // px

  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }

  void on_register(plugin::Registrar& reg) override;
  void on_play_begin(runtime::World& world) override;
  void on_play_end() override;

  // Live state (tests / Lua).
  float wind() const { return wind_; }
  void set_wind(float px_per_s);
  int count() const { return static_cast<int>(rolling_.size()); }
  std::uint64_t gusts() const { return gusts_; }  // ticks rolled this ride

  static bool is_tumbleweed(const std::string& name);

 private:
  struct Roll {
    float origin_x = 0.0f;
    float base_y = 0.0f;
    float phase = 0.0f;
    double t = 0.0;
  };
  void roll(runtime::World& world, double dt);

  float wind_ = kDefaultWind;
  std::map<std::uint64_t, Roll> rolling_;
  std::uint64_t gusts_ = 0;
};

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
