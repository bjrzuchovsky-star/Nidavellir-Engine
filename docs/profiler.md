# Profiler and debug tools: Assay Office (`ts.profiler`)

Assay Office is an optional built-in plugin (category Engine, off by default). It adds a frame profiler, in-game debug tools and three editor panels. Turn it on for a project in the Trading Post or the editor's Plugins panel (Outfitter's Wagon). That adds `"ts.profiler"` to the `plugins` list in `project.json`.

Sources:

- `src/profiler/`: the core (`ts_profiler`), console commands and the Lua tables (`ts_profiler_lua`).
- `src/debugtools/`: the in-game overlay and console (`ts_debugtools`), and the 3D world extension.
- `src/plugins/profiler/`: the plugin.
- `src/editor/plugins/ProfilerPanels.cpp`: the editor panels.

## What gets measured

The profiler is off until something switches it on. Play does that when the project enables `ts.profiler`, and so do the Record box, an open overlay or console, `prof on`, and `profiler.enable(true)`. While it is off, a scope costs one relaxed atomic load.

Each frame the profiler records three things:

- **CPU scopes** (`TS_PROFILE_SCOPE("name")`), nested, with a start time and duration relative to the frame. Scopes already in the engine:

  | Host | Scopes |
  |---|---|
  | 2D world | `world.step`, `world.physics`, `world.scripts`, `world.plugins`, one per plugin system, `world.animation`, `world.fx` |
  | 3D world | `world3d.step`, `world3d.physics`, `world3d.scripts`, one per extension, `world3d.animation`, `world3d.fx` |
  | Scripts | `scripts.lua`, `scripts.graph`, `scripts.cpp`, `scripts.cs` |
  | 3D renderer | `render3d.frame`, `render3d.submit` |
  | `ts_game` | `game.update`, `game.render` |
  | Editor | `editor.frame`, `editor.imgui_render` |

  Lua scopes show up as `lua.<name>`.
- **Counters**:

  | Group | Counters |
  |---|---|
  | Render | `render.draws`, `render.triangles`, `render.lights`, `render.shadow_casters`, `render.lines`, `render.skinned`, plus `render.quads` in 2D |
  | Assets and memory | `assets.meshes`, `assets.materials`, `assets.textures`, `mem.gpu_mb` (tracked buffer and texture bytes), `mem.process_mb` (resident set), `mem.lua_kb` |
  | World | `world.entities`, `world.ticks` |
  | Net, client | `net.rtt_ms`, `net.kbps_in`, `net.kbps_out`, `net.snapshot_hz`, `net.corrections`, `net.pending_inputs`, `net.interp_ms`, `net.stale_snapshots` |
  | Net, server | `net.server.riders`, `net.server.rtt_ms`, `net.server.kbps_in`, `net.server.kbps_out`, `net.server.snapshot_hz`, `net.server.snapshot_bytes` |
  | Scripts | `script.<name>`, from Lua `profiler.counter` |
- **GPU pass times** from OpenGL timer queries (`GL_TIME_ELAPSED`) in the 3D renderer: `shadow`, `opaque`, `transparent` and `overlay`. Results are read a few frames late so the CPU never waits on them. The 2D fixed-function path and drivers without timer queries report no GPU times. The panels say so.

Up to 600 frames are kept (10 s at 60 Hz).

## Editor panels (View menu, Dusk theme)

All three panels are listed in the 2D and 3D View menus, including during Play:

- **Assay Office** has these controls:
  - **Record**, **Freeze / Live** and **Clear**.
  - **Capture**, with an optional name.
  - **Load capture...**, which lists the project's `captures/` folder.
  - A frame-time bar chart with a 16.7 ms line. Bars turn amber, then red, as frames get slower. Click a bar to freeze and inspect that frame.
  - A **timeline (flame) view** of the selected frame, with one row per nesting depth and a zoom slider. Hover a zone for its exact times.
  - A **scope table** with calls, avg, self, max and total over the last N frames.
- **Render & Memory**: GPU passes as shares of the GPU total, and sparklines for every `render.*`, `assets.*`, `mem.*` and `world.*` counter.
- **Net**: sparklines for the client and server `net.*` counters while riding with a posse.

When `ts.profiler` is on, the Telegraph (the editor log panel) gets a **command line** that runs the console commands below. Up arrow recalls the last command.

## In-game debug tools

`ts_game` (2D and 3D), `ts_client` and editor Play (2D and 3D) add two built-in Dusk UI documents when the project enables `ts.profiler`:

- **F3** toggles the **overlay** (`debug_overlay`). It is non-modal and shows fps, frame and GPU times against the 16.7 ms budget, the slowest scopes, draws and triangles, memory and assets, and net stats.
- **Backtick (`)** toggles the **console** (`debug_console`). It shows the log and a command input, and swallows game input while it is open.

A project can ship its own `ui/debug_overlay.ui.json` or `ui/debug_console.ui.json`. Keep the widget ids `ov_fps`, `ov_budget`, `ov_scopes`, `ov_render`, `ov_net`, `dc_log` and `dc_input`, and the project's version replaces the built-in one.

## Console commands

| Command | Does |
|---|---|
| `help` | list commands |
| `echo <text>` | print text |
| `prof on\|off\|clear` | record / stop / drop history |
| `capture [name] [frames]` | save a capture to `<project>/captures/<name>.tsprof.json` |
| `fps` | fps, avg / max frame ms |
| `scopes [n]` | the n slowest scopes |
| `counters [prefix]` | latest counters, e.g. `counters net.` |
| `gpu` | GPU pass times |
| `mem` | memory and asset counts |
| `overlay [on\|off]` | show / hide the overlay (debug tools only) |
| `clear` | clear the console log (debug tools only) |

C++ hosts can add their own commands with `profiler::Console::shared().add(name, help, fn)`, where `args[0]` is the command name, and remove them with `remove(name)`.

## Lua

```lua
function on_tick(dt)
  profiler.begin("ai_think")       -- shows as lua.ai_think
  think()
  profiler.finish()
  profiler.counter("bandits", #bandits)   -- script.bandits
  if profiler.frame_ms() > 20 then console.run("capture spike 120") end
end
```

The `profiler` table has these functions:

- `begin`, `finish`, `counter`
- `capture([name])`, which returns the path, or nil and an error
- `fps`, `frame_ms`
- `enabled`, `enable`
- `scopes(n)`, which returns `{name, avg, max, calls}`

`console.run(line)` returns the command's output.

## Captures

A capture is [Chrome trace event](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU) JSON:

- Each frame and each zone is an `"X"` event, and each counter is a `"C"` event. Open the file in Perfetto or `chrome://tracing`.
- A `"tombstone"` block (format 1) stores each frame's index, CPU ms, GPU passes and counters. The Assay Office reads it back with **Load capture...** (`profiler::load_capture`).

## Smoke

`ts_admin --smoke-profiler` is also part of `--smoke`. It checks:

- scope nesting, the ring buffer, counters, GPU entries, stats and fps;
- the capture save and load round trip;
- console commands, including custom ones;
- the overlay and console on a `UiSystem`;
- 2D and 3D worlds feeding scopes, including the Lua `profiler` and `console` tables;
- the three panels drawing during editor Play.
