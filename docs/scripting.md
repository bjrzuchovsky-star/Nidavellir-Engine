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

The SDK is `sdk/include/tombstone/script_sdk.h`, a header-only wrapper over the C ABI in `script_abi.h` (ABI 4). Scripts derive from `ts::Script`. New scripts use the same names as C# (`OnStart`, `OnUpdate`, `Prop`, `Move`, `Input::GetAxis`). Older scripts that override `on_start` / `on_tick` still run.

```cpp
#include <tombstone/script_sdk.h>
struct Door : ts::Script {
  int opened = 0;
  void OnStart() override { Log("door ready, reach " + std::to_string((int)Prop("reach", 24.0))); }
  void on_interact(ts::Entity) override { toast("The door creaks"); set("open", ++opened); }
  std::string save() const override { return std::to_string(opened); }
  void load(const std::string& s) override { opened = std::atoi(s.c_str()); }
};
TS_SCRIPT(Door, "door")
```

How C++ scripts build and reload:

- **One module per project.** All `scripts/*.cpp` files build into a single module (`ts_scripts.dll` on Windows, `ts_scripts.so` on Linux). The engine generates a CMake project in `<project>/.tombstone/native` and builds it with CMake and the host compiler.
- **Windows toolchain.** The compiler is the user's MSVC toolchain (Visual Studio 2022, "Desktop development with C++"). If CMake or that toolchain is missing, the Telegraph says so and points at the Visual Studio download. Nothing else in the editor stops.
- **Linux.** GCC or Clang on `PATH`, plus CMake.
- **Background builds.** Builds run on a worker thread, and the new module is swapped in when it is ready.
- **Compile errors.** Errors go to the Telegraph with file:line, and the old module keeps running.
- **State.** Override `save()` / `load()` to keep fields across a hot reload. World state written with `set()` is kept anyway.
- **Exports** ship the compiled module (`native/ts_scripts.dll` or `.so`) and never the source files. The exported game does not need a compiler.

The first-person sample is `samples/scripting/fps-cpp/`. It does not use Rider 3D. The 2D twin of the coin is `samples/scripting/2d/scripts/coin.cpp`.

## C# scripts

The binding lives in `sdk/dotnet/TombStone.Scripting` (`Script.cs`, `Api.cs`, `Host.cs`). Namespace `TombStone`, base class `Script`, attribute `[Script("file_stem")]`.

```csharp
using TombStone;
[Script("teller")]
public class Teller : Script {
    int visits;
    public override void OnStart() => Log("reach " + Prop("reach", 24.0));
    public override void OnInteract(ulong rider) { visits++; Toast("Howdy", "visit " + visits); }
    public override string Save() => visits.ToString();
    public override void Load(string s) => int.TryParse(s, out visits);
}
```

How C# scripts build and run:

- **One assembly.** Every `scripts/*.cs` file builds into `ProjectScripts.dll`, targeting **.NET 8**. Save a file while the editor is running and the ride hot-reloads the assembly.
- **Build.** `dotnet build` needs the [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0).
- **Runtime.** .NET is loaded through `hostfxr` (the engine does not link nethost). Each build gets a collectible `AssemblyLoadContext`, so the previous assembly can unload.
- **No SDK.** C# is optional. Without the .NET 8 SDK or a .NET 8+ runtime, the editor says C# scripts are switched off, names the install link, and keeps going. Lua, graphs and C++ are not affected. New Script still writes the `.cs` file; it runs once the SDK is installed.
- **Compile errors.** `dotnet` diagnostics go to the Telegraph with file:line. A failed build keeps the last good assembly running.
- **State.** Fields (`float`, `int`, `bool`, `string`, `Vector2`, `Vector3`) are kept across a hot reload by the default `Save` / `Load`. Override those if you want a different layout. Scene props are applied again on the new instance.
- **Logs.** `Log`, `Warn` and errors record the script file and line, so a Telegraph line can be followed back to the source.

### Exported games and the .NET runtime

An export compiles the scripts and ships `dotnet/ProjectScripts.dll` plus the `TombStone.Scripting` binding and its `runtimeconfig`. It does **not** embed the .NET runtime.

- A game with **no** `.cs` scripts does not need .NET installed.
- A game **with** C# scripts needs the [.NET 8 runtime](https://dotnet.microsoft.com/download/dotnet/8.0) (the desktop runtime is enough; the SDK is only for building in the editor) on the machine that runs it. `hostfxr` finds that install the same way `dotnet` does. Set `DOTNET_ROOT` if the runtime lives somewhere unusual.
- The Windows build of the editor still needs the SDK on the author's PC. The Linux GCC build of the engine does not link .NET; C# is enabled only when the SDK is present.

The acceptance sample is `samples/scripting/fps-cs/scripts/character_movement.cs`. It compiles unchanged. Open `samples/scripting/fps-cs` and press Play: WASD, Space, mouse look. There is no Player component and Rider 3D is not on the claim. A small 2D example is `samples/scripting/2d/`.

