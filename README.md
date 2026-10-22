# Tombstone Engine

Tombstone Engine is a fully custom C++ game engine built for high performance, low-level control, and a streamlined development workflow.  
It is designed around a modular architecture, a custom build system, and a custom rendering backend to ensure predictable behavior and maximum efficiency across all engine operations.

This repository contains the core engine source code, internal tools, and supporting systems used to build and run Tombstone Engine.

---

## Features

### Custom Build System
A dedicated build pipeline designed specifically for the engine's architecture.  
Provides fast compilation, predictable output, and full control over how engine modules are built and linked.

### Custom Rendering Backend
A low-level rendering system focused on speed, stability, and minimal overhead.  
Built to support modern graphics APIs and optimized for high-framerate gameplay and editor performance.

### Modular Engine Architecture
Core systems are separated into clean, independent modules, allowing for:
- Efficient runtime behavior  
- Clear engine structure  
- Easy internal maintenance  
- High scalability for future features  

### Admin / Client / Game Builds
Tombstone Engine is structured into three distinct build types:

- **Admin Build**  
  Full engine + editor tools. Used internally for development, debugging, and engine management.

- **Client Build**  
  Lightweight engine runtime for developers building games on top of Tombstone Engine. `ts_client` is the multiplayer client (connects to a `ts_server`).

