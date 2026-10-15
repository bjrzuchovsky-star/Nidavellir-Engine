#pragma once

// ts.profiler in 3D: a World3D extension that records while the world
// rides (when the project enables the plugin) and opens the `profiler` /
// `console` Lua tables for 3D scripts. Hosts attach it next to story3d.

#include "world3d/World3D.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace profiler {

class ProfilerWorld3D final : public world3d::World3DExtension {
 public:
  static constexpr const char* kName = "ts.profiler";
  const char* name() const override { return kName; }
  void on_start(world3d::World3D& w) override;
  void on_stop(world3d::World3D& w) override;
  void open_lua(world3d::World3D& w, lua_State* L, int sandbox) override;

 private:
  bool switched_on_ = false;
};

std::shared_ptr<ProfilerWorld3D> attach_profiler(world3d::World3D& w);
ProfilerWorld3D* find_profiler(const world3d::World3D& w);

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
