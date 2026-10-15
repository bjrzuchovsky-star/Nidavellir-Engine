#pragma once

// The story plugins in 3D: a World3D extension carrying a StoryRuntime
// (quests / dialogue / saves as the project enables them), its Lua tables,
// the UI tap and the rider's position for saves. Hosts attach it when any
// of ts.quests / ts.dialogue / ts.saves is on.

#include "story/StoryRuntime.h"
#include "world3d/World3D.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace story3d {

class StoryWorld3D final : public world3d::World3DExtension {
 public:
  static constexpr const char* kName = "ts.story";
  explicit StoryWorld3D(story::StoryFeatures f) : features_(f) {}
  const char* name() const override { return kName; }
  void on_start(world3d::World3D& w) override;
  void on_tick(world3d::World3D& w, double dt) override;
  void on_stop(world3d::World3D& w) override;
  void open_lua(world3d::World3D& w, lua_State* L, int sandbox) override;

  story::StoryRuntime& runtime() { return rt_; }
  const story::StoryFeatures& features() const { return features_; }

 private:
  story::StoryFeatures features_;
  story::StoryRuntime rt_;
  story::StoryRuntime* slot_ = nullptr;  // Lua upvalue target
};

// Adds the extension when any feature is on; returns it (null otherwise).
std::shared_ptr<StoryWorld3D> attach(world3d::World3D& w, const story::StoryFeatures& f);
StoryWorld3D* find(const world3d::World3D& w);

}  // namespace story3d
}  // namespace tombstone
}  // namespace ts
