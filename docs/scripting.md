# Scripting: Lua, visual graphs, C++ (hot reload) and C#

Source: `src/runtime/` (`Script*.cpp`, `World_Script.cpp`, `GraphScript.*`, `NativeScript.*`, `CompiledScript.*`, `CSharpScript.*`, `ScriptLanguages.cpp`), `sdk/`.

A script component names a file under `scripts/`, and the file extension picks the language:

| Extension | Language | Host | Hot reload |
|---|---|---|---|
| `.lua` | Lua 5.4.8 (sandboxed) | `LuaScriptHost` | Per file, state kept |
| `.graph.json` | Visual graph | `GraphScriptHost` | Variables kept |
| `.cpp` | C++20 | `NativeScriptHost` (one module per project) | Through `save()` / `load()` |
| `.cs` | C# (.NET 8) | `CSharpScriptHost` (hostfxr + AssemblyLoadContext) | Through `Save()` / `Load()` |

All four languages share the same hooks and world API.

## Script components (several per object)

An object can carry any number of scripts (up to 16), in any mix of languages, much like Unity components. Each script is its own section in the Inspector of the 2D and 3D editors:

- **Checkbox:** turns the script on or off. A switched-off script stays on the object with its settings but does not run at all (no `on_start`, no events).
- **Arrow buttons / `...` menu:** move the script up or down, open it, or remove it. Every change is one undo step (**Attach Script**, **Move Script**, **Remove Script**, ...).
- **Properties:** each script shows its own `props` (Lua `props`, graph variables, C++ / C# fields the SDK reports). They are saved per object, per script, so two objects can run the same file with different settings.
- **Open / VS Code / Visual Studio:** opens the file (see [Editing in VS Code or Visual Studio](#editing-in-vs-code-or-visual-studio)).

### Order

Scripts run in a fixed, predictable order:

1. **On one object,** events (`on_start`, `on_interact`, triggers, collisions, VR hooks such as `on_grab` / `on_release`, ...) reach its scripts top to bottom, in Inspector order, whatever their languages.
2. **Across the world,** a phase (`on_start`, `on_update`, `on_fixed_update`, ...) runs every object's first script, then every object's second script, and so on. Within one stack position the languages keep their old order: lua, graph, cpp, cs. So a project with one script per object behaves exactly as before.

Put the script that sets things up at the top. A script that fails (a Lua error, a C# exception) is logged and switched off for the ride; the rest of the object's scripts keep going. Errors name the object and the script, for example `"Wagon" (scripts/boom.lua): attempt to call a nil value`.

### Adding scripts

- **Add Component** (bottom of the Inspector) > **New Script...** asks for a name, a language (Lua, Visual graph, C++ or C#) and a template, writes `scripts/<name>.<ext>` and attaches it. Names are turned into safe file names (`Coach Gate` becomes `coach_gate.lua`); an existing file is never overwritten.
- **Add Component** > **Script** lists the claim's existing scripts with a search box. The 2D menu also adds Collider, Player, Camera and Spawn Point; in 3D the other components stay on the **+** buttons above.
- **Drag a script** from the Asset Browser or the Scripts panel onto an object in the viewport (it lights up and says *Attach to ...*), onto a Hierarchy row, or onto the Inspector. Dropping a script the object already runs does nothing and says so.

### Templates

Every language has the same five templates, so you can start in one and port to another:

| Template | What it does |
|---|---|
| Blank | Every hook stubbed out, ready to fill in |
| Gate | Opens on the action button; can shut itself |
| Pickup | Pocketed by the first rider through its trigger |
| Sign | Shows a HUD line on the action button |
| Ticker | Repeats on a timer (a tumbleweed hop) |

C++ templates compile against `tombstone/script_sdk.h` and C# templates against the TombStone.Scripting SDK; graph templates are small graphs that open in the Brand Book. Without the .NET 8 SDK, New Script still writes C# files; they run once the SDK is installed.

### Scene format

2D scenes (version 12) and 3D scenes (version 17) store the stack as `"scripts": [ { "path": ..., "enabled": true, "props": { ... } }, ... ]`. An object with just one running script is still written with the old `"script"` key, and older scenes with a single `"script"` load as a one-script stack.

### Editing in VS Code or Visual Studio

**Open** uses **Settings > Tools > Open scripts in**: *Automatic* (VS Code if found, then Visual Studio for C++ / C#, otherwise the system's default app), *VS Code*, *Visual Studio* or *The OS default app*. Visual graphs always open in the Brand Book under *Automatic*. The VS Code and Visual Studio buttons on each script force one or the other.

- **VS Code** is found on `PATH` (`code`), in the registry, or under `%LOCALAPPDATA%\Programs\Microsoft VS Code`; set `TS_VSCODE` to override. It opens the claim's `<claim>.code-workspace` and jumps to the file (`-g`).
- **Visual Studio** is found with `vswhere` (or `TS_DEVENV`). C# scripts open the generated project; others open the claim folder with the file (`/Edit`).
- Before launching, TombStone writes what the IDE needs: `.vscode/settings.json`, `extensions.json` and `c_cpp_properties.json` (written once, yours to tune), the `.code-workspace`, a `compile_commands.json` and `CppProperties.json` for the C++ scripts (pointing at the script SDK headers), and for C# `.tombstone/ide/<Claim>.Scripts.csproj` referencing a copy of the SDK binding. The engine never builds from these files.
- If the editor is not installed, the status bar says so and points at Settings; nothing else changes.

`ts_admin --smoke-scripts-multi` (also in `--smoke`) checks all of this. `ts_admin --scripts-shot <out.png>` renders the Inspector with several scripts and the Add Component menu.

## Lua

**Sandbox.** Only the base, table, string, math, utf8 and coroutine libraries exist. `load`, `dofile`, `require`, `collectgarbage` and `string.dump` are removed.

**Limits:**

- 2,000,000 instructions per hook call
- 64 MB memory shared by all scripts
- 4096 timers per world
- 256 state keys per entity

**Determinism.** `dt` is always 1/60, timers count ticks, and `math.random` is seeded and replayable.

**Hooks:**

- `on_start()`, `on_tick(dt)`
- `on_trigger_enter(other)`, `on_trigger_exit(other)`
- `on_interact(rider)`, `on_reload()`
- 3D and VR add `on_anim_event`, `on_grab`, `on_release`, `on_point`, `on_unpoint` and `on_vr_trigger`.

**Globals** (registered in `ScriptApi.cpp`):

- `log`, `print`, `warn`, `toast`
- `after`, `every`, `cancel`
- `find`, `player`, `spawn`, `duplicate`
- `tick`, `time`
- the `self` and `props` values

**Entity methods:**

- `id`, `name`, `alive`
- `pos`, `center`, `size`, `set_pos`, `move_by`
- `show`, `hide`, `visible`
- `set_collider`, `collider_on`
- `play`, `release`
- `get`, `set`, `destroy`
- `is_player`, `slot`, `toast`, `spawned`

**Engine tables:**

- `audio`: see [audio](audio.md)
- `ui`: see [UI](ui-and-fonts.md)
- `particles`: see [particles](particles-and-lighting.md)
- 3D adds `anim` and `vr`, and 3D entity methods `pos3`, `set_pos3`, `move3`, `yaw`, `set_yaw`, `spin`, `scale3` and `set_scale3`.
- Plugins add their own tables: `frontier`, `ships`, `tumbleweed`.

```lua
props = { open_line = "The gate creaks open.", locked = false }
function on_interact(rider)
  if props.locked then rider:toast("Locked tight.", 2) return end
  self:set_collider(false); self:hide()
  rider:toast(props.open_line, 2)
end
```

**Hot reload.** The editor and `ts_game` check `scripts/` twice a second. A broken save keeps the old version running and logs the error with file:line. A runtime error stops only the instance that raised it.

The Scripts panel writes new files from these templates: Blank, Gate, Pickup, Sign, Ticker.

## Visual graphs (Brand Book)

`*.graph.json` files are run by an interpreter on the fixed tick (`GraphScript.cpp`, `graph_node_catalog()`). The catalog registers 67 node types in these families:

- events
- flow
- math, compare/logic, values and variables
- entities and state
- audio, particles, UI, toast and log
- animation
- Plugin Call

Each hook call has a budget of 20000 steps and a nesting depth limit of 64 (`kMaxDepth`).

Open the editor (`src/editor/scripting/BrandBook.*`) in any of these ways:

- **File > Brand Book... (visual scripts)** in 2D, or **Claim > Brand Book... (visual scripts)** in 3D
- by double-clicking a `.graph.json`
- standalone: `ts_admin --brand-book <project> [graph]`

Ctrl+S saves the graph, and a running ride hot-reloads it.

## C++ scripts (hot reload)

The SDK is `sdk/include/tombstone/script_sdk.h`, a header-only wrapper over the C ABI in `script_abi.h`:

```cpp
#include <tombstone/script_sdk.h>
struct Door : ts::Script {
  int opened = 0;
  void on_interact(ts::Entity) override { toast("The door creaks"); set("open", ++opened); }
  std::string save() const override { return std::to_string(opened); }
  void load(const std::string& s) override { opened = std::atoi(s.c_str()); }
};
TS_SCRIPT(Door, "door")
```

How C++ scripts build and reload:

- **One module per project.** All `scripts/*.cpp` files build into a single module. The engine generates a CMake project in `<project>/.tombstone/native` and builds it with CMake and the host compiler (on Windows, the VS2022 toolchain).
- **Background builds.** Builds run on a worker thread, and the new module is swapped in when it is ready.
- **Compile errors.** Errors go to the Telegraph with file:line, and the old module keeps running.
- **Building from the editor.** The Brand Book has a **Build C++ scripts** button.
- **Exports** ship `native/ts_scripts.dll` and never the source files.

## C# scripts

The binding lives in `sdk/dotnet/TombStone.Scripting` (`Script.cs`, `Api.cs`, `Host.cs`):

```csharp
using TombStone;
[Script("teller")]
public class Teller : Script {
    int visits;
    public override void OnInteract(ulong rider) { visits++; Toast("Howdy, visit " + visits); }
    public override string Save() => visits.ToString();
    public override void Load(string s) => int.TryParse(s, out visits);
}
```

How C# scripts build and run:

- **Build.** `dotnet build` turns the scripts into `ProjectScripts.dll`. This needs the .NET 8 SDK.
- **Runtime.** .NET is loaded at runtime through `hostfxr` (the engine does not link nethost). Each build gets a collectible AssemblyLoadContext.
- **No SDK.** Without the SDK, C# is disabled and the editor shows a message to install it. Other languages keep running.

## Calling plugins from scripts

| Language | Call |
|---|---|
| Graphs | Plugin Call node |
| C++ | `plugin("table", "fn", {ts::arg(3.0)})` |
| C# | `Plugin("table", "fn", 3.0)` |

All three go through `ScriptWorld::script_plugin_call`.

## Plugin events (story hooks)

Plugins broadcast events to every running script, in entity id order. The story plugins send `on_quest_started`, `on_quest_completed`, `on_quest_failed`, `on_objective`, `on_dialogue_event`, `on_dialogue_end`, `on_game_saved` and `on_game_loaded`. Each event carries a text value and a number.

| Language | Receive |
|---|---|
| Lua | `function on_quest_completed(id) ... end` (any hook name) |
| Graphs | **On Plugin Event** node: set its `hook` input; it outputs `text` and `number` |
| C++ | `void on_quest_completed(const std::string& id) override` and the other story virtuals, or override `on_event(hook, text, n)` to see every event |
| C# | `public override void OnQuestCompleted(string id)` and the others, or override `OnEvent(hook, text, n)` |

C++ and C# receive these through the ABI hook `TS_HOOK_EVENT` (9), with text in the form `"hook\ttext"`. The ABI version stays at 2: modules built before this change ignore the new hook.

Smoke tests: `--smoke-graph`, `--smoke-cpp`, `--smoke-cs`.
