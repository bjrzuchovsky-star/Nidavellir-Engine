#include "debugtools/ProfilerWorld3D.h"

#include "profiler/Profiler.h"
#include "profiler/ProfilerLua.h"

#include "lua.h"

namespace ts {
namespace tombstone {
namespace profiler {

std::shared_ptr<ProfilerWorld3D> attach_profiler(world3d::World3D& w) {
  if (find_profiler(w)) return nullptr;
  auto x = std::make_shared<ProfilerWorld3D>();
  w.add_extension(x);
  return x;
}

ProfilerWorld3D* find_profiler(const world3d::World3D& w) {
  return dynamic_cast<ProfilerWorld3D*>(w.extension(ProfilerWorld3D::kName));
}

void ProfilerWorld3D::on_start(world3d::World3D& /*w*/) {
  Profiler& p = Profiler::get();
  if (!p.enabled()) {
    p.set_enabled(true);
    switched_on_ = true;
  }
}

void ProfilerWorld3D::on_stop(world3d::World3D& /*w*/) {
  if (switched_on_) Profiler::get().set_enabled(false);
  switched_on_ = false;
}

void ProfilerWorld3D::open_lua(world3d::World3D& /*w*/, lua_State* L, int sandbox) {
  push_profiler_lua(L);
  lua_setfield(L, sandbox, "profiler");
  push_console_lua(L);
  lua_setfield(L, sandbox, "console");
}

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