- **Server Build**  
  `ts_server`: the headless, authoritative multiplayer server (no window, no GL). See [Multiplayer](#multiplayer-the-posse).

- **Game Build**  
  Stripped-down runtime containing only what shipped games need for execution and performance.

### Performance-Focused Design
Every system is built with performance as the primary goal:
- Minimal abstraction overhead  
- Tight memory usage  
- Predictable update loops  
- High-efficiency rendering path  
- Optimized runtime modules  

---

## Plugins and project types

**Project type.** Every new claim is 2D, 3D or VR, picked on the Project Manager cards and stored as `"dimension"` in `project.json`. 2D is the full editor you know. 3D opens the 3D editor (below). VR opens the same 3D editor with a VR player rig and headset preview (see [VR projects](#vr-projects)). A project without `"dimension"` loads as 2D.

**Plugins (`src/plugin`, `ts_plugin`).** A plugin is a `ts::plugin::Plugin` with an id, a semver version and dependencies (required or optional, with a minimum version). `on_register` gets a `Registrar` that can add World systems/components, editor panels and menu items, Lua API tables, asset types and tick hooks. The `PluginManager` orders plugins by dependency (Kahn's algorithm, catalog order breaks ties) and checks versions (same major, >= minimum). Missing, disabled or wrong-version dependencies and loops are reported to the Telegraph, and the skip cascades to dependents. Plugins unload in reverse order.

- **Static:** `TS_PLUGIN_STATIC(sym, Class)` in the plugin, plus `TS_PLUGIN_DECLARE(sym)` / `TS_PLUGIN_LINK(sym, catalog, err)` in the host. The explicit link call works on MSVC because there are no global constructors for the linker to strip.
- **Dynamic:** build a shared module and put `TS_DYNAMIC_PLUGIN(Class)` in it. It exports the C entry `ts_plugin_entry`, which is loaded with `LoadLibrary` on Windows and `dlopen` elsewhere. ABI, API and C++ runtime tags are checked, so a module must be built with the same compiler and runtime as the host. Search path: `$TS_PLUGIN_PATH`, `<project>/plugins`, then the build's `plugins/` directory. `sample.lantern` (`src/plugins/lantern`) is the sample.
- **Built-ins:** `ts.posse` (multiplayer: Network panel, Play-menu entries, toolbar button; on by default), `ts.tumbleweed` (sample gameplay plugin: system, `tumbleweed` Lua table, `.tumble.json` asset type; off by default) and `ts.frontier` (Frontier Survival, below; off by default).

**Per-project enablement.** The Outfitter's Wagon panel (Plugins tab) hitches and unhitches plugins. Hitching a plugin brings its dependencies along, and unhitching one also unhitches its dependents. The list is saved right away to `project.json` as `"plugins"`. Changes take effect when the editor is idle, or after Play or the posse stops (restart-of-play). Old projects without the key get the default built-ins. `ts_game`, `ts_server` and `ts_client` load the project's plugins too.

**Tests.** `ts_admin --smoke` covers load order, dependencies, the missing-dependency error, versions and loops, enable/disable persistence, the 2D/3D/VR round trip, an old `project.json`, and the dynamic sample.

## 3D projects

**Renderer (`src/render3d`, `ts_render3d`).** OpenGL 3.3 core forward renderer on the existing GLFW window / loader (functions fetched with `glfwGetProcAddress`; no extra loader library).
- Passes: sun shadow map (2048², 3×3 PCF, a box that follows the camera target), gradient sky with a sun disc, then meshes, then unlit lines (grid, colliders, gizmos).
- PBR-lite material: albedo / normal / metal-rough textures (stb_image), metallic, roughness, emissive. GGX specular, hemisphere ambient, Reinhard tone map, gamma 2.2.
- Lights: one directional sun plus up to 8 point lights per frame.
- Meshes: built-in primitives `cube`, `plane`, `sphere`, `capsule`, and glTF 2.0 (`.gltf` / `.glb`) through cgltf v1.14, fetched by URL and SHA-256 like the other dependencies. Source engine `.mdl` (GMod; clean-room reader with VMT / VTF), OBJ, FBX, DAE, 3DS, PLY, STL, X and more through Assimp 5.4.3 (importers only), and `.blend` through a local Blender also import; see [docs/model-import.md](docs/model-import.md). You are responsible for having the rights to any model you import.
- Draws into its own offscreen target (editor viewport, tests, `read_pixels`) or straight into the window (`ts_game`). `make_null_renderer3d()` is the headless backend.

**Scene (`scene.json` v7).** A 3D scene carries `"dimension": "3d"` and entities with a transform (position, rotation quaternion, scale) and optional mesh, material, light, collider (box / capsule, trigger), player and script components, plus environment, editor camera and snap settings. 2D scenes are unchanged. The 2D reader refuses a 3D file (and the 3D reader a 2D one) with a clear reason.

**Editor.** 3D claims open the 3D editor:
- Viewport: orbit (RMB or Alt+LMB), pan (MMB), zoom (wheel), fly (hold RMB + WASD / Q E), F frames the selection.
- Selection: click to pick, Ctrl+click to add or remove.
- Gizmos: W / E / R switch between translate, rotate and scale. Snap is on the toolbar (Shift+G); steps are 0.5 m, 15° and 0.25 by default.
- Panels: Hierarchy, Inspector for every component, Assets (primitives, `.gltf` / `.glb`, images), Environment (sky, sun, shadows, ambient), Telegraph and Plugins.
- Assets: drag a model into the viewport to place it on the ground with a collider fitted to it. Drop an image on an entity to make it the albedo texture.
- Every edit is one undo step (Ctrl+Z / Ctrl+Y) and autosaves `scene.json`.

**Play (F5).**
- The scene rides in a `World3D` copy, and Stop puts everything back.
- The rider (Player component) is a capsule. WASD moves relative to the follow camera, Space jumps and E interacts (within 2.5 m). Q / Z or RMB turn the camera.
- Collision is the capsule against static box / capsule colliders, with gravity. Trigger colliders fire enter / exit.
- Lua scripts run as in 2D (`self:x()` / `self:y()` map to x / z). 3D adds `pos3`, `set_pos3`, `move3`, `yaw`, `set_yaw`, `spin(deg, axis)`, `scale3` and `set_scale3`.

**ts_game.** `ts_game --project <dir>` rides 3D projects in a GL 3.3 core window. `--smoke` loads the scene, rides 120 ticks, checks the rider moved and landed, and renders one offscreen frame with a pixel check.

**Sample.** `ghost-town-3d` is seeded next to the 2D sample when the projects folder is empty. It has a main street of primitive buildings, a textured boardwalk, glTF lanterns with point lights, a Lua windmill and a bell trigger that rings.

**Tests.** `ts_admin --smoke` runs the 3D checks: math and primitives, the scene v7 round trip (byte-stable), glTF loading and the sample, the play runtime, a GL render with pixel checks, and the editor (gizmos with snap, undo / redo, picking, glTF drop, save / reload, play / stop). For the 3D checks alone, run `ts_admin --smoke-3d`. The GL render check is skipped when there is no display, unless `TS_SMOKE_REQUIRE_GL=1` is set; on Linux CI, run it under `xvfb-run`.

## 3D skeletal animation

Skinned glTF characters animate in the 3D editor, in Play, and in exported games.

- **Import (cgltf).** The loader reads the first skin of a `.gltf` or `.glb`: joints, inverse bind matrices, and `JOINTS_0`/`WEIGHTS_0` (up to 4 weights per vertex, normalised on load). Every animation becomes a clip. Clips carry translation, rotation and scale channels with `LINEAR`, `STEP` and `CUBICSPLINE` interpolation; rotations use shortest-path slerp. Skinned meshes stay in bind space, and their bounds come from the rest pose.
- **GPU skinning.** `GlRenderer3D` skins in the vertex shader. The bone palette (joint × inverse bind) is a std140 uniform block `Bones` with at most **128 bones**. A skin with more joints draws unskinned and logs a warning. Shadows are skinned too. `FrameStats::skinned` counts skinned draws.
- **Animator component** (`scene.json` v12; v11 and older scenes load unchanged):
  - **Parameters:** float, bool and trigger.
  - **Layers:** stacked bottom to top. Each layer has a weight and an optional bone mask. A mask bone takes its whole subtree, so `chest` gives an upper-body layer.
  - **States:** each state plays either one clip or a **1D blend space**. In a blend space the clips sit at parameter values (for example idle 0 / walk 1.8 / run 4.5 on `speed`) and stay phase-synced.
  - **Transitions:** go from a state or from `*` (any state) when a condition holds: `> value`, `< value`, true, false, trigger, or always. A transition can optionally wait for the end of a non-looping state, and it crossfades over its duration.
  - **Events:** each event has a clip, a time and a name. Scripts receive `on_anim_event(name, time)`.
  - **Root motion** (optional): moves the entity by the root bone's ground travel, while the bone itself stays in place.
  - **Driven by rider:** the Play-mode character controller feeds `speed`, `grounded`, `vertical_speed`, and the `jump` trigger on takeoff.
- **VR:** set `VrRig3D.hand_model` (inspector: *Hand model*) to a skinned hand glTF. The right-hand model is mirrored for the left hand. The first clip is posed by grip/squeeze, and the hand closes while holding something.
- **Editor:**
  - The **Animator** inspector component edits parameters, layers and masks, states (with clip pickers and blend points), transitions, events, root motion and driving. *Reset to default locomotion* fills in the standard setup.
  - **View → Animation Preview** lists the selection's clips and provides play/pause, speed (negative plays backwards) and a scrub timeline with event ticks. The preview only poses the edit view; it never changes the scene.
- **Lua** (World3D): `e` is an entity handle, id or name.

  ```lua
  anim.play(e, "wave", 0.2)         -- a state, or any clip (goes to the top layer whose mask covers it; one-shot)
  anim.play(e, "idle", 0.2, true)   -- hold the clip instead of returning to the layer's entry state
  anim.crossfade(e, "run", 0.4)
  anim.set_param(e, "speed", 3.0)   -- numbers / booleans; anim.trigger(e, "jump")
  anim.param(e, "speed"); anim.state(e, 1)  -- layer 1 = base; ad-hoc clips read "clip:<name>"
  anim.stop(e, 2, 0.3)              -- fade a layer out
  ```

  `self:play("jump")` on a 3D entity also starts an animator state or clip.
- **Graph nodes** (category *Animation*): `anim.play`, `anim.crossfade` and `anim.set_param`. An id of 0 means *self*. They go through the same `anim` table via the plugin bridge.
- **Sample assets (CC0).** The Ghost Town 3D template ships *Drifter* (`assets/drifter.gltf`, ~77 KB): a 15-bone low-poly figure with the clips `idle`, `walk`, `run` (cubic-spline hips), `jump` (stepped arms) and `wave` (right arm only). The rider uses it, and *Old Hank* by the saloon waves at riders who come near. The Haunted Saloon VR template ships `assets/hand.gltf`, an 11-bone hand with a `grip` clip. Both assets are generated by the engine (`write_sample_character_gltf` / `write_sample_hand_gltf`) and are dedicated to the public domain (CC0). The Khronos CesiumMan and Fox samples are CC-BY 4.0, so they are not bundled; they import fine if you add them yourself.
- **Limits:** only the first skin per file is used. Nodes between joints that are not joints themselves are ignored. Root motion is ground-plane (XZ) only.
- **Smoke:** `ts_admin --smoke-anim3d` covers:
  - skin math against a hand-computed reference;
  - linear, step and cubic sampling plus slerp;
  - 1D blend weights;
  - Drifter and hand glTF round trips;
  - the animator: crossfade, jump and landing, the upper-body mask, events and root motion;
  - the scene v12 round trip;
  - World3D, Lua and the graph bridge;
  - an offscreen GPU skinned-render pixel check against the CPU reference;
  - the editor preview.

## VR projects

VR projects (`"dimension": "vr"`) are 3D scenes with a **VR Rig** entity (the player's tracking-space floor origin) and props marked **Grabbable**. They open in the 3D editor; the Haunted Saloon VR sample (`haunted-saloon-vr`) is seeded with the other samples.

**Runtime.** `ts_xr` talks to OpenXR through the Khronos loader (OpenXR-SDK 1.1.63, fetched by hash; `-DTS_WITH_OPENXR=OFF` drops it). On Windows it renders with OpenGL (`XR_KHR_opengl_enable`) into the runtime's swapchains, one view and projection per eye. On Linux, and wherever no runtime or headset answers, it falls back to the **desktop simulator**: the same stereo rendering, with mouse and keys driving the head and hands. Tested target: SteamVR.

**In the headset.**
- Head and both controllers are tracked through one action set, with bindings for Valve Index, HTC Vive, Oculus/Meta Touch and the Khronos simple controller.
- **Trigger** fires the laser. It calls `on_vr_trigger(hand)` on what it points at, or `on_interact` if the script has no `on_vr_trigger`.
- **Grip** grabs a Grabbable within reach. If nothing is in reach, it pulls the Grabbable under the laser into your hand. Let go to drop it, or swing and let go to throw it.
- **Left stick** walks (smooth locomotion; walls block you and low steps are climbed).
- **Right stick:** push forward to aim the teleport arc and release to jump; push left or right to snap turn (or smooth turn).
- **Menu** (left controller) opens the comfort menu: vignette, snap angle, locomotion mode, and seated mode. On Vive wands the trackpad acts as the stick.
- **Comfort defaults** live on the VR Rig component in the inspector: locomotion (teleport / smooth / both), move speed, snap angle (0 = smooth turn), teleport range, vignette while moving, and seated mode with its height offset.

**Editor.** The toolbar has two VR buttons:
- **Preview in headset** plays the scene in the HMD. A **Headset Mirror** window shows both eyes. If no headset is available, the preview runs in the simulator.
- **Play (simulator)** plays the scene with the simulator.

Stop ends the session; the authored scene is never changed.

**Simulator keys.**
- Hold the right mouse button to look; WASD walk.
- Q / E snap turn; hold T to aim the teleport arc and release to jump.
- Left mouse is the trigger; G or middle mouse is the grip. Hold Shift to use the left hand instead.
- M / Tab opens the menu; Ctrl crouches; X / Z move the hands farther or closer.

**ts_game.**
- `ts_game --project <dir>` runs a VR project in the headset with a mirror window. If no headset is available, it uses the simulator in a window.
- `--sim` forces the simulator.
- `--xr-probe` creates and destroys an OpenXR instance and session, prints the runtime, system and view sizes, and exits.
- `--smoke` runs the simulator headless. It checks stereo pixels, walking, snap turn, grab and throw, and teleport.

**Lua.** Every script in a VR world gets a `vr` table:
- `vr.active()`
- `vr.head()` returns x, y, z, yaw.
- `vr.hand("left"|"right")` returns x, y, z, trigger, grip.
- `vr.held(hand)` and `vr.pointed(hand)` return an entity or nil.
- `vr.rig()`
- `vr.teleport(x, y, z)`
- `vr.haptic(hand, amplitude, seconds)`

New hooks: `on_grab(hand)`, `on_release(hand, speed)`, `on_point(hand)`, `on_unpoint(hand)` and `on_vr_trigger(hand)`. Triggers fire `on_trigger_enter` for thrown props and for the player's head.

**SteamVR setup (Windows).**
1. Install Steam and SteamVR, connect the headset, and start SteamVR. The headset and controllers should show green.
2. In SteamVR, open **Settings → OpenXR** (in older versions, **Settings → Developer**) and click **Set SteamVR as OpenXR runtime**. This sets `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` to SteamVR's `steamxr_win64.json`.
3. Run `ts_game --xr-probe`. It should print `SteamVR/OpenXR` and the headset. Then open a VR project and click **Preview in headset**, or run `ts_game --project TombStoneProjects/haunted-saloon-vr`.

**scene.json v8** adds two optional entity keys: `"vr_rig"` and `"grabbable"`. **v9** adds `"parent"` (entity id; children follow their parent in the editor and at runtime) and the editor-only `"locked"` / `"editor_hidden"`. v1–v8 files still load, and newer versions are refused.

## Frontier Survival (2D gameplay plugin)

`ts.frontier` is a Necesse-style survival layer for 2D projects. It lives in `src/plugins/survival` as a built-in of `ts_plugins_builtin`, is **off by default**, and you hitch it in the Outfitter's Wagon (Gameplay). The plugin adds no new scene format: the frontier is generated around the riders, and only the edits are saved.

**World.**
- An endless top-down tile world in 32×32-tile chunks (32 px tiles), streamed around every rider: chunks within 2 load and chunks beyond 3 are written back and dropped.
- Seeded and fully deterministic: integer-only noise, so every machine builds the same tiles.
- Biomes: prairie (trees, bushes, boulders), desert (cacti, copper), badlands (mesa rock, iron, gold) and caves (cave rock walls, rich ore). Spawn sits in a clearing on the prairie.
- Edited chunks are saved to `<project>/saves/<world>/c_<cx>_<cy>.chunk` (binary RLE, tagged with the seed and generator version). `world.json` beside them holds the seed, the clock, every rider's saddlebags and grit, and the settled beds.
- It autosaves every 60 s and when the ride stops. A save made with another seed keeps its own seed, with a warning.

**Riding.**

| Key | Does |
|---|---|
| F / left mouse | Mine or chop the tile in front, swing the held weapon, eat food |
| G / right mouse | Place the held block or station (never on top of anyone) |
| C | Fire (bow: arrows; six-shooter: cartridges) |
| Q / X, 1-0 | Pick a hotbar slot |

- Mining wears a tile down by the pick's power. Ores need a pick of the right tier, and drops go straight into the saddlebags.
- Saddlebags have 30 slots, the first 10 being the hotbar (drawn in-world with a grit bar).
- Crafting stations (workbench, forge, campfire) must be within reach.
- Combat: melee arc plus projectiles, with knockback and a short hurt flash. Dying puts you back at camp at full grit.
- Creatures wander, chase and attack: coyotes, rattlesnakes, scorpions, cave bats, and night riders (bandits) after dark. They spawn by biome in a ring 10-16 tiles out (4 hostile by day, 8 by night) and drift off beyond 48 tiles.
- Day / night: a full day lasts `day_minutes` (8 by default). Dusk washes the screen, and torches and campfires glow.
- Settlers: lay a bedroll in an enclosed room (4-64 floor tiles, at least one door) and a settler moves in and stays home. Jobs are a stub for now.

**Data files** (`<project>/survival/`, written with defaults; edit freely): `items.json` (kind, stack, damage, tier, mine power, ammo, heal, what it places), `recipes.json` (needs, count, station), `creatures.json` (hp, speed, damage, aggro, biomes, night-only, friendly, weight, drop) and `world.json` (seed, world name, day length).

**Editor.**
- **Saddlebags** panel: grit, the time of day, the inventory with drag-to-swap and click-to-select, and a "Make do" list with reasons when a recipe can't be made.
- **Prospector** panel: seed field, Reroll, a live map preview, biome percentages and the spawn-chunk hash. "Burn the claim / regenerate" stakes the new seed and burns `saves/<world>/` after asking.

**Lua (`frontier` table).** All slots are 1-based, like `other:slot()`:
- Inventory: `give(slot, id, n)` returns the leftover; `take(slot, id, n)`; `count(slot, id)`; `selected(slot)`; `craft(slot, recipe)` returns ok, why.
- Health: `health(slot)` returns hp, max; `hurt(slot, n, by)`; `heal(slot, n)`.
- World: `tile(tx, ty)` returns ground, object; `set_tile(tx, ty, ground, object)`; `spawn(creature, x, y)`; `time()` returns phase, is_night; `seed()`.

**Multiplayer.** `ts_server` and `ts_client` now load the project's plugins (`ts_client` needs `--project` to read `project.json`), so the earlier "server ignores plugins" caveat is gone. The server is authoritative for tiles, saddlebags and combat:
- Clients generate chunks from the seed in the server's Welcome. Each client gets the edited chunks near it once, plus every tile edit as it happens. Creature and shot state goes out at 10 Hz, and each rider gets their own saddlebags.
- Clients only send hotbar, craft and swap requests; F / G / C ride in the normal input.
- Plugin messages travel on a new `Plugin` protocol message (protocol v2) through `World::net_post` / `net_deliver`. Plugins can register draw layers, movement blockers and net channels (plugin API v2).

**Sample.** `frontier-survival` is seeded with the other samples when the projects folder is empty. It is a 2D claim with `"plugins": "ts.frontier"`, a rider, a follow camera, and a Lua supply crate (`scripts/supply_crate.lua`) that hands out a starter kit through `frontier.give`.

**Tests.** `ts_admin --smoke` (or just `ts_admin --smoke-survival`) covers:
- Worldgen determinism, printing a region hash to compare machines (seed 1849: `2c74981b44e2ee81`).
- Chunk save / load, including negative coordinates and seed mismatch.
- The Lua crate, crafting by hand and at a placed workbench, mining tiers, stacks, and the housing check.
- Combat: bites, knife, bow, death and respawn, food.
- Posse replication of tile edits, inventory, craft requests and client mining over ENet loopback with plugins on both sides, plus a late joiner receiving the edited chunks.

## Airship Works (2D ship builder plugin)

`ts.shipyard` is a Cosmoteer-style ship builder for 2D projects. It lives in `src/plugins/shipyard` as a built-in of `ts_plugins_builtin`, is **off by default**, and you hitch it in the Outfitter's Wagon (Gameplay). Like Frontier Survival, it adds no scene format: ships are `.ship.json` files under `shipyard/designs/`.

**Parts** (`<project>/shipyard/parts.json`, written with defaults; edit freely). Each part has `id`, `name`, `kind`, a footprint `w` × `h` in 16 px cells, `mass`, `hp`, `power` (+ makes steam, − burns it), `crew` (+ bunks, − hands needed) and, by kind, `thrust`, `range` / `damage` / `fire_rate` / `shot_speed`, `shield` / `shield_regen`, `capacity` and `color`. The stock bin holds:

| Kind | Stock parts |
|---|---|
| hull / armor / corridor | Plank hull, riveted iron plate, gangway |
| engine | Steam paddle engine (1×2), trim jet |
| reactor | Coal boiler (2×2, 12 steam) |
| quarters | Bunkhouse (6 berths) |
| gun | Gatling turret, Long Tom cannon (2×2) |
| shield | Tesla shield vane |
| storage | Cargo hold |

Parts turn in quarter steps. An engine pushes toward its front (rotation 0 = the bow) from the nozzle at its back.

**Designer.** View → **Airship Works (Shipyard)** opens the designer, in the Dusk theme. It works while editing and while riding; saved plans fly from the next launch.
- **Parts bin:** pick a part. Tooltips show its numbers.
- **Slipway:** a 40×40 grid. LMB places (drag to paint), RMB erases, **R** turns the part in hand, Ctrl+LMB turns a placed part, the wheel zooms, and Ctrl+Z / Ctrl+Y undo and redo. A ghost shows green or red for fits. Loose parts get a red rim, and the centre of mass is marked.
- **Checks:** a design is AIRWORTHY or GROUNDED.
  - Errors: parts not bolted to the rest, burning more steam than the boilers make, no forward thrust, overlaps, off-grid parts, unknown part ids.
  - Warnings: no astern thrust, no strafe one way, no turning push both ways, too few bunks, no guns.
- **Stats:** parts, mass, hull hp, steam made and burned, hands and bunks, thrust per direction, acceleration, top speed, turn acceleration, guns, damage per second, shield and cargo.
- **Files:** Save plans, Save as… (`shipyard/designs/<name>.ship.json`), Fresh keel, and **Fly this one**, which sets `player_design` in `sky.json`.

**Flying.** Every rider takes the helm of the flagship (`player_design`). The rider entity is hidden and pinned to the ship's centre of mass, so the follow camera tracks the ship.

| Key | Does |
|---|---|
| W / S | Ahead / astern |
| A / D | Strafe port / starboard |
| Q / E | Turn anticlockwise / clockwise (E is the action button, so Space also turns) |
| F / C / left mouse | Fire (guns lead the nearest pirate in range, otherwise fire along the bow) |

- **Physics:** each ship is a 2D rigid body. Mass, centre of mass and inertia come from the parts.
  - Thrust allocation: every powered engine fires when its push lines up with the helm, or when its torque matches the turn asked for.
  - Linear damping is 0.55/s and angular damping 1.1/s, at a fixed 60 Hz tick.
  - Ships pass through each other by default. Set `"hull_physics": "box2d"` in sky.json to give the hulls Box2D bodies so they collide (see Physics).
- **Steam and crew:** boilers feed engines first, then guns, then shields, in part order. Parts that miss out go offline (dimmed). With `"crew": true` (the default), parts that need hands also need bunks.
- **Combat:**
  - Turrets aim with lead and fire shot.
  - The shield bubble soaks damage first and recharges from powered vanes.
  - Each part has its own hp, and destroyed parts come off. The ship then re-checks connectivity: it keeps the largest piece, and every other piece becomes a captainless derelict wreck. Wrecks keep the velocity their parts had (v + ω×r) and drift.
  - A ship with no parts left is gone. A rider gets a fresh flagship after `respawn_seconds`.
- **Pirates:** each pirate captain picks the nearest hostile ship. It closes to about 0.7× its gun range, orbits there, keeps its bow on the target and fires when in range. Once the pirates are cleared, a fresh wave from `enemies` arrives after `reinforce_seconds`.

**`shipyard/sky.json`:** `player_design`, `crew`, `respawn_seconds`, `reinforce_seconds`, and `enemies` (`[{design, x, y}]`). Designs `stock:sloop` and `stock:marauder` are always available.

**Lua (`ships` table).** Slots are 1-based:
- `of(slot)` returns the slot's ship id.
- `list([team])` returns ids; team is `"posse"`, `"pirates"` or `"derelict"`.
- `info(id)` returns name, team, x, y, vx, vy, angle in degrees, parts, hp, max_hp, mass, shield, ai and slot.
- `spawn(design, x, y, team)` returns the id, or nil and the reason.
- `damage(id, part, n)` returns whether the part was destroyed.
- `remove(id)`, `set_ai(id, on)`, `helm(id, strafe, throttle, turn, fire)` (for captainless ships).
- `stats()` returns parts_destroyed, splits and shots_fired.

**Multiplayer.** The server is authoritative: it simulates every ship, and riders pilot theirs through normal input. On the `shipyard` channel:
- The server sends each new rider its slot (`W`).
- It sends a ship's layout (`F`: parts by id, hp, pose) on spawn, break-up and join, and every 5 s.
- Every 2nd tick (30 Hz) it sends a state message (`S`: pose, per-part hp, powered / firing flags, turret aim, shot).
- Clients mirror this state and never simulate. Part damage and break-ups replicate as they happen.

**Sample.** `sky-pirates` is seeded with the other samples. Existing project folders get it once, with a `.sky-pirates.seeded` marker. It is a 2D claim with `"plugins": "ts.shipyard"` and contains:
- a captain (speed 0, so posse prediction leaves him to the server) and a follow camera;
- the Dust Devil sloop (`shipyard/designs/sloop.ship.json`);
- two Black Kettle marauders;
- a Lua bounty board (`scripts/bounty_board.lua`) that pays $50 for every pirate ship that leaves the sky.

**Tests.** `ts_admin --smoke` (or just `ts_admin --smoke-shipyard`) covers:
- **Designs:** stock parts cover every kind; the sloop and marauder are airworthy; a cut hull gives 5 loose parts; steam budget and missing thrust ground a design; overlap and off-grid placement are refused; rotate and erase; `.ship.json` round trip, with newer versions refused; `parts.json` fallback and edits.
- **Mass and thrust:** mass, centre and inertia of a hand-checked 3-part row; one tick at full ahead equals F/m·dt; turning both ways, strafe, astern and damping; losing the boiler cools the engines; the crew rule.
- **Damage and break-up:** the hull plus 3 derelict wrecks; mass conserved; parts stay put at the break and wrecks drift; an AI duel with shields soaking shot and parts destroyed.
- **Determinism:** a 2v2 AI battle hashed twice over 1800 ticks, plus a 0.5 px nudge that must change the hash. The hash is `27b13a5f46eb6ca5` on both GCC and MSVC Release builds, but the sim uses `std::sin` / `cos`, so other toolchains may differ.
- **Sample ride:** W / E / F, the pinned captain, the Lua bounty board, respawn.
- **Posse replication** over ENet loopback: layouts and state, the client's W flown by the server, pose mirrored, part damage and a break-up replicated, and clients unable to deal damage.

**Caveats.**
- Ships don't collide with each other or with tiles; only shot hits ships.
- Crew is an abstraction: there is no walking crew and no corridor pathing, and gangways are just structure.
- Clients draw the last state they received, without interpolation (30 Hz).
- The designer is an editor panel. Inside `ts_game` you fly saved designs but can't edit them.

## Pathfinder AI toolkit (ts.ai)

`ts.ai` is an optional built-in plugin, listed in the Trading Post as **Pathfinder (AI Toolkit)**. It is off by default; switch it on per project in the Plugins panel or with `"plugins": "ts.ai"` in `project.json`. The Ghost Town 3D sample ships with it on.

**Pieces.**
- `src/ai` (`ts_ai`, no engine dependencies):
  - grid A*, jump point search and Dijkstra;
  - HPA* (`HierGrid`) for big chunked worlds, path smoothing and flow fields;
  - steering (seek / arrive / separation / avoidance, `SteerCrowd`);
  - a Recast/Detour navmesh and crowd. recastnavigation v1.6.0 is fetched by SHA-256 in `cmake/FetchRecast.cmake`;
  - behaviour trees, a blackboard and utility scoring;
  - perception (sight cone, hearing) and a `StateMachine` helper;
  - `AgentBrain`, the BT host both 2D and 3D share.
- `src/plugins/ai` (`AiPlugin`): agents in 2D worlds, the Lua `ai` table, a debug draw layer and Frontier coyote pathing.
- `src/ai3d` (`ts_ai3d`): a `World3DExtension`. On Play it bakes a navmesh from the scene's static colliders plus `nav_link` off-mesh links, puts every entity with an `agent` component into a Detour crowd, and runs its tree.
- `src/editor/plugins/AiPanels.cpp`: the **Behaviour Trees** panel (View menu).

**Behaviour trees (`*.bt.json`).**
```json
{"name": "Guard", "blackboard": {"goal": [560, 0, 120]},
 "root": {"type": "selector", "reactive": true, "children": [
   {"type": "sequence", "children": [{"type": "can_see", "target": "any"}, {"type": "chase", "key": "target"}, {"type": "attack"}]},
   {"type": "sequence", "children": [{"type": "wander", "radius": 160}, {"type": "wait", "seconds": 1, "max": 3}]}]}}
```
- **Composites:** `sequence`, `selector` (memory, or `"reactive": true`), `parallel`, `random`, and `utility` (picks the child with the best score, with hysteresis).
- **Decorators:** `inverter`, `succeeder`, `failer`, `repeat`, `retry`, `cooldown`, `timeout`, `if`.
- **Conditions:** `check`, `chance`, `can_see`, `heard`, `near`.
- **Actions:**
  - **Movement:** `move_to`, `wander`, `chase`, `flee`, `stop`.
  - **Combat and animation:** `attack`, `face`, `play_anim`.
  - **Flow and data:** `wait`, `set`, `inc`, `log`, `succeed`, `fail`, `running`.
- **Blackboard keys** hold numbers (entity ids), text (entity names), bools or positions (`[x, y, z]`).
- **Determinism:** each instance has its own seeded RNG, so a tree replays identically for the same seed.

**Behaviour Trees panel.**
- Lists, opens, creates and saves the project's `*.bt.json` files.
- Has a palette coloured by node kind, and lets you add, move and remove nodes, edit parameters, and see lint warnings.
- During Play it shows the watched agent's live node states, its running branch (`selector > stroll > wander`), its blackboard and its senses.
- Its checkbox turns on the debug overlay: in 2D, paths, blocked cells, BT state and facing; in 3D, navmesh edges, links, paths, BT state posts and sight cones.

**2D (Lua `ai`, positions in px).**
- **Agents:**
  - `ai.agent(e, {speed=, radius=, avoidance=, bt=, sight=, fov=, hearing=})`, `ai.remove(e)`;
  - `ai.bt(e, path)`, `ai.state(e)`, `ai.set_bb(e, k, v[, y])`, `ai.get_bb(e, k)`;
  - `ai.can_see(e, target)`, `ai.heard(e[, within])`.
- **Movement:** `ai.move_to(e, x, y)`, `ai.stop(e)`, `ai.moving(e)`.
- **Paths and flow fields:** `ai.path(x0, y0, x1, y1)` returns a table of `{x, y}`, `ai.path_length(...)` returns the length or -1, and `ai.flow(gx, gy, x, y)` returns `dx, dy`.
- **World:** `ai.noise(x, y[, loudness, what])`, `ai.grid(cell_px)`, `ai.invalidate()`, `ai.debug(on)`, `ai.count()`.
- **Walls and paths:** static solid colliders and solid tiles are walls. Paths use JPS for short hops and HPA* for long ones.
- **Senses:** agents see riders and hear positional sound events.
- **Multiplayer:** AI runs on Local and Server worlds only. Clients get positions through normal replication.

**3D (Lua `ai`, metres, y up).**
- **`agent` component:** `{speed, radius, height, avoidance, bt, wander_radius}`. `nav_link` is `{to, radius, bidirectional}`. Both are scene.json **v13**.
- **Lua:**
  - `ai.agent(e, {...})`, `ai.bt`, `ai.state`, `ai.set_bb` / `ai.get_bb`, `ai.can_see`, `ai.debug`, `ai.count`;
  - `ai.move_to(e, x, y, z)`, `ai.stop`, `ai.moving`;
  - `ai.path_length(x0, y0, z0, x1, y1, z1)`, `ai.noise(x, y, z[, loudness, what])`.
- **Movement:** the crowd drives position and yaw, and feeds the animator's `speed` parameter.
- **Old Hank** in the Ghost Town wanders the street with `ai/hank.bt.json`. He stops and faces riders he can see, and hears shots.
- **Hosts:** the editor's 3D Play and `ts_game` attach the extension when the project enables `ts.ai`.

**Graphs, C++ and C#.**
- **Graph nodes** (AI category): Make Agent, Run Behaviour Tree, AI Move To, AI Stop, Set / Get Blackboard and AI State. Plugin Call reaches the rest.
- **C++:** `#include <tombstone/ai.h>`, then `ts::ai::agent(*this, self(), 120)`, `ts::ai::move_to(...)`, `ts::ai::state(...)` and so on.
- **C#:** `this.AiAgent(Self, 120)`, `this.AiMoveTo(...)`, `this.AiState(...)` and so on.
- All of them go through the plugin bridge to the same `ai` table.

**Frontier.** With `ts.ai` on, chasing coyotes plan a short JPS path around fences and walls, replanning twice a second, instead of running straight into them. Shipyard pirates are unchanged.

**Smoke.** `ts_admin --smoke-ai` (also part of `--smoke`) checks:
- **Grids:** A* = JPS = Dijkstra on random maps, weighted costs, HPA* within 1.25× optimal on 512², and flow fields.
- **Navmesh and crowds:** navmesh build plus paths, raycast and off-mesh links; Detour and steering crowds with no overlap.
- **Behaviour trees, perception and the state machine:** BT semantics, a determinism trace and round trips; perception and the FSM.
- **The plugin in a 2D world:**
  - a BT guard, a Lua agent and a graph agent go round a wall;
  - an agent hears a positional gunshot;
  - the bridge works, and clients don't simulate;
  - coyotes steer around fences;
  - the BT panel draws headless.
- **3D:** v13 round trip, crowd crossing, a climber taking a `nav_link`, and Old Hank wandering and hearing.

**Caveats.**
- The 3D navmesh is baked once when Play starts, from static colliders as boxes. Moving or toggled colliders do not re-bake it, and capsules count as boxes.
- In 2D, the walkable grid is cached per world. Call `ai.invalidate()` after moving static solids or editing tiles from script.
- World3D has no network roles yet, so 3D AI runs wherever the world runs. VR rides and `ts_game` VR don't attach it.
- The agent and nav_link components have no inspector UI yet. Edit them in scene.json or set them from Lua `ai.agent`.
- The `lua` BT action isn't wired to entity scripts yet (it fails). Use the blackboard from scripts instead.
- 2D agent avoidance is positional separation; agents may brush by up to ~1 px.

## Audio (ts_audio)

Sound runs on [miniaudio](https://miniaud.io) 0.11.21 (wav / mp3 / flac) plus
stb_vorbis (ogg), both fetched by SHA256 (`cmake/FetchMiniaudio.cmake`). With
no sound card, or `TS_AUDIO=null`, the engine uses a null backend that still
mixes, so smokes, servers and CI run headless.

- **Sounds**: drop `.wav/.ogg/.mp3/.flac` into `<project>/assets`. Built-in
  placeholders are generated in code (CC0 by construction): `builtin:click`,
  `step`, `gunshot`, `mine`, `boom`, `grab`, `throw`, `chime`, `tune`.
- **Buses**: master / music / sfx / ui / voice with volume + mute. Music
  streams on two decks with crossfades and playlists. One-shots take
  pitch / volume variation.
- **Space**: 2D equal-power pan + distance fade from the camera; 3D
  positional audio with the listener on the camera (or the headset in VR),
  linear attenuation and optional doppler.
- **Audio Source component** (scene.json v8 for 2D, v10 for 3D; older scenes
  load unchanged): clip, bus, volume, pitch, autoplay, loop, positional,
  near / far, doppler.
- **Lua**: `audio.play(clip, {volume, pitch, variation, bus, loop, at=entity,
  x, y, z})`, `audio.music(clip, fade)`, `audio.playlist({...}, fade)`,
  `audio.stop_music(fade)`, `audio.set_volume(bus, v)`, `audio.stop(...)`.
- **Multiplayer**: on a server, every sound becomes a `ts.audio` event and
  is replayed on the clients (each hears it from its own camera).
- **Editor**: *View > Saloon Band (Mixer)* (faders, level lamps, mute
  "hats", music deck, voices). The Supply Wagon lists sounds with Play /
  Stop, drag onto the Inspector, or double-click to assign. The Inspector
  edits Audio Sources in 2D and 3D. Play, the posse (heard through the viewed
  rider), 3D Play and the VR preview all make sound.
- **Gameplay**: Frontier Survival footsteps, mining knocks and gunshots;
  Airship Works guns and breakups; VR grab / throw / comfort-menu clicks;
  editor toolbar clicks.
- **Settings**: mixer levels save to `TombStoneConfig/audio.json`. ts_game
  and ts_client read the same file.
- **Smokes**: `ts_admin --smoke-audio` covers decoding, bus levels on the
  null backend, pan / attenuation / doppler math, crossfade + playlist,
  scene roundtrips, events through Lua and server to client, mixer
  persistence, editor preview, and a World3D ride. The survival, Shipyard
  and VR smokes check their gameplay sounds.

## In-game UI (ts_ui)

A retained UI layer shared by `ts_game` (2D and 3D), `ts_client`, the VR
runtime and the editor's Play view.

- **Fonts:** TTF via stb_truetype (fetched by hash) baked into one 1024x1024
  atlas: **Rye** (display) and **Arvo** (body), both SIL Open Font License
  1.1. The license texts ship in `assets/fonts/OFL-*.txt`.
- **Widgets:** label, button, image, panel, progress, slider, text input, list.
  Anchors, pivots and offsets on a 1280x720 reference that scales with the
  window height. Panels lay out children as a `stack` or a `grid`. The theme is
  the Dusk palette (`style`: accent / muted / title / clear).
- **Navigation:** mouse, keyboard (arrows/Tab/Enter/Esc) and gamepad (d-pad, A, B).
  Modal documents and focused text inputs take the keys. Otherwise the game keeps them.
- **Data:** `<project>/ui/*.ui.json`. Each file is one document
  (`name`, `visible`, `modal`, `layer`, `focus`, `root`). The writer saves only
  non-default fields, so round trips are byte-stable.
- **Lua:** `ui.show(doc)`, `ui.hide(doc)`, `ui.set_text(id, s)`,
  `ui.set_value(id, v)`, `ui.set_visible(id, on)`, `ui.on_click(id, fn)` and
  `ui.on_change(id, fn)`. Handlers are called as `fn(id, value, text)`.
- **Built in:** Esc or pad Start opens a pause menu (Resume / Settings with
  Master / Music / Effects volume sliders, saved / Quit). In-game toasts are
  drawn as text cards in 3D. The Frontier hotbar shows real item counts and
  the selected item's name.
- **VR:** the UI is drawn on a world-space panel (0.64 x 0.36 m) in front of the
  rider. You click it with the laser and trigger.
- **Samples:** `ghost-town-3d` ships a main menu template
  (`ui/main_menu.ui.json`, wired by `scripts/town.lua`).
- **Editor:** Play shows the project UI over the viewport. **View > UI Preview**
  renders any document live without entering Play.
- **Smoke:** `ts_admin --smoke-ui` covers the atlas, text measure, layout math,
  navigation, data round trips, the pause menu, the software raster and Lua `ui.*`.

## Particles + 2D lighting (ts_fx)

**Particles.** Any 2D or 3D entity can carry a `particles` component
(scene.json v9, scene3d v11; older files load unchanged). An emitter has
burst and continuous emission, point / circle (sphere) / box / line shapes,
lifetime, speed + spread, gravity, drag, 3-key size and colour curves,
spin, a texture or sprite sheet (frames over life or at an fps), alpha or
additive blending, local or world space and a deterministic seed (0 = from
the entity id). Presets: `dust`, `gun_smoke`, `muzzle_flash`, `sparks`,
`campfire`, `rain`, `sand_storm`, `thruster`, `explosion`, `sparkle`
(2D in pixels, 3D in metres). The editors have a Particles inspector with a
live preview (click it to restart) in both 2D and 3D.

Lua:

```lua
particles.burst("sparks", self, 20)          -- preset at an entity
particles.burst("dust", {x = 100, y = 40})    -- preset at a point ({x,y,z} in 3D)
particles.burst(self)                         -- the entity's own emitter, once
particles.emit(self, 10)                      -- extra particles now
particles.stop(self); particles.start(self)
```

Gameplay hooks: Frontier Survival mining debris (dust + sparks), tile breaks,
muzzle flash + gun smoke; Airship Works muzzle flashes, break-up explosions
and thruster plumes; 3D VR grab sparkle. In multiplayer, bursts travel as
`ts.fx` events from the server; cosmetic local effects (thrusters) are not
sent. 3D particles are drawn as soft camera-facing billboards in ts_game,
the VR presenter and editor Play.

**2D lighting.** A scene `lighting` block (View > Lighting in the 2D
editor) turns it on: ambient tint, optional day / night cycle (day + night
ambient, day length, start time) and lightmap resolution. Entities carry a
`light` component (point or spot: colour, intensity, radius, angle, cone,
softness, flicker, shadows). Solid tiles cast soft shadows (CPU ray march,
3 rays per texel, blurred) and the lightmap multiplies the world under the
HUD. Plugins can add lights, occluders, ambient overrides and HUD layers
through the Registrar (`add_light_source`, `add_light_occluder`,
`add_ambient`, `add_hud_layer`); Frontier Survival uses these for torches,
campfires and night instead of the old glow squares.

Smoke: `ts_admin --smoke-fx` (also part of `--smoke`).

## The Vault (asset library)

The Vault is an asset library shared by every claim: send a texture, model, sound, font or prefab to it from the Asset Browser (right-click > **Send to Vault...**), then pull it into any other claim from the **Vault** panel in the 2D or 3D editor. The panel has thumbnails, search, kind / tag / 2D / 3D / VR filters, versions, pins and update badges. A pull copies the files together with their `.meta`, so the GUID and references stay the same in every claim, and dependencies come along (a model brings its textures). The library is a local folder (default `Documents\TombStone\Vault`) or `ts_vault`, a small self-hosted HTTP server with API keys, SHA-256 content addressing, resumable chunked uploads and a SQLite index. Exports leave out pulled assets the game doesn't use. CLI: `ts_admin --vault push|pull|list|status|update`. Smoke: `ts_admin --smoke-vault` and `ts_vault --smoke`. See [docs/vault.md](docs/vault.md), including hosting `ts_vault` on Linux and Windows. You are responsible for having the rights to anything you upload to a Vault.

**Live Team Claims (0.2.0).** Ride a claim with your posse in real time: **Team > Host from my editor** on a LAN, or `ts_vault --team` so one self-hosted server does the Vault and team sessions with the same API keys. Every undo step goes out as one op batch with server-ordered sequence numbers; the posse shows in the Team panel with colours, remote cursors and selection outlines in the viewport, and soft locks (`[locked: name]` in the Hierarchy) while someone has an entity selected. Late joiners catch up from a snapshot plus the op tail, and mismatched scene formats are turned away. Smoke: `ts_admin --smoke-team`. See [docs/team-claims.md](docs/team-claims.md).

## Physics (Jolt 3D / VR, Box2D 2D)

Both engines are fetched by hash and are deterministic: the smokes check the same state hash on Linux (GCC) and Windows (MSVC).

### 3D and VR (Jolt Physics v5.3.0)
- **Components** (Scene3D v14), in the 3D inspector:
  - **Collider:** box, capsule, sphere, convex hull or mesh, with compound parts, a trigger flag and a collision layer.
  - **Rigid Body:** static, kinematic or dynamic, plus mass, friction, restitution, damping and gravity scale.
  - **Joint:** fixed, hinge, slider, distance or ball, with a target entity, anchor, axis and limits.
- **Physics Settings** (View > Physics Settings): gravity, sub-steps, layer names, a symmetric collision matrix, and "debug draw in Play". All edits are undoable.
- **Gizmos** (View > Colliders + joints): static colliders are green, dynamic orange, kinematic blue and triggers yellow. Joints show the anchor and the hinge or slider axis. Hull and mesh colliders are drawn as their model bounds.
- **VR:** held objects are kinematic bodies that follow the hand. They knock things over, hit each other, and keep the hand's velocity when thrown. Impacts fire `on_impact`, sounds and dust.
- **Desktop physics gun** (editor Play and ts_game):

  | Input | Action |
  |---|---|
  | Hold F (or left mouse in ts_game) | Carry the body under the crosshair |
  | G, or left click while carrying | Throw it |

### 2D (Box2D v3.1.1)
- **Rigid Body** and **Joint** components (scene.json v10) sit in the 2D inspector under the collider.
  - Joints: weld, revolute, prismatic, distance, with limits.
  - Colliders turn with the entity and can be a box, circle or capsule.
- **Units:** px, y down, degrees clockwise, 32 px per metre, gravity 980 px/s².
- **Riders:** the platformer player stays on its own solver. It stands on bodies and shoves them through kinematic proxies.
- **Collision overlay:** draws the real rotated boxes, circles and capsules, with a centre dot and heading tick on each body. It follows the bodies in Play.

### Scripting
The same API is available in every language:

| Language | Access |
|---|---|
| Lua | `physics` table (`physics3d` in 3D) |
| Graphs | Physics nodes |
| C++ | `tombstone/physics.h`, via `s.plugin(...)` |
| C# | `TombStone.Scripting.Physics` |

Calls include:
- `apply_force`, `apply_impulse`, `apply_torque`
- `set_velocity`, `velocity`, `speed`
- `raycast`, `ray_distance`
- `joint`, `unjoint`, `joint_value`
- `bodies`

### Airship Works hulls (opt-in)
Set `"hull_physics": "box2d"` in `shipyard/sky.json` to fly hulls as Box2D bodies built from the part boxes.
- Ships and wrecks then collide.
- Mass and inertia still come from the parts, so the handling is unchanged.
- The default (`"builtin"`) keeps the original integrator with no hull contact, and the same determinism hashes.

### Smokes
`ts_admin --smoke-physics` covers the 3D core, the VR saloon, the 2D world and the headless editor panels. `--smoke-shipyard` covers the Box2D hulls.

### Limits
- 2D gravity is fixed.
- 2D circle and capsule bodies block riders as boxes.
- Plugin blockers are not in Box2D.

## Project Goals

- Deliver a fast, stable, and fully custom C++ engine  
- Provide a clean development workflow through custom tooling  
- Maintain strict control over engine behavior and architecture  
- Support high-framerate gameplay and editor performance  
- Offer a clear separation between engine development and game development

---

## Build Requirements

- C++20 compatible compiler  
- Supported graphics API (Vulkan/DirectX/OpenGL depending on configuration)  
- Platform-specific SDKs as required  
- Custom build system included in this repository  

---

## License

This project is **proprietary** and **all rights reserved**.  
No usage, copying, modification, or distribution is permitted without explicit written permission.

See the `LICENSE` file for full details.

---

## Status

Active development.  
Major systems are being rebuilt and optimized as part of the current engine iteration.

---

## Repository layout

```
CMakeLists.txt          # Root: TombStoneEngine, C++20, TS_BUILD_* options
cmake/                  # Shared CMake helpers (TombStoneOptions.cmake)
src/core/               # Engine core (types, assert, Engine, JsonMini)
src/platform/           # Platform / window stubs
src/render/             # Renderer stubs
src/input/              # Input stubs
src/net/                # Transport interface + ENet backend, link simulator (lag / jitter / loss), byte codec
src/mp/                 # Multiplayer: wire protocol, GameServer, GameClient (prediction + interpolation), LocalPosse, posse smoke
src/physics/            # 2D physics stubs
src/physics2d/          # Box2D v3 wrapper (px units): 2D rigid bodies, joints, Airship hulls
src/physics3d/          # Jolt wrapper: 3D / VR rigid bodies, joints, character, casts
src/scene/              # Scene data: Entity2D + components, TileMap, scene.json read/write (no UI, no GL)
src/runtime/            # Game runtime: World, PlaySession (60 Hz fixed step), abstract input, Lua script host, Telegraph log (no UI, no GL)
src/vault/               # The Vault: SHA-256 blob store + SQLite index, HTTP client / server, local + remote backends, project lock
src/gfx/                # stb_image TextureCache + OpenGL texture uploader (admin + game)
src/editor/             # Admin UI (AppFlow, settings, projects, workspace, Play mode, launcher, ui, screens)
cmake/FetchImGuiDeps.cmake  # FetchContent GLFW + stb_image (admin + game) and Dear ImGui (admin only)
cmake/FetchLua.cmake    # FetchContent Lua 5.4.8 (hash-pinned, core + pure libs only, built as C++)
cmake/FetchENet.cmake   # FetchContent ENet 1.3.18 (hash-pinned; ws2_32 + winmm on Windows)
apps/admin|client|game|server|vault/ # Build targets controlled by TS_BUILD_* (vault = ts_vault, the self-hosted Vault server)
```

### Admin flow (ImGui)

`ts_admin` opens a GLFW + OpenGL3 window and drives ImGui screens owned by `src/editor/`:

1. **Loading** -- splash title + progress bar; auto-advances to Login  
2. **Login** -- username/password, Login, Dev login, Settings  
3. **ProjectManager** -- list projects, Open / double-click, New 2D (validated name), Settings, Logout; errors shown in-UI  
4. **Settings** -- edit `projects_root` (Browse opens an in-app ImGui folder picker), theme, `auto_login_dev`, username; Apply/Save still validates, writes settings, and reloads projects  
5. **Editor2D** -- live Hierarchy (create/duplicate/rename/delete, multi-select), Viewport2D canvas (drag-to-move, box select, snap-to-grid, pan MMB/RMB/Alt-drag, wheel zoom), Inspector (name, snapped transform, color/tint, layer/z, TileMap / Sprite / Player / Camera2D / Spawn Point / Collision / Animator / Script sections) + Back, **Tile Palette** (tilemap painting), **Stable** (animation sets), **Supply Wagon** (project images), **Scripts** (Lua files) and the **Telegraph** (console); File → Save Scene; **Play / Pause / Stop** runs the scene in the Viewport  

Namespace: `ts::tombstone::editor`.

**Editor2D workspace** loads/saves `<project>/scene.json` (entities: id/name, x/y/w/h, rgba tint, layer/z, optional `tilemap` / `sprite` / `player` / `camera` / `spawn` objects, plus pan/zoom/grid, `grid_size`, `snap`, `selection`; older files without the new keys still load). Opening a 2D project loads scene.json when present, otherwise seeds Camera2D / Player / TileMap and writes an initial scene.json. Hierarchy/Inspector edits autosave; leaving the editor (Back / Quit) also saves. Hierarchy edits and Inspector fields update the viewport immediately. **Settings → Browse...** uses a portable ImGui directory browser (no extra native deps); Apply remains the step that validates/writes `projects_root` and reloads the project list.


### UI identity (Admin shell)

Admin has its own look, kept in `src/editor/ui/Theme` (palette, metrics, custom-drawn chrome) and `src/editor/ui/Brand.h` (copy). The tone is frontier/western with a gothic edge, not cartoonish:

- **Dusk** (default): deep charcoal base, warm stone/sand neutrals, bone text, **amber/gold** primary accent, **rust/copper** secondary. **Parchment** is the light variant (warm paper, ink-brown text, darker gold). Pick either in Settings → Lamplight.
- Sharp 2-3 px corners with 1 px inked frame borders. Custom-drawn section headers (amber diamond, label, caption, fading copper rule), ornamental dividers, a headstone mark, title strips, and a branded progress bar. Hover and active states go copper and amber.
- Copy keeps one short, dry voice: splash "TOMBSTONE / Four riders. One frontier. Build it right.", login "Sign the ledger. Ride in.", Project Manager "Claims" / "Break Ground" / "Bury Project?", and Settings sections "Territory / Rider / Lamplight / Trail". The editor status bar carries the brand, and the window title follows the open project.

Editor2D runs in an ImGui **Docking** layout (Hierarchy over Supply Wagon and Scripts / Viewport2D over the Telegraph / Inspector over Tile Palette and Stable) with a tool strip (Undo, Redo, Select/Brush/Erase/Fill/Rect/Pick tools + brush swatch, Grid, Snap, cell size, Duplicate, Delete, Reset Cam, Save) and a status bar (fps, zoom, grid/snap, selection, Telegraph badge, recent action, path). Docking and multi-viewport flags are enabled in `apps/admin/main.cpp`.

### Editor2D tools

| Action | Input |
|---|---|
| Undo / Redo | Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z (also Edit menu and toolbar) |
| Move selection | LMB drag in Viewport (Esc cancels mid-drag) |
| Multi-select | Ctrl+click (toggle) / Shift+click (add; Hierarchy: range) |
| Box select | LMB drag on empty canvas (Ctrl/Shift adds) |
| Duplicate / Delete | Ctrl+D / Del (also toolbar, Hierarchy, Edit menu) |
| Nudge | Arrows (1 px, or 1 cell with snap); Shift+Arrows (10 px, or 4 cells) |
| Snap / grid | Shift+G snap, G grid, `[` `]` halve/double cell size, or toolbar cell field |
| Select all / clear / rename | Ctrl+A / Esc / F2 |

When snap is on, drag moves, nudges, duplicates, and Inspector X/Y/W/H edits all land on the grid. In a group drag the primary snaps and the others keep their spacing. The Inspector edits the primary selection (amber outline) and shows the selection count. Every tool autosaves `scene.json`. `--smoke` covers snap, nudge, multi-select, box select, duplicate, and delete, plus autosave and persistence.

**Undo / redo** covers every workspace edit: create, rename, delete (single or multi), duplicate, drag-move, arrow nudge, snap-selection-to-grid, Reset Scene, and Inspector edits (name, X/Y/W/H, tint, layer). History is snapshot based (entity list, including each entity's tile grid and sprite, + selection), capped at 200 steps, kept in memory only, and cleared when a project is opened or closed. Continuous edits coalesce into one step: a whole viewport drag is one "Move", an Inspector field is one step from activation to release, and held or rapid arrow nudges (within 0.6 s) are one "Nudge". The Edit menu names the step ("Undo Move 3"), and the status bar reports "Undid: Move 3" / "Redid: Duplicate 2". Any new edit clears the redo stack. Undo and redo restore the selection from that step and autosave `scene.json`. Grid, snap, and camera are view settings and are not tracked. While a text field is active, Ctrl+Z belongs to the text field. `--smoke` runs six edits, undoes all of them back to the initial state, redoes them, checks that a new edit clears redo, and confirms `scene.json` after each step.

### Editor tools (2D + 3D)

Mouse-first editing in both editors. Every tool is one undo step, autosaves `scene.json`, and is off while playing. `--smoke` (or just `--smoke-tools`) drives each tool through the same code paths as the mouse and keys.

| Action | 2D | 3D |
|---|---|---|
| Resize | Drag the 8 handles on the primary selection (Shift keeps the aspect; snap rounds to the grid) | Scale gizmo (R) |
| Rotate | Drag the round knob above the box (Shift or snap: 15° steps) | Rotate gizmo (E) |
| Duplicate while dragging | Alt+drag an entity: the copies move and the originals stay (Esc takes the copies back) | Alt+drag a gizmo handle |
| Snapping | Grid plus other entities' edges and centres, with red guide lines (View > Snap to Objects) | Grid steps; hold Ctrl while moving to stick to the surface under the mouse; End = drop to ground (lands on the entity below or y = 0) |
| Context menu | Right-click in the viewport or Hierarchy: rename, duplicate, delete, copy/cut/paste, group/ungroup, unparent, lock, hide, focus, align/distribute, add component, and (always last) New Entity / New Child Entity | Same, plus Create |
| Frame selection | F (Bucket fill moved to U) | F |
| Copy / cut / paste | Ctrl+C / Ctrl+X / Ctrl+V. The clip is scene.json text on the system clipboard, so it pastes into another scene or project (ids remapped, parent links inside the clip kept) | Same |
| Group / ungroup | Ctrl+G (a transparent "Group" around the selection) / Ctrl+Shift+G | Ctrl+G (empty "Group" at the selection centre) / Ctrl+Shift+G |
| Lock / hide | Ctrl+L / Ctrl+Shift+L, H / Shift+H, or the eye and lock toggles on each Hierarchy row. Locked entities can't be picked or moved. Hidden is editor-only, and the game still draws them | Same (`editor_hidden`) |
| Parenting | Drag a Hierarchy row onto another: top quarter = before, bottom quarter = after, middle = child. Drop on empty space to unparent | Same |
| Align / distribute | Edit > Align / Distribute or the context menu (left/centre/right/top/middle/bottom; distribute needs 3+) | Min/centre/max and distribute on X/Y/Z |
| Pan / zoom | Wheel zooms toward the cursor; MMB drag, RMB drag, or Alt+drag on empty canvas pans | RMB orbit/fly, MMB pan, wheel dolly |
| Search | Filter box at the top of the Hierarchy (name; parents of a match stay listed) | Name filter |

Children keep world-space transforms and follow their parent's moves, resizes and turns in the editor, through Inspector edits, and at runtime (`ts_game`, Play, the posse). Moving, duplicating, copying or deleting a parent takes its subtree along. In 2D, rotation draws in the editor, Play, the posse view and `ts_game`, but runtime colliders stay axis-aligned.

### TileMaps (painting)

A TileMap entity owns a grid of tile ids: width x height in cells, tile size in px, origin at the entity's x/y (its w/h always follow the grid). `0` is empty. Any number of TileMaps can live in a scene (seeded scenes get an 8x2 painted "TileMap"; **Edit → Create TileMap** or the Tile Palette's **+ TileMap** stakes a 16x8 one).

Tiles come from the TileMap's **tileset** image when one is assigned (sliced left-to-right, top-to-bottom by tile size; id 1 = top-left slice) or, with no image, from a built-in 16-tile frontier palette (Dirt, Sand, Stone, Grass, Water, Wood, Clay, Adobe, Dry Brush, Mud, Brick, Cactus, Iron Rail, Coal, Gold Ore, Bone), so painting works with zero assets. A missing tileset falls back to the built-in colours.

| Tool | Key | Use |
|---|---|---|
| Select | V | Original pick / drag / box-select behaviour |
| Brush | B | LMB click or drag on the selected TileMap; a whole stroke (mouse down → up) is one undo step |
| Erase | E | Same as Brush, clears cells |
| Fill | U | Bucket-fill the 4-connected region matching the clicked tile |
| Rect | R | Drag a box of tiles (hold Shift on release to erase the box) |
| Eyedropper | I or Alt+click | Pick the tile under the cursor as the brush (I then returns to Brush) |
| Brush size | 1 / 2 / 3 | 1x1, 2x2, 3x3 (also in the Tile Palette) |
| Cancel stroke | Esc | Mid-stroke: restores the tiles, no undo step |

The viewport shows the brush footprint / fill cell / rect box under the cursor and cell lines on the TileMap being painted; the status bar shows the tool, hovered cell and tile id. Picking a tile tool with no TileMap selected selects the first one; clicking another TileMap retargets the brush. In tile tools Alt+LMB over the TileMap picks a tile, so pan with MMB/RMB there. The Inspector's TileMap section edits Cols / Rows (resizing keeps existing tiles; Enter or +/- applies), Tile px, and the tileset (picker or drop an image from the Supply Wagon), plus Clear Tiles. Every paint stroke, fill, rect, resize, tile-size and tileset change is one undo step and autosaves `scene.json`.

### Sprites and the Supply Wagon (assets)

Images live in `<project>/assets/` (created when a project is made or opened). The **Supply Wagon** panel lists every image there (png/jpg/bmp/tga/gif/psd/pnm) with a thumbnail and size, refreshing every couple of seconds. **Import...** opens the in-app file picker and copies the image in (never overwrites: `name_2.png`). Drag an image onto the viewport to stake a sprite entity centred on the drop point and sized to the image; double-click or right-click assigns it to the selected entity, or as the selected TileMap's tileset.

Any non-TileMap entity can carry a sprite: project-relative image path, optional source rect (x/y/w/h, 0 = to the edge), flip X/Y, tinted by the entity colour (**White tint** shows the image as drawn). The viewport draws the textured quad (nearest filtering) instead of the rect; a missing or unreadable file falls back to the tinted rect with an amber "!" marker and a tooltip. Sprite assignment, removal, flips, source rect, new sprite entities and "Fit to image" are all undoable.

Images are decoded with [stb_image](https://github.com/nothings/stb) (public domain / MIT, fetched at configure time for admin builds, pinned by commit + SHA-256). Decoding and caching (`src/gfx/TextureCache`) are renderer-agnostic: the admin app registers an OpenGL uploader, while `--smoke` runs headless and only reads image sizes. The cache keys by path and reloads a file when it changes on disk.

### Play mode (run the game inside the editor)

Hit **Play** and the scene rides: the Viewport stops being an editing canvas and runs the real game runtime on a snapshot of the scene. **Stop** throws the run away and puts the editor back exactly as it was: entities, selection, camera, undo history. Nothing done while playing writes `scene.json` or adds an undo step.

| Action | Input |
|---|---|
| Play / Stop | F5 or Ctrl+P (toolbar Play / Stop, Play menu) |
| Pause / resume | F6 (toolbar Pause) |
| Step one tick (paused) | F10 (toolbar Step) |
| Move player 1 | WASD or arrow keys; gamepad left stick / d-pad |
| Action | E or Space; gamepad A (opens the sample gate, talks to signs) |
| Gamepads | pads 1-4 drive player slots 1-4 (pad 1 shares slot 1 with the keyboard) |
| Free camera | C toggles; drag to pan, wheel to zoom (C again hands back to the follow cam) |
| Collision overlay | K (also Play menu; works in edit mode too) |
| Launch Game | toolbar Launch / File menu: saves, then starts `ts_game` on this project |

While riding the viewport gets a copper frame and a `RIDING mm:ss tick N` badge (amber `PAUSED` and a dimmed scene while held); the status bar shows the play clock, tick count, measured ticks/s, player 1's position and the camera mode. Edit tools, the Hierarchy, Inspector, Tile Palette and Supply Wagon are locked, and Save is refused with a note. The Scripts panel and the Telegraph stay live: edit a script, save, and watch the ride pick it up. Script toasts show as cards at the bottom of the Viewport. **Esc never ends a ride**: only F5 / Ctrl+P / Stop do. Play flushes any half-finished edit and autosaves first, so `ts_game` and Play see the same scene.

Gameplay components are edited in the Inspector (each change is one undo step):

- **Player**: player slot P1-P4 and speed (px/s). Movement is 8-way; diagonals are not faster.
- **Camera2D**: follow target, smoothing (seconds; 0 = locked on), zoom, optional bounds rect (**Fit to TileMap**). With no Camera2D the game follows player 1.
- **Spawn Point**: the player in this slot starts centred on it.

New projects seed **Player** with a P1 controller, a dynamic collider and the animated sample cowboy (see Sprite animation), **Camera2D** following it, and the scripted **Corral Gate** and **Gold Nugget** (see Gameplay scripting).

### Collision and solid tiles

The game is top-down with no gravity: up to four riders move freely and the runtime (`src/runtime/World_Collision.cpp`, no UI or GL) keeps them out of the walls.

- **Solid tiles.** Every tile id in a tileset is solid or walkable. The built-in palette starts with Stone, Water, Wood, Adobe, Brick, Cactus, Coal and Gold Ore solid (Dirt, Sand, Grass and the rest are open ground); an image tileset starts with nothing solid. Project overrides live in `scene.json` under `tile_solidity` (per tileset, so every TileMap using that tileset agrees).
- **Colliders.** Any entity can carry an axis-aligned box collider: offset and size relative to the entity, **solid** or **trigger**, **static** or **dynamic**. Static solids (crates, fences, walls) block riders and never move. Dynamic solids (riders, barrels) get pushed. Entities without a collider are scenery.
- **Movement.** Each tick a rider moves X first, resolves against solid tiles and static solids, then Y, so pushing diagonally into a wall slides along it. A move longer than half the thinnest obstacle (half a tile, half a 4 px fence) is split into substeps, so nothing tunnels even at thousands of px/s. Riders stop flush: a 32 px rider stopped by a wall at x = 192 has its right edge at exactly 192.
- **Dynamic vs dynamic.** After moving, overlapping dynamic bodies are pushed apart along the axis of least overlap, half each (a few passes), so riders never finish a tick inside one another and a rider can shove a barrel.
- **Triggers.** A trigger collider never blocks. Each tick the World records enter / exit events for riders and dynamic bodies (exits before enters, each fired exactly once). The editor's Play status bar and the K overlay show the latest one ("Player rode into Gate (tick 171)"), and `ts_game` shows it in the window title. A trigger has no behaviour of its own: scripts hook it with `on_trigger_enter` / `on_trigger_exit` (see Gameplay scripting), and every event also lands on the Telegraph.

**Solid-tile workflow.** In the **Tile Palette**, solid tiles carry a red corner wedge. **Right-click** a tile to toggle it, or use the **Solid** checkbox next to the brush tile. Each toggle is one undo step ("Solid: Grass" / "Walkable: Grass"), autosaves `scene.json`, and applies to the tileset of the TileMap being painted (or the built-in palette).

**Colliders in the Inspector.** The **Collision** section shows **+ Collider** (sized to the entity: dynamic for players, static otherwise), then Solid / Trigger, Static / Dynamic, Offset and Size, **Fit to Entity** and **Remove Collider**. Every change is one undo step, and a whole drag of Offset or Size is one step.

**K overlay.** **K** (or **Play → Collision Overlay**) toggles the collision overlay in both the edit Viewport and Play:

| Colour | Shows |
|---|---|
| Red | Solid tiles (each row merged into runs) |
| Copper | Static solid colliders |
| Green | Dynamic solid colliders (riders, barrels) |
| Gold with a cross | Triggers (filled while somebody is inside) |

A legend sits in the Viewport's bottom-left corner. In edit mode the overlay reflects the scene as edited, and in Play it reflects the live World. K is ignored while typing in a text field.

### Sprite animation (the Stable)

Animation is data in `src/scene/Animation` and a per-tick system in `src/runtime/World_Anim.cpp`, so the editor's Play mode and `ts_game` run the same code. Neither knows a renderer: the World hands out the current frame as UVs in its draw list.

**Animation sets.** A sprite sheet gets a `<name>.anim.json` beside it in `<project>/assets/` (`assets/rider.png` -> `assets/rider.anim.json`). It holds the grid the sheet is cut into and a list of named clips. Any number of entities can share one set; editing the set changes all of them.

**Animator component.** An entity carries `set` (the `.anim.json`, project-relative), `clip` (the clip the ride starts on; empty = the default), `default_clip` (empty = the set's default), `speed` (0..16, 1 = as authored) and `playing` (off holds the frame). The sprite keeps its image and source rect as the stand-in if the set goes missing.

**Timing.** Animators advance on the fixed 60 Hz tick by `1/60 x speed` seconds, never on the frame rate. A clip runs at `fps` or, when `ms` lists per-frame durations, at those. Modes:

| Mode | Frames for a 4-frame clip |
|---|---|
| `loop` | 0 1 2 3 0 1 2 3 ... |
| `once` | 0 1 2 3 3 3 ... (then reports finished) |
| `ping_pong` | 0 1 2 3 2 1 0 1 2 ... |

**Riders pick their own clips.** An entity with both a Player controller and an animator picks `walk_<dir>` while its stick is pushed and `idle_<dir>` when it lets go, with `<dir>` one of `down`, `up`, `left`, `right`. The heading follows the dominant axis of the input (a 0.2 deadzone). On a diagonal it keeps the current heading if that key is still held, otherwise the horizontal one wins. Idle keeps the last heading, so a rider that stops after riding left stands facing left. Sets can be thinner than four directions:

1. `walk_left` missing: `walk_right` drawn mirrored (and the other way round).
2. `walk_up` / `walk_down` missing: the side clip the rider last faced.
3. Then `walk_down`, then a bare `walk` (mirrored when facing left).
4. Idle runs the same chain with `idle`. A walking rider whose set has no walk clips at all uses the idle chain. If nothing matches, the rider plays the animator's default clip, then the set's.

Turning while walking keeps the stride (walk_right frame 2 becomes walk_down frame 2); stopping starts the idle from frame 0. Other animators play their start clip (the authored `clip`, else the default) and never switch on their own.

**Playing a clip by name.** `World::play_clip(id, "tip_hat", restart = true, hold = false)` switches an animator to any clip in its set and returns false when there is no such clip. On a rider the next auto-pick takes over again unless `hold` is true; `World::release_clip(id)` hands it back. Scripts call it as `e:play(clip, hold)` / `e:release()`.

**The Stable** (View -> Stable (Animation), tabbed beside the Tile Palette) is the animation-set panel:

- **Sheet**: pick any Supply Wagon image (`[set]` marks one that already has a `.anim.json`). A sheet without one starts a new set cut into 32 px cells, written on the first edit. **Open in Stable** in the Inspector opens an animator's set.
- **Slice**: frame W x H, then **Slice**. Columns and rows are fitted to the image, and clips are clamped into the new grid.
- **Clips**: the list shows frames, fps and mode, with `*` on the default. **+ Clip** starts at the last frame clicked; **Make default**, **Turn out** (delete), **Name** (Enter renames), **FPS** and **Mode** edit the selected clip.
- **Frames**: a thumbnail strip of every cell. Click a cell to start the selected clip there, then Shift+click the last frame.
- **Preview**: the selected clip running at its real pace, with **Hold** / **Ride** and **Restart**.

Stable edits save straight to the `.anim.json` (they are not undo steps and do not touch `scene.json`).

**Inspector -> Animator.** **Add Animator** (it picks the sheet's set when there is one), then the set, start clip, default clip, speed, playing, **Open in Stable** and **Remove**. Each change is one undo step and autosaves `scene.json`; a whole Speed drag is one step.

**Viewport.** In edit mode animators show their default clip, running when **View -> Preview Animations** is on (the default) and resting on frame 0 when it is off. Dragging a sheet that has a `.anim.json` from the Supply Wagon stakes an animated entity one frame in size ("Create Animated Sprite", one undo step).

**The sample rider.** New scenes (and **Reset Scene Placeholders**) write `assets/rider.png` and `assets/rider.anim.json` if they are missing, and P1 rides it. The cowboy is drawn pixel by pixel in code (`src/scene/SampleRider.cpp`) and saved as a PNG, so the project carries no third-party art. The sheet is 192 x 192: 32 x 48 frames, 6 columns (idle 0-1, walk 2-5) and 4 rows (down, up, left, right). There are eight clips, `idle_*` at 3 fps and `walk_*` at 8 fps, and the default is `idle_down`. Existing files are never overwritten.

#### `.anim.json` format

```json
{
  "version": 1,
  "image": "rider.png",
  "grid": {"frame_w": 32, "frame_h": 48, "cols": 6, "rows": 4},
  "default_clip": "idle_down",
  "clips": [
    {"name": "idle_down", "fps": 3, "mode": "loop", "start": 0, "count": 2},
    {"name": "walk_down", "fps": 8, "mode": "loop", "start": 2, "count": 4},
    {"name": "tip_hat", "fps": 6, "mode": "ping_pong", "frames": [{"x": 0, "y": 0, "w": 32, "h": 48}, {"x": 32, "y": 48, "w": 32, "h": 48}], "ms": [120, 240]}
  ]
}
```

- `image` is relative to the `.anim.json`'s folder.
- `grid` cuts the sheet into `cols x rows` cells of `frame_w x frame_h`, numbered row-major from the top-left (cell 7 of a 6-column grid is column 1 of row 1).
- A **grid clip** uses `start` and `count` cells. A **rect clip** lists explicit source rects in `frames`.
- `fps` is frames per second (0.1-120). The optional `ms` gives per-frame durations in milliseconds that override `fps`.
- `mode` is `loop`, `once` or `ping_pong`. `default_clip` is the clip animators fall back to (empty = the first clip).
- The writer is deterministic, with one clip per line. Unknown modes, duplicate clip names, a newer `version` or broken JSON are rejected with a reason, and the entity falls back to its sprite.

### Gameplay scripting (Lua)

Gameplay behaviour lives in small **Lua 5.4** files under `<project>/scripts/`. Any entity can carry a **script** component: the file plus its own property overrides. The same runtime (`src/runtime/Script*.cpp`, `World_Script.cpp`) runs them in the editor's Play mode and in `ts_game`, inside the World's fixed 60 Hz tick.

**Why Lua.** TombStone is built for four riders on an authoritative server, so a script has to be something the server can run, bound and replay. Embedded Lua fits that:

- **It is small and portable.** Lua 5.4.8 is about 30 C files with no dependencies. It is fetched by hash at configure time and compiled as C++, so script errors unwind through host destructors (no `longjmp`). The same code builds under MSVC, GCC and Clang.
- **It is sandboxed by construction.** Only the base, table, string, math, utf8 and coroutine libraries are compiled in. The io, os, package and debug libraries are never built, and `load`, `dofile`, `loadfile`, `require`, `collectgarbage` and `string.dump` are gone. Scripts reach the world only through the API below: no files, no sockets, no clock.
- **It is deterministic.** Hooks run on the tick, not the frame. `dt` is always 1/60 s, timers count ticks, and `math.random` is a seeded, replayable generator. The same scene with the same input gives the same ride, which is what a server needs to stay authoritative.
- **It is bounded.** Each hook call has an instruction budget of 2,000,000 (an endless loop is cut off and reported). All scripts together share a 64 MB memory cap. A world can hold at most 4096 timers, and an entity at most 256 state keys.
- **It is cheap to iterate on.** Save the file and the running ride picks it up (hot reload). Designers never rebuild the engine.

Native C++ plugins would give up the sandbox and hot reload, and a home-made visual or DSL language would cost far more than Lua's cost to embed.

**Attaching a script.** In the Inspector's **Script** section (under Gameplay), **Attach** a file from `scripts/`, switch **File**, **Open** it in your OS editor, or **Detach Script**. The props the file declares show up as typed fields: checkboxes for booleans, drag fields for numbers and text boxes for strings. An overridden prop shows in amber with a **default** button, and an override the file no longer declares is listed with **drop**. Attach, detach, file changes and every prop change are one undo step each and autosave `scene.json`; a whole drag or text edit is one step.

**The Scripts panel** (View -> Scripts, tabbed beside the Supply Wagon) lists `scripts/**.lua`. **Write a script** takes a name and a template and writes `scripts/<name>.lua`; it never overwrites a file. The templates are:

| Template | What it does |
|---|---|
| Blank | Every hook stubbed out |
| Gate | Opens on the action button (the sample gate) |
| Pickup | A rider through the trigger pockets it (the sample nugget) |
| Sign | Shows a toast to whoever presses action beside it |
| Ticker | Bobs up and down on a timer |

Double-click a file (or **Open in Editor**) to hand it to the OS default editor (ShellExecute on Windows, `open` on macOS, `xdg-open` elsewhere). The panel shows "Rides clean." with the declared props, or the load error with file:line. **Attach to <selected>** puts the file on the selected entity. The panel stays live during Play.

**Hot reload.** While riding, the editor checks `scripts/` twice a second (`ts_game` does the same). Each instance of a changed file re-runs its top level in place: props are re-applied, `self:get` state and timers are kept, and `on_reload()` is called if the file defines it. A save that does not compile keeps the old version running and puts the error on the Telegraph. An instance stopped by an error comes back once a fixed file is saved.

**Errors never take the ride down.** A runtime error, an exhausted instruction budget or the memory cap is logged once, with file:line, as an **error** on the Telegraph:

```
[tick 1] [error] scripts/boom.lua:3: attempt to index a nil value (local 'saddle') -- Player's script is stopped; the rest of the world rides on
```

Only that entity's instance is stopped. Everything else, including other entities running the same file, keeps riding.

**A script file:**

```lua
-- scripts/gate.lua: ride up close and press the action button.
props = {                       -- defaults; the Inspector overrides per entity
  open_line = "The gate creaks open.",
  close_after = 0,
  locked = false,
}

function on_start()
  self:set("open", false)
end

function on_interact(rider)
  if self:get("open", false) then return end
  if props.locked then
    rider:toast("Locked tight. Somebody's got the key.", 2)
    return
  end
  self:set("open", true)
  self:set_collider(false)      -- no more wall
  self:hide()
  log(self:name() .. " opened by P" .. rider:slot())
  rider:toast(props.open_line, 2)
  if props.close_after > 0 then
    after(props.close_after, function()
      self:set("open", false)
      self:set_collider(true)
      self:show()
    end)
  end
end
```

Each entity gets its own instance of the file, with its own globals, its own `self` and its own `props`, so two gates running `gate.lua` never share a variable.

**The sample content.** New scenes (and **Reset Scene Placeholders**) seed two scripted entities beside the rider, and write `scripts/gate.lua` and `scripts/gold.lua` if they are missing:

- **Corral Gate.** A solid post that swings open when P1 presses the action button beside it.
- **Gold Nugget.** A trigger. The first rider through it gets a "Picked up 10 gold (10 in the purse)" toast, a running `gold` tally in its state, and a log line, and the nugget leaves the world.

The **action button** is E or Space on the keyboard and A on a gamepad. A tap shorter than a tick is latched until the next tick, so it is never lost at high frame rates. `on_interact` goes to the nearest scripted entity with `on_interact` whose collider (or rect) is within 24 px of the rider's.

**Tick order.** Each tick, riders move and collide and triggers resolve. Then the script phase runs: `on_start` for newcomers, trigger exits and enters, `on_interact`, due timers, and finally `on_tick`. Spawned entities join at once and get `on_start` before their first `on_tick`; destroyed ones leave at the end of the script phase. Stop throws every bit of it away: hidden, moved, spawned and destroyed entities, state and timers are all gone, and the edit scene comes back untouched.

#### Script API

Hooks (define any of them):

| Hook | When |
|---|---|
| `on_start()` | The entity's first tick in the world |
| `on_tick(dt)` | Every tick, `dt` = 1/60 |
| `on_trigger_enter(other)` / `on_trigger_exit(other)` | A rider or dynamic body crosses this entity's trigger collider |
| `on_interact(rider)` | A rider pressed the action button within reach |
| `on_reload()` | The file was hot-reloaded mid-ride |

Globals: `self` (this entity), `props` (declared defaults merged with the Inspector's overrides) and the following functions:

| Function | Does |
|---|---|
| `log(...)` / `print(...)` | Info line on the Telegraph, tagged with file:line and the entity |
| `warn(...)` | Warning line (amber, counts on the badge) |
| `toast(text [, seconds])` | HUD card for every rider (default 2.5 s) |
| `after(seconds, fn)` / `every(seconds, fn)` | One-shot / repeating timer on the tick clock (at least one tick); returns an id |
| `cancel(id)` | Cancel one of this entity's timers; true when it was pending |
| `find(name)` | First entity with that name, or nil |
| `player([slot])` | The rider on slot 1-4 (default 1), or nil |
| `spawn(name, x, y)` | Copy of the authored entity `name` at x/y (script included); returns it |
| `duplicate(e [, x, y])` | Copy of a live entity, in place or at x/y |
| `tick()` / `time()` | World tick / seconds of ride time (tick / 60) |
| `math.random`, `math.randomseed` | Replayable generator, seeded the same for every ride |

Entity methods (`self`, `other`, `rider`, anything `find` returns):

| Method | Does |
|---|---|
| `e:id()`, `e:name()`, `e:alive()` | Identity; `alive()` is false once destroyed |
| `e:pos()`, `e:center()`, `e:size()` | x, y (top-left) / centre / w, h |
| `e:set_pos(x, y)`, `e:move_by(dx, dy)` | Teleport (no collision sweep) |
| `e:show()`, `e:hide()`, `e:visible()` | Draw it or not |
| `e:set_collider(on)`, `e:collider_on()` | Park / restore its collider (walls, triggers) |
| `e:play(clip [, hold])`, `e:release()` | Play an animation clip (`hold` keeps a rider off its auto walk / idle) / hand back |
| `e:get(key [, default])`, `e:set(key, value)` | Per-entity state: booleans, numbers, strings; `nil` clears |
| `e:destroy()` | Leaves the world at the end of the script phase |
| `e:is_player()`, `e:slot()` | Is it a rider? Its slot, 1-4 |
| `e:toast(text [, seconds])` | HUD card for that rider only (everyone when it is not a rider) |
| `e:spawned()` | Made by `spawn` / `duplicate` during this ride |

Methods on a destroyed entity do nothing and return nothing (`alive()` and `spawned()` say false); bad arguments raise an error at the calling line.

### The Telegraph (console)

The **Telegraph** (View -> Telegraph (Console), docked along the bottom of the Viewport) is the single log that the world, its scripts and the editor write to:

- **Script lines** come from `log` / `warn` and errors, with file:line.
- **Toasts** appear as `toast: P1: ...`.
- **Trigger events** appear as `trigger: Player rode into Gold Nugget`.
- **Editor notes** are every status-bar message ("Hot-reloaded 1 script mid-ride", "Undid: Move 3", autosave failures as warnings).

World lines carry their tick: `[tick 99] [info] scripts/gate.lua:32: Corral Gate opened by P1`.

- **Info / Warn / Error** toggle each level and show its count. **Search the wire...** filters case-insensitively. **Clear** empties the log. **Auto-scroll** follows new lines unless you have scrolled up.
- **Click a line** to pick out the entity it concerns: it is selected in the Hierarchy and Inspector and, when not riding, framed in the Viewport. A line from an entity that only existed during the ride says so.
- **The status-bar badge** reads `Telegraph`, `Telegraph: 2 warnings` or `Telegraph: 1 error` (amber / red) for lines you have not seen yet. Clicking it opens the panel.
- The log keeps the last 5000 lines and starts fresh for each project. The editor also echoes world lines to stdout as `[Telegraph] ...`.

`ts_game` prints the same lines to stdout as `[telegraph] [tick 8] [info] ...`. With `--log <file>` it also writes them to that file (overwritten on each run), and the final line notes how many script errors the ride had.

### ts_game (the standalone game)

```bash
./build/apps/game/ts_game --project ./build/apps/admin/TombStoneProjects/sample-2d-platformer
./build/apps/game/ts_game --smoke --project <dir>    # headless: load, ride 60 ticks, verify, exit
./build/apps/game/ts_game --project <dir> --log ride.log   # also keep the Telegraph in ride.log
```

`ts_game` opens its own GLFW window (1280x720, resizable), loads `<project>/scene.json`, its images and `.anim.json` sets through the runtime, and draws tilemaps and (animated) sprites with a small fixed-function OpenGL renderer (no ImGui, no editor code). Keys: WASD / arrows (and gamepads 1-4) ride, E / Space (pad A) action, P or F6 pause, F10 step while paused, R or F5 reload `scene.json` from disk, K collision overlay (same colours as the editor; `--collision` starts with it on), Esc quit. The title bar shows P1's position and the latest trigger event. Script toasts draw as leather cards at the bottom of the window, newest lowest and fading out, tagged `P2:` when more than one rider is in, using the public-domain [stb_easy_font](https://github.com/nothings/stb) (no font files are shipped). Scripts hot-reload twice a second when their files change. The editor's **Launch Game** finds `ts_game` next to `ts_admin` or in the build tree (`build/apps/game/`); if it isn't built it says so (configure with `-DTS_BUILD_GAME=ON`).

**Runtime architecture.** `src/scene` owns the data (Entity2D and its optional components, tilemaps, the scene.json reader/writer) and `src/runtime` owns the game: a `World` built from a copy of the entities, advanced by a `PlaySession` at a fixed 60 Hz (accumulator, at most 8 catch-up ticks per frame) from an abstract per-player `InputFrame` (move x/y + action buttons for slots 0-3), with a follow camera and a renderer-neutral draw list (world-space quads + UVs) interpolated between ticks. Neither library knows about ImGui or OpenGL: the editor feeds it keyboard/gamepad input and draws the list with ImGui, `ts_game` feeds GLFW input and draws it with OpenGL, and `--smoke` feeds scripted input. Collision, animation and scripts all run inside the World's tick. Networked players (a server plus up to four clients, see [Multiplayer](#multiplayer-the-posse)) are just more `InputFrame` sources: the server steps the same World.

### Multiplayer (the posse)

Four riders, one frontier. Multiplayer is server-authoritative: `ts_server` runs the real `World` (collision, animation, Lua scripts and all) at the fixed 60 Hz tick, and every `ts_client` sends its rider's input and draws what the server says, with prediction so its own horse never feels laggy.

```bash
# Host (UDP 24642 by default):
./build/apps/server/ts_server --project ./TombStoneProjects/sample-2d-platformer
# Riders (up to four), on this machine or another:
./build/apps/client/ts_client --connect 127.0.0.1:24642 --name Ike --project ./TombStoneProjects/sample-2d-platformer
./build/apps/client/ts_client --connect 192.168.1.20 --name Doc     # no --project: sprites draw as solid stand-ins
# Headless checks:
./build/apps/server/ts_server --smoke     # loopback posse of 2 (90 ms +-25, 10% loss) and 4 (40 ms +-10, 5% loss)
./build/apps/client/ts_client --smoke     # 2 riders at 70 ms +-20, 8% loss + the client's draw list
./build/apps/server/ts_server --smoke --riders 1 --lag 50 --jitter 20 --loss 10   # one custom run
```

`ts_server` flags: `--project <dir>`, `--port N` (0 = any free port), `--snapshot-every N` (ticks between snapshots, default 2 = 30 Hz), `--lag MS` / `--jitter MS` / `--loss PCT` (simulate a rough line on the server side), `--seconds S` (close after S seconds), `--smoke`. It logs joins, leaves and the Telegraph to stdout and a status line every 10 s; Ctrl+C closes it politely (riders hear "the server closed up shop").

`ts_client` flags: `--connect host[:port]` (also `[v6]:port`), `--name NAME`, `--project <dir>` (optional local copy for images; the scene itself always comes from the server, so a rider can't be out of date), `--lag` / `--jitter` / `--loss` on its own link, `--interp MS` (how far behind other riders are drawn, default 100), `--frames N`, `--smoke`. Keys as in `ts_game` (WASD / arrows, E / Space action, gamepad 1). The title bar shows your seat, the posse size, RTT, snapshot rate and bandwidth; script toasts show on your HUD (everyone's, plus your own).

**How it rides.**

- **Seats.** The first four riders get slots P1-P4 (the lowest free one). Each is seated on the scene's rider for that slot; when the scene has fewer riders than the posse, the server clones the first rider onto the slot's SpawnPoint (or beside P1). Empty seats ride hidden with their collider parked. A fifth rider is turned away ("The posse is full"); a protocol mismatch is refused with a reason.
- **Handshake.** Hello (protocol version, name, hash of the client's own `scene.json`) -> Welcome (slot, rider entity, tick, snapshot rate and the server's whole `scene.json`) or Reject. A differing local scene is noted in the server log and the server's wins.
- **Input.** Every client tick (60 Hz) sends its input unreliably, with the last 8 inputs repeated so loss costs nothing. The server keeps a small jitter buffer per rider and applies every input exactly once, in order; a backlog after a lag spike is ridden off at once (extra moves through the same movement code prediction uses) so it never turns into lasting delay. A tick with no input waits instead of guessing.
- **Snapshots** go out unreliably every `snapshot_every` ticks (30 Hz): server tick, the last input applied for *this* rider, a time echo for RTT, and one row per entity (position, visibility, collider on/off, rider heading, template name for script-spawned entities). Spawns and destroys made by scripts ride along: a client creates or removes them to match.
- **Events** go reliably: rode in / rode out (with the reason), script toasts (once each, the client times them), Telegraph lines from the world, and the roster with each rider's RTT once a second.
- **Prediction + reconciliation.** The client moves its own rider the moment you press, with `World::predict_rider` (exactly the movement + wall resolution `World::step` uses, no scripts). On every newer snapshot it snaps the rider to the server's position, replays the inputs the server has not applied yet, and smooths any difference away over a few ticks (more than 64 px jumps). With a single rider the prediction is exact: the smoke sees **zero** corrections at 50 ms +-20, 10% loss. Corrections come from things the client can't foresee: other riders shoving you (dynamic bodies), scripts moving you.
- **Interpolation.** Everything else is drawn `--interp` ms in the past, between the two snapshots around that moment, so remote riders glide even at 30 Hz with jitter and loss. Collider switches (an opened gate) apply from the newest snapshot right away, so you can ride through the instant the server opens it.
- **Determinism.** The server is the only place scripts run. Clients build a World from the server's scene only to draw and predict against it.

**Play with posse (editor).** The toolbar's **Posse** toggle (or *Play > Play with Posse*) starts a `GameServer` on a free loopback port and 1-4 `GameClient`s inside `ts_admin`, riding a copy of the scene you have open (unsaved edits included; your scene is never touched). You're P1 on the keyboard / pad; P2-P4 follow bot reins (a slow square, each out of step) unless you turn them off. The Viewport shows the ride through the chosen rider's eyes (prediction, interpolation and all), with each rider's server position as a faint box so you can *see* the lag. The **Network** panel (docked with the Stable; *View > Network*):

- **Posse:** Ride with Posse / **Break Camp**, rider count, **Saddle Another** / **Send One Home** while riding, **Open Client Window** (a real `ts_client` process riding into the same server).
- **Rough Trail:** latency (each way), jitter and loss sliders on every rider's line, live, with *Town* (none), *Prairie* (60 ms +-15, 2%) and *Mountain Pass* (150 ms +-40, 10%) presets.
- **Server:** snapshots per second per rider, snapshot size, KB/s out / in, joins / leaves / turned away.
- **Riders:** seat, name, RTT, snapshots/s, KB/s in / out, reconciliations ("fixes": hover for the last / worst correction and inputs in flight) and **Look** to ride through that rider's eyes.
- **Trail Map:** the whole scene in miniature, a dot where the server has each rider and a ring where the chosen client draws it.

Joins, leaves and every script line land on the Telegraph (channel `posse` for the server's own news).

**Smoke.** `run_posse_smoke` (`src/mp/PosseSmoke.cpp`, shared by `ts_server --smoke`, `ts_client --smoke` and `ts_admin --smoke`) writes a small four-seat project (a rider, four SpawnPoints, a wall, a bell script that toasts every second and rolls a spawned tumbleweed across town), starts a loopback server and 2-4 clients behind the link simulator, lets bots ride for ~3 s, then reins everybody in and requires every client to agree with the server to **0.01 px** (own rider predicted, the others interpolated) within a few seconds. Every client must have seen a toast and the spawned tumbleweed; with four riders a fifth is turned away; the last rider leaves and the seat must free, the rider vanish for everyone and the posse be told, and a newcomer must get that same seat. `ts_admin --smoke` also drives the editor's Play with posse headless.

**Ports and firewalls.** One UDP port (default 24642). Riders across a network need it open on the host. Windows links ENet against `ws2_32` and `winmm` (handled by `cmake/FetchENet.cmake`); nothing POSIX-only is used outside `#ifdef`s.

### scene.json format

`"version": 7` (current; v7 adds the optional entity keys `rotation` (degrees, clockwise), `parent` (entity id) and the editor-only `locked` / `hidden`, all written only when set; v1–v6 files still load and bad parent links are cleared). Every entity has id/name, x/y/w/h, r/g/b/a and layer, plus optional component objects. Version 2 added `tilemap` and `sprite`, version 3 added the gameplay components `player`, `camera` and `spawn`, version 4 added `collider` and the top-level `tile_solidity`, version 5 added `animator`, and version 6 adds `script`:

```json
"tilemap": {"cols": 8, "rows": 2, "tile_size": 32, "tileset": "", "encoding": "rle", "data": "8*4,8*1"},
"sprite": {"path": "assets/rider.png", "flip_x": false, "flip_y": false, "use_src": false, "src_x": 0, "src_y": 0, "src_w": 0, "src_h": 0},
"player": {"slot": 0, "speed": 160},
"camera": {"target": 2, "smoothing": 0.15, "zoom": 1, "use_bounds": false, "bounds_x": 0, "bounds_y": 0, "bounds_w": 1024, "bounds_h": 768},
"spawn": {"slot": 0},
"collider": {"x": 0, "y": 0, "w": 32, "h": 48, "type": "solid", "body": "dynamic"},
"animator": {"set": "assets/rider.anim.json", "clip": "", "default_clip": "", "speed": 1, "playing": true},
"script": {"path": "scripts/gate.lua", "props": [{"name": "locked", "bool": false}, {"name": "open_line", "text": "Swing wide."}]}
```

`script.path` is project-relative under `scripts/` and ends in `.lua`. `props` holds only this entity's overrides, sorted by name, each typed as `bool`, `number` or `text`. A prop left at the file's default is not written.

`collider` x/y are the offset from the entity's top-left corner, `type` is `solid` or `trigger`, and `body` is `static` or `dynamic`. The scene's `tile_solidity` lists the solid tile ids of each tileset that differs from the defaults (`""` = the built-in palette). A listed tileset uses exactly its list, and a toggle that brings a tileset back to its defaults drops its entry:

```json
"tile_solidity": [
  {"tileset": "", "solid": [3, 4, 5, 6, 8, 11, 12, 14, 15]},
  {"tileset": "assets/tiles.png", "solid": [7, 8, 9]}
]
```

Older files still load: a v1 entity named `TileMap*` becomes an empty 32 px grid covering its old rect, and a v1/v2 scene with no gameplay components gets a P1 controller on the entity named `Player` and a follow camera on `Camera2D` (targeting Player), so old projects play straight away. A v1-v3 scene gives every `player` entity a default dynamic collider (its whole rect) and starts with default tile solidity. A v4 rider without a collider stays a ghost. v1-v4 files load with no animators, and v1-v5 files with no scripts (nothing is added). The file is written as v6 on the next save.

`data` is row-major run-length text: comma-separated `count*id` runs (a single cell is just `id`).

`--smoke` also runs the runtime headless: scripted input moves the player exactly speed x time at 60 Hz (diagonals clamped), the camera follows and respects bounds, spawn points place riders, pause holds the world and step runs exactly one tick, Play -> Stop in the editor restores the workspace byte-for-byte with undo history and `scene.json` (bytes and mtime) untouched, edit tools are locked while playing, and a v2 project opens and plays.

Collision `--smoke`: a rider stops flush on a solid tile with zero speed, slides along a wall while pushing diagonally (keeping exactly the parallel motion), never tunnels at 5000 px/s through a tile or a 4 px fence, is blocked by a static crate and shoves a dynamic one, and respects `tile_solidity` overrides. Two riders dropped inside each other separate half each and never overlap while charging head-on. A trigger fires one enter and one exit on the expected ticks. The overlay boxes are checked, along with the v3 -> v4 upgrade and a byte-identical v4 roundtrip. The editor half toggles a palette tile (one undo step, `scene.json` in sync through undo / redo), removes and edits a collider with undo / redo, then plays: the rider stops on the newly solid tile, rides into a trigger exactly once, collision edits are locked, and Stop leaves solidity and `scene.json` alone. `ts_game --smoke` checks P1's collider shows in the overlay and reports the box and trigger counts.

Animation `--smoke`: clip timing is checked tick by tick at 60 Hz for loop, once (finishes on the right tick), ping-pong and per-frame `ms`. It also checks the `.anim.json` byte-identical roundtrip, its layout and rejects, the grid slice, library reload on change, and v4 -> v5 (a v4 scene loads untouched; animators roundtrip). The runtime half rides a 4-direction set and a right-only set. Each heading picks the right walk clip and idle keeps it. It checks diagonals, a stride kept through a turn, left drawn as mirrored right (with flipped UVs in the draw list), `play_clip` hold / release, pause, speed and the missing-set fallback. The editor half opens a fresh project: the rider sheet and set are seeded, the preview frames are checked, and animator edits are one undo step each with `scene.json` in sync through undo / redo / remove. Play walks right through at least three frame changes, then idles facing right, animator edits are locked, and Stop restores the workspace and `scene.json` byte for byte. It also checks animated and plain Supply Wagon drops, and Stable slice / add / rename / timing / range / default / delete landing in the `.anim.json` with no undo steps. `ts_game --smoke` rides P1 right and requires the drawn rider frame to change within `walk_right`.

Scripting `--smoke` has three parts:

- **Data.** The v6 `script` component roundtrips with typed props, a v5 scene loads untouched, and malformed script objects are refused.
- **Runtime.** It drives the Lua host headless. Hooks fire on the right ticks (start, tick, trigger enter / exit, the action-button interact), and the API is exercised: move, show / hide, collider, state, spawn / duplicate / destroy, clips and toasts. `after` / `every` / `cancel` timers fire on the right ticks. Errors are logged with file:line and stop only their instance: a nil index, an endless loop cut off by the budget, the 64 MB cap, a syntax error and a missing file. `io`, `os`, `load` and friends are absent. Hot reload works from memory and from disk, a broken save keeps the old script, a fixed save revives it, and `math.random` replays identically.
- **Editor.** It opens a fresh project with the seeded gate and nugget. Inspector prop overrides are one undo step each, with `scene.json` in sync through undo / redo. It writes a script from a template (refusing clobbers and bad names) and attaches it with undo. Then it rides:
  - a script that errors every tick logs `boom.lua:3` exactly once and only that instance stops;
  - the nugget is pocketed with trigger, toast and `gold.lua:15` lines on the Telegraph, and clicking the line selects the nugget;
  - an action tap on a frame too short for a tick still opens the gate on the next tick, with the Inspector's `open_line`;
  - a saved edit hot-reloads, and a broken save logs file:line and lights the badge;
  - Stop puts back everything the scripts hid, opened or destroyed, with the workspace and `scene.json` byte-identical.

  The Telegraph's level filters, search and clear are checked last.

`ts_game --smoke` rides the sample headless: it pockets the nugget, is stopped by the shut gate, presses action, and rides through. It prints the Telegraph lines it expects:

```
[telegraph] [tick 8] [info] trigger: Player rode into Gold Nugget
[telegraph] [tick 8] [info] toast: P1: Picked up 10 gold (10 in the purse)
[telegraph] [tick 8] [info] scripts/gold.lua:15: P1 picked up 10 gold at Gold Nugget
[telegraph] [tick 99] [info] scripts/gate.lua:32: Corral Gate opened by P1
[telegraph] [tick 99] [info] toast: P1: The gate creaks open.
```

The editor `--smoke` also paints a multi-cell stroke and checks it is one undo step, a 2x2 brush, erase, a cancelled stroke, bucket fill, rect fill/erase, eyedropper, resize (keeps tiles; undo restores cut cells), tile size, undo-all / redo-all with `scene.json` in sync, the v2 RLE roundtrip, the v1 upgrade, sprite drop-create / assign / flip / source-rect roundtrip with undo/redo, missing and corrupt image fallbacks, tileset slicing, and import.

### Building admin with ImGui

Admin pulls **GLFW 3.4**, **Dear ImGui (docking branch)** and the single **stb_image.h** header via CMake `FetchContent` (`cmake/FetchImGuiDeps.cmake`); `ts_game` uses the same GLFW and stb_image (no ImGui). `ts_game` also pulls the public-domain **stb_easy_font.h** for its HUD toasts. Every build that includes the runtime (`ts_admin`, `ts_game`) pulls **Lua 5.4.8** from lua.org, pinned by SHA-256 (`cmake/FetchLua.cmake`). It is compiled as C++ with only the core and pure libraries, so it needs nothing beyond the C++ toolchain on MSVC, GCC or Clang. Downloaded sources live under the build directory (`build/_deps/...`) and are gitignored — they are **not** committed.

Requirements (in addition to C++20):

- OpenGL development libraries  
- On Linux: X11 (or Wayland) development packages for GLFW (`libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `libgl1-mesa-dev`)  
- On Windows/MSVC: a working desktop OpenGL driver (Visual Studio 2019+ recommended)

```bash
cmake -S . -B build -DTS_BUILD_ADMIN=ON -DTS_BUILD_CLIENT=ON -DTS_BUILD_GAME=ON -DTS_BUILD_SERVER=ON
cmake --build build
./build/apps/admin/ts_admin          # GUI
./build/apps/admin/ts_admin --smoke  # headless flow smoke (no window, no GL)
./build/apps/game/ts_game --smoke --project <dir>
./build/apps/server/ts_server --smoke
./build/apps/client/ts_client --smoke
```

Only Admin fetches and links ImGui. `ts_client` uses GLFW, stb_image, stb_easy_font, Lua and ENet (no ImGui); `ts_server` only Lua and ENet (headless: no GLFW, no GL). `-DTS_BUILD_ADMIN=OFF` skips `src/editor` and ImGui.

### Projects & settings (on disk)

Defaults are **cwd-relative** (portable; no machine-absolute paths):

| Path | Purpose |
|------|---------|
| `./TombStoneConfig/settings.json` | User settings store |
| `./TombStoneProjects/` | Default `projects_root` (saves/projects folder) |

`settings.json` fields: `projects_root`, `username` (last-used), `auto_login_dev` (bool), `theme` (`dark`/`light` stub), `last_project_path`.

Each project is a folder under `projects_root` with a `project.json` (and, for 2D projects after first Editor2D open, a `scene.json`):

```json
{
  "name": "Sample 2D Platformer",
  "dimension": "2d",
  "vr": false,
  "created": "2026-09-30T00:00:00Z",
  "last_opened": "2026-09-30T00:00:00Z"
}
```

`scene.json` stores the Editor2D workspace (entity list + view). New projects come from templates (see **Claim templates** below); nothing is seeded. Changing `projects_root` in Settings reloads the project list from the new folder. Local `TombStoneProjects/` and `TombStoneConfig/` are gitignored.

Configure with CMake 3.20+ and a C++20 toolchain. Toggle `TS_BUILD_ADMIN`, `TS_BUILD_CLIENT`, and `TS_BUILD_GAME` as needed. See **Building admin with ImGui** above when enabling Admin.

### Claim templates (New Claim...)

**+ New Claim...** in the Project Manager opens the template picker. Each card shows a thumbnail, the dimension (2D / 3D), a VR badge, a description and the plugins it turns on. Filters are All / 2D / 3D / VR / Mine. The **Blank 2D** and **Blank 3D** claims are always there.

Templates are data, not code. Each one is `templates/<id>/template.json` plus a baked `project/` folder. Creating a claim copies the folder and renames it.

```json
{ "id": "haunted-saloon-vr", "name": "Haunted Saloon VR", "dimension": "3d", "vr": true,
  "description": "...", "plugins": [], "tags": ["sample"], "order": 30,
  "thumbnail": "thumbnail.png", "project": "project", "generator": "haunted-saloon-vr" }
```

- `generator` is optional. If the `project/` folder is missing, the built-in sample writer is used instead.
- `ts_admin --bake-templates <dir>` regenerates the baked folders.
- Templates are searched in this order: `$TS_TEMPLATES_PATH`, then `<exe>/templates` (the build copies them there), then the source tree.
- **Save as Template** writes the highlighted claim into `<projects_root>/.templates/`, shown under **Mine**.
- Admin no longer seeds sample projects into `projects_root`. Use the templates instead.

### VR is a switch on 3D

`project.json` now has `"dimension": "2d" | "3d"` plus `"vr": true|false`. Old projects with `"dimension": "vr"` load as 3D with VR on, and nothing is lost. You can flip VR in the create flow or in **Claim Settings**. Any 3D project with VR on gets the editor's VR tools. Export writes `"vr"` into `game.json`.

`ts_game` runs a VR project in the headset when one is found, or with `--sim` / `TS_VR=sim`. Otherwise it falls back to the desktop 3D window. Pass `--desktop` to force the desktop. When a scene has no Player, the VR rig stands in as the desktop rider.

### The Trading Post (plugin store)

Open it with **Trading Post** in the Project Manager, or with **Visit the Trading Post...** in the editor's Plugins panel. You can browse, search, filter by category, and see details, versions and dependencies. Install / uninstall works there, and so does turning a plugin on or off for the current claim. That toggle is the same list Outfitter's Wagon uses.

The store lists goods from four kinds of source:
- **Built-ins**: ts.posse, ts.tumbleweed, sample.lantern, plus the *optional* ts.frontier and ts.shipyard, which stay off until a project enables them.
- **Local folders**, which hold plugin libraries.
- **Remote indexes**, over `file://`, `http://`, or `https://` (Windows).
- **Story (optional)**: Story Core (`ts.story`) plus Bounty Board (`ts.quests`), Parley (`ts.dialogue`) and Strongbox (`ts.saves`); see below.
- **Assay Office (optional)**: `ts.profiler`, the profiler and debug tools; see below.
- **Coming soon**: Steam.

Installed plugins live in the engine plugin cache: `$TS_PLUGIN_CACHE`, which defaults to `./TombStonePlugins`. Each one is `<id>/installed.json` plus its library. Uninstall leaves a `.remove` marker when the library is still loaded. Sources are stored in `<cache>/trading_post.json`:

```json
{ "local_folders": ["D:/plugins"], "index_urls": ["https://example.com/ts/index.json"],
  "trusted_keys": ["<64 hex Ed25519 public key>"], "allow_unsigned": false }
```

Remote index (`index.json`, with `index.json.sig` beside it):

```json
{ "name": "My Exchange", "plugins": [ { "id": "acme.lanterns", "name": "Lanterns", "version": "0.3.0",
  "category": "Sample", "description": "...", "types": ["2d"],
  "deps": [ { "id": "ts.tumbleweed", "min": "1.0.0" } ],
  "downloads": { "windows-x64": { "url": "pkgs/lanterns.dll", "sha256": "<hex>", "size": 12345 },
                 "linux-x64":   { "url": "pkgs/liblanterns.so", "sha256": "<hex>", "size": 23456 } } } ] }
```

How an index and its downloads are checked:
- **Signature**: `index.json.sig` is a hex Ed25519 signature over the exact bytes of `index.json`. An index whose signature doesn't match one of `trusted_keys` is refused. `allow_unsigned` is for development only.
- **Signing**: run `ts_admin --store-sign <index.json> <64-hex seed>`. It writes the `.sig` file and prints the public key to trust. Ed25519 comes from Monocypher 4.0.2, fetched with a pinned hash.
- **Downloads**: each download must match its `sha256` and `size`. The library is loaded to confirm its plugin id before the install counts.
- **URLs**: relative URLs resolve against the index URL.

Smokes: `ts_admin --smoke-templates`, `ts_admin --smoke-store`. Both also run as part of `--smoke`.

### Quests, dialogue and saves (optional)

Story Core (`ts.story`) holds the shared story variables, condition and action language, and runtime. Three optional plugins sit on top of it, and enabling any one of them also enables the core:

- **Bounty Board** (`ts.quests`): `quests/*.quest.json` with objectives, prerequisites, chains and rewards; a journal and tracker HUD; the `quests` Lua table; `on_quest_*` and `on_objective` hooks.
- **Parley** (`ts.dialogue`): branching `dialogue/*.dlg.json` with conditions, actions, `{var}` text and once-only choices; a Dusk dialogue box; the `dialogue` Lua table; a node-graph editor panel.
- **Strongbox** (`ts.saves`): slots under `saves/slots/`, a rotating autosave (settings in `story.json`), a save menu and the `saves` Lua table.

The editor panels appear in the 2D and 3D View menus, including during Play. The Ghost Town 3D template turns all of this on: Old Hank offers "Ring the Bell" through dialogue. Smoke: `ts_admin --smoke-story`. Docs: `docs/quests.md`, `docs/dialogue.md`, `docs/saves.md`.

### Profiler and debug tools: Assay Office (optional)

Turn on `ts.profiler` for a project and Play records every frame: named CPU scopes per system, script language and plugin; GPU pass times from timer queries (3D); draw calls, triangles, asset counts and memory; and posse net stats. The 2D and 3D View menus get **Assay Office** (frame bars, timeline / flame view, scope table, capture and load), **Render & Memory** and **Net**. The Telegraph gets a console command line. In game (and in editor Play), **F3** shows the debug overlay and **`** opens the console (`help`, `fps`, `scopes`, `counters`, `gpu`, `mem`, `capture`). Captures go to `<project>/captures/*.tsprof.json` (Chrome trace JSON: open them in Perfetto too). Lua gets `profiler` and `console`. Smoke: `ts_admin --smoke-profiler`. Docs: `docs/profiler.md`.

### Persistent world: the Town Hall (ts_mmo)

`ts_server --mmo --project <dir>` runs a dedicated, persistent world on the posse's ENet transport. It has accounts with salted Argon2i password hashes (Monocypher); SQLite persistence for riders, inventory, positions and quest/story state, bridged to Strongbox save slots; zones with interest management (only nearby entities replicate); server-checked movement with corrections and cheat kicks; chat channels (say, zone, world, custom, whispers, admin); link-dead resume with rotating tokens; and an admin console on stdin. `ts_server --mmo-loadtest N` floods it with simulated bots. In the 2D editor (with `ts.posse` on), **View > Town Hall** runs the server in-process with a live map, riders, chat, console and bots, or launches `ts_server --mmo`. Smoke: `ts_admin --smoke-mmo`. Docs: `docs/mmo.md`.

### Terrain, foliage and weather (3D)

**Create > Terrain (hills)** in the 3D editor adds a heightmap terrain. **View > Terrain + Foliage** has brushes to sculpt (raise, lower, smooth, flatten, noise), to paint four splat texture layers, and to paint or erase instanced grass, trees and rocks. Plants sway in the wind and fade out with distance. Chunks switch LOD with distance, and Play adds a Jolt heightfield collider. Strokes undo, and data is saved to `terrain/*.tsterrain` (Scene3D v16). The optional **Weather** plugin (`ts.weather`) adds rain, snow, dust storms, fog and lightning storms in 2D and 3D, wind that moves foliage and particles, and a time-of-day link (`weather.json`, the Weather panel, Lua `weather`). Smokes: `ts_admin --smoke-terrain`, `--smoke-weather`. Docs: `docs/terrain-and-foliage.md`, `docs/weather.md`.

## Ship It: build & export (Freight Office)

Package a project into a standalone game folder, from the editor
(**File > Export... (Freight Office)** in 2D, **Claim > Export...** in 3D/VR)
or headless:

```
ts_admin --export <project dir | sample:<id>> --out <dir>
         [--profile shipping|dev] [--server] [--zip] [--runtime <ts_game>]
         [--name N] [--version V]
ts_admin --smoke-export     # exports all 5 samples, rides each from its pak
```

Samples: `sample-2d-platformer`, `ghost-town-3d`, `haunted-saloon-vr`,
`frontier-survival`, `sky-pirates`. Build Release first (VS2022:
`cmake --build build --config Release`); the exporter ships the `ts_game`
next to the running `ts_admin` and warns when it is a Debug build.

Output (`<out>/<slug>/`):

| File | What |
|---|---|
| `<slug>.exe` / `<slug>` | `ts_game`, renamed; on Windows the project's `icon.png` is stamped in as the exe icon |
| `game.json` | manifest: name, version, icon, window size, start scene, dimension, profile, pak |
| `game.tspak` | cooked content: scenes, scripts, assets, ui, project.json (editor-only data stripped) |
| `plugins/` | dynamic plugin DLLs the project enables (built-ins need none) |
| `lib/` | Linux: OpenXR loader `.so` (static on MSVC); `<slug>.sh` sets `LD_LIBRARY_PATH` |
| `server/` | `--server`: `ts_server` + loose content + `run_server` script |
| `README.txt`, `export_report.txt` | how to run (MSVC runtime note: VC++ 2015-2022 x64 redistributable) and the validation report |

`ts_game` with no `--project` (or `--project <export dir>`) reads `game.json`
next to it, mounts `game.tspak` through **ts_vfs** and runs. Profiles:
**shipping** (script errors / missing scene / missing plugin stop the export;
editor tool state stripped from project.json + 2D scene) and **dev**
(warnings only, keeps `.pdb`, launcher writes `telegraph.log`). `--zip` writes
`<out>/<slug>-<version>.zip`.

Validation report: scene present, every `.lua` compiles, asset paths named in
the JSON data exist, enabled plugins resolve (built-in or DLL).

**.tspak** (`src/vfs/Pak.h`): header `TSPAK\0\0\1`, version, count, index
offset, content hash; file blobs; index of (path, offset, size, stored size,
flags, FNV-1a 64 hash). Reads are hash-checked. `ts_vfs::Vfs` mounts paks
over loose roots (pak first, loose fallback). The runtime mounts a pak by
laying it out once in a per-content-hash cache (`$TS_PAK_CACHE`, else
`%LOCALAPPDATA%\TombStone\paks` / `~/.cache/tombstone/paks`), so every
loader (scene, scripts, UI, audio, textures, glTF, plugin data) reads packed
content without per-loader hooks. Entries are stored uncompressed (the
flags field reserves compression).

## Scripting languages (Lua, graphs, C++, C#)

Every script component names a file under `scripts/`. Its extension picks the
language, and one project can mix all four. They share the same hooks
(`start`, `tick`, `trigger enter/exit`, `interact`, `reload`, `destroy`, the
VR hand events and UI events) and the same world API: find / spawn / move /
show / collider / play, entity state (shared by all languages and
replicated), toast, log, sound / music, particle bursts, UI and buttons.
Each phase runs the languages in a fixed order (lua, graph, cpp, cs), so a
mixed project stays deterministic.

| Extension | Language | Host | Hot reload |
|---|---|---|---|
| `.lua` | Lua 5.4 (sandboxed) | `LuaScriptHost` | per file, state kept |
| `.graph.json` | visual graph | `GraphScriptHost` (interpreter) | variables kept |
| `.cpp` | C++20 | `NativeScriptHost`: one module per project | `save()` / `load()` |
| `.cs` | C# (.NET 8) | `CSharpScriptHost`: hostfxr + AssemblyLoadContext | `Save()` / `Load()` |

**Visual graphs and the Brand Book.** Graphs are `*.graph.json` assets with
64 node types:

- events: start, tick, trigger, interact, key, UI, hand
- flow: branch, sequence, delay, for, do-once
- math / compare / logic, values, variables
- entities and state
- audio, particles, UI, toast, log
- **Plugin Call**

The interpreter runs on the fixed tick and is deterministic, so a server can
run graphs too. Data pins are pulled, delays count ticks, and each hook call
has a budget of 20000 steps and nesting depth 64.

Edit graphs in the **Brand Book** node editor: **File > Brand Book...** in
2D, **Claim > Brand Book...** in 3D/VR, the Scripts panel, or double-click a
`.graph.json`. It is a frontier-themed canvas:

- node cards coloured by family, heavy exec wires, and data wires tinted by
  value type
- a right-click palette with search
- a Ledger panel for each node's constants
- zoom, pan and fit-to-view
- Ctrl+S saves, and a running ride hot-reloads the graph

The Brand Book can also run on its own:
`ts_admin --brand-book <project> [scripts/x.graph.json]`.

**C++ scripts.** Write scripts against the header-only SDK
`sdk/include/tombstone/script_sdk.h`, which wraps the C ABI in
`script_abi.h`:

```cpp
#include <tombstone/script_sdk.h>
struct Door : ts::Script {
  int opened = 0;
  void on_interact(ts::Entity) override { toast("The door creaks"); set("open", ++opened); }
  std::string save() const override { return std::to_string(opened); }   // kept on hot reload
  void load(const std::string& s) override { opened = std::atoi(s.c_str()); }
};
TS_SCRIPT(Door, "door")   // scripts/door.cpp
```

- All of a project's `scripts/*.cpp` build into one module. The engine
  generates a CMake project in `<project>/.tombstone/native`, then builds it
  with CMake and the host compiler.
- Builds run on a worker thread and the game keeps ticking. The new module
  is swapped in when it is ready.
- Compile errors appear in the Telegraph with file:line, and the old module
  keeps running.
- The Brand Book has a **Build C++ scripts** button.
- Exports ship `native/ts_scripts.dll` (or `.so`) inside the pak, and never
  the sources.

**C# scripts.** Scripts derive from `TombStone.Script`, from the binding in
`sdk/dotnet/TombStone.Scripting`:

```csharp
using TombStone;
[Script("teller")]                  // or name the class Teller
public class Teller : Script {
    int visits;
    public override void OnInteract(ulong rider) { visits++; Toast("Howdy, visit " + visits); }
    public override string Save() => visits.ToString();
    public override void Load(string s) => int.TryParse(s, out visits);
}
```

- The engine builds `scripts/*.cs` with `dotnet build` into
  `ProjectScripts.dll`. That needs the **.NET 8 SDK**.
- .NET is hosted in the engine process through `hostfxr`. The runtime is
  found when the game starts, so the engine does not link nethost.
- Each build loads into its own collectible `AssemblyLoadContext`. Hot reload
  saves state, loads the new context, restores state, and unloads the old
  context.
- Without the SDK, C# scripts are disabled cleanly. The Telegraph and the
  editor (the Brand Book's Languages list and the Scripts panel) say
  *"install the .NET 8 SDK (https://dotnet.microsoft.com/download/dotnet/8.0)
  to enable C#"*, and every other language keeps running.
- Exports ship `dotnet/ProjectScripts.dll`, `TombStone.Scripting.dll` and
  its runtimeconfig. The player only needs a .NET 8+ runtime.

**Plugin call bridge.** Plugins expose Lua tables (`WorldExtensions::lua`).
Graphs reach them with the Plugin Call node, C++ with
`plugin("table", "fn", {ts::arg(3.0)})`, and C# with
`Plugin("table", "fn", 3.0)`. All three go through
`ScriptWorld::script_plugin_call`, which keeps one sandboxed Lua state per
world (with an instruction budget) and returns a single value.

**Smokes** (all are part of `--smoke`):

| Command | Checks |
|---|---|
| `ts_admin --smoke-graph` | interpreter, determinism, budget, reload, plugin bridge, Brand Book starter graph |
| `ts_admin --smoke-cpp` | build, hooks / props / state, bridge, hot reload, compile error file:line, background swap, exported game |
| `ts_admin --smoke-cs` | the same for C#, plus AssemblyLoadContext unload and a mixed Lua + C# + C++ world |

Without a toolchain, `--smoke-cs` checks that C# is disabled cleanly and
reports SKIPPED. `TS_SMOKE_REQUIRE_CPP=1` / `TS_SMOKE_REQUIRE_CS=1` turn a
skip into a failure. `TS_SMOKE_CS_PREBUILT=<dir>` runs a prebuilt assembly
on a runtime-only machine.