If the same object has a Player component **and** Rider 3D is switched on, Play warns that both will move the body. Take the Player component off, or switch Rider 3D off, and let the script drive it. The samples do neither.

## API reference (C# and C++)

Both languages sit on one native API (`TsScriptApi`, ABI 4). Names match. C++ uses methods where C# uses properties (`Position()`, `Transform()`). Lua names are unchanged (`find`, `pos3`, `move3`, `on_tick`).

| C# | C++ | What it does |
|---|---|---|
| `Awake()` | `Awake()` | When the instance is created, before `OnEnable`. |
| `OnEnable()` / `OnDisable()` | `OnEnable()` / `OnDisable()` | Enabled at creation; disabled just before destroy. |
| `OnStart()` | `OnStart()` | First tick, before `OnFixedUpdate`. `on_start()` still works in C++. |
| `OnFixedUpdate(float dt)` | `OnFixedUpdate(float)` | Before the physics step. |
| `OnUpdate(float dt)` | `OnUpdate(float dt)` | After physics. `OnTick` / `on_tick` still run; the default calls `OnUpdate`. |
| `OnLateUpdate(float dt)` | `OnLateUpdate(float)` | After `OnUpdate`. |
| `OnDestroy()` | `on_destroy()` | The instance is going away. `OnDisable` runs first. |
| `Prop("n", 6.0f)` | `Prop("n", 6.0f)` | Typed default. Float, int, bool, string and `Vector3` (`"x,y,z"` in the scene). Shows in the Inspector. |
| `Position` / `Position.Y` | `Position()` | World position. Y is up in 3D. |
| `Transform.Right` / `Forward` / `Up` | `Transform().Right` / `Forward` / `Up` | World axes from the body's rotation. Forward is -Z. |
| `Move(Vector3)` | `Move(Vector3)` | Delta, in metres (3D) or pixels (2D). 3D movement separates from static colliders. |
| `Rotate(Vector3 eulerDegrees)` | `Rotate(Vector3)` | Adds euler degrees. |
| `Find("Name")` | `Find("Name")` | Null / empty when missing. `SetLocalRotation(x, y, z)` is local to the parent. |
| `Player` | `Player()` | Slot 1's body, or null. |
| `Input.GetAxis("Horizontal" \| "Vertical" \| "Mouse X" \| "Mouse Y")` | `Input::GetAxis(...)` | WASD / arrows, and mouse counts this tick. |
| `Input.GetButtonDown("Jump")` | `Input::GetButtonDown("Jump")` | Space. `"Action"` is the action key (E). |
| `Toast(title, message)` | `Toast(title, message)` | Both strings, shown as one line. |
| `Log` / `Warn` | `Log` | Telegraph, with file:line. |
| `Sound` / `Burst` | `sound` / `burst` | Clip and particle preset, same as Lua. |
| `SetVisible` / `SetCollider` / `Destroy` | `set_visible` / `set_collider` / `destroy` | Same as Lua `show` / `set_collider` / `destroy`. |
| `Get` / `Set` | `get` / `set` | Per-object values shared with Lua. |

`Vector3` has `.X` `.Y` `.Z` (settable), `Zero`, `Up`, `+` and `* float`, and a readable `ToString`.

Grounding in the sample is `Position.Y <= 0.01` plus `Move`, which will not push the capsule through a static floor. It does not call the Rider plugin and does not read the Rider's grounded flag.

### Not yet (parity with Unity / Unreal)

Designed on the ABI 4 `query` slot, not finished in this patch:

- `OnCollisionEnter` / `Stay` / `Exit` run in 2D and 3D when solid colliders overlap. Triggers still use `OnTriggerEnter` / `Exit`.
- `GetComponent` / `AddComponent` / `RemoveComponent` for collider, rigidbody, renderer, audio, camera and animator, and for other scripts in the same language. `Instantiate` clones a named entity or a `.prefab.json`. `Destroy(id, seconds)` waits, then removes it. `FindWithTag` / `SetTag` use the optional scene `tag` field. Layers and parenting from script are not yet.
- Public fields and `[SerializeField]` show in the Inspector, with `[Header]`, `[Range]` and `[Tooltip]` (C++ puts the same tags in comments above the field). Vectors, colours, enums and entity references are included. Custom property drawers are not.
- `Physics.Raycast` and `Overlap` (C#) and `Raycast` / `Overlap` (C++) hit solid colliders without a physics plugin. Sweep casts are not yet.
- Coroutines, events between scripts, scene loading, the full `anim.*` mirror, in-game UI, save helpers, networking hooks.
- Attaching the Visual Studio or VS Code debugger to a C# assembly or a C++ script DLL during Play.
- Editor scripting (custom Inspector / menu tools).

IntelliSense: the editor still writes `.tombstone/ide/<Claim>.Scripts.csproj` and the C++ `compile_commands.json` / SDK headers. Doc comments on the new API are the headers themselves.

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
