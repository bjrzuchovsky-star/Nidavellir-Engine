#pragma once

// ts.weather in 3D worlds: a World3D extension. Each tick it steps the
// weather Sim, rewrites the world's runtime Environment (fog, overcast sky,
// dimmer sun, lightning flashes, wind for foliage and particles, the sun
// following the hour when time_link is on) and adds rain / snow / dust
// billboards in a box around the camera. Lua gets the `weather` table and
// scripts hear on_weather(kind) / on_lightning(count). Hosts attach it when
// the project enables ts.weather (like ai3d / story3d).

#include "render3d/Renderer3D.h"
#include "weather/Weather.h"
#include "world3d/World3D.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace weather {

// The environment a weather state gives a scene whose own is `base`.
void apply_environment(const State& s, const Config& c, const render3d::Environment& base,
                       render3d::Environment* out);

class Weather3D final : public world3d::World3DExtension {
 public:
  static constexpr const char* kName = "ts.weather";
  ~Weather3D() override;
  const char* name() const override { return kName; }
  void on_start(world3d::World3D& w) override;
  void on_tick(world3d::World3D& w, double dt) override;
  void on_stop(world3d::World3D& w) override;
  void open_lua(world3d::World3D& w, lua_State* L, int sandbox) override;
  void billboards(const world3d::World3D& w,
                  std::vector<render3d::Renderer3D::Billboard>* out) const override;

  Sim& sim() { return sim_; }
  const Sim& sim() const { return sim_; }
  // Use this config instead of the project's weather.json (editor preview).
  void set_config(const Config& c) {
    cfg_ = c;
    have_cfg_ = true;
  }
  static Weather3D* of(const world3d::World3D& w);
  // The most recently started one (editor preview), or null.
  static Weather3D* latest();

 private:
  Sim sim_;
  Config cfg_;
  bool have_cfg_ = false;
  render3d::Environment base_;
  Kind announced_ = Kind::Clear;
  std::uint64_t strikes_seen_ = 0;
  float thunder_in_ = -1.0f;
  float thunder_volume_ = 1.0f;
};

std::shared_ptr<Weather3D> attach(world3d::World3D& w, bool enabled);

}  // namespace weather
}  // namespace tombstone
}  // namespace ts
