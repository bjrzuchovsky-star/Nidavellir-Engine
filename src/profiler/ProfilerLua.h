#pragma once

// Lua faces of the Assay Office (ts.profiler):
//   profiler.begin(name) / profiler.finish()   a named scope around script code
//   profiler.counter(name, value)              a per-frame counter ("enemies", ...)
//   profiler.capture([name]) -> path | nil, err
//   profiler.fps() / profiler.frame_ms()       last 60 frames
//   profiler.enabled() / profiler.enable(on)
//   profiler.scopes([n]) -> {{name=, avg=, max=, calls=}, ...}
//   console.run(line) -> output text           the shared debug console

struct lua_State;

namespace ts {
namespace tombstone {
namespace profiler {

void push_profiler_lua(lua_State* L);
void push_console_lua(lua_State* L);

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
