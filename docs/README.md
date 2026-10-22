# TombStone Engine documentation

These pages are written from the source in this repository (`src/`, `apps/`, `sdk/`, `templates/`, `cmake/`). Where a page cites a file, that file is the place to check the details.

| Page | Covers |
|---|---|
| [Getting started (Windows, VS2022, CMake)](getting-started.md) | Requirements, configure and build, the executables, first run, smoke tests |
| [Editor guide](editor-guide.md) | Admin flow, the 2D and 3D editors, tools, TileMaps, the Stable, Play mode, the Telegraph |
| [Prefabs](prefabs.md) | `.prefab.json` assets in the 2D and 3D editors: instances, overrides, apply / revert, undo, `--smoke-prefab` |
| [Assets](assets.md) | Asset browser, importers, `.meta` GUIDs, move-safe references, hot reload, `--smoke-assets` |
| [Model import](model-import.md) | glTF, Source `.mdl` (GMod), OBJ, FBX, DAE, PLY, STL, X and more through Assimp, `.blend` via Blender; import settings, VMT / VTF, limits, `--smoke-models` / `--smoke-mdl` |
| [The Vault](vault.md) | The asset library shared by every claim: local folder or self-hosted `ts_vault` server, Send to Vault / Pull, GUID-stable pulls, versions and pins, API, CLI, hosting, `--smoke-vault` |
| [Project types and templates](projects-and-templates.md) | 2D / 3D / VR, `project.json`, claim templates, plugins per project |
| [The Trading Post](trading-post.md) | Plugin store, sources, the plugin cache, signed indexes, the plugin system |
| [Scripting](scripting.md) | Lua, visual graphs (Brand Book), C++ scripts with hot reload, C# |
| [Audio](audio.md) | `ts_audio`, buses, Audio Source, Lua `audio.*`, the Saloon Band mixer |
| [UI and fonts](ui-and-fonts.md) | `ts_ui`, fonts, widgets, `.ui.json`, Lua `ui.*` |
| [Localisation](localisation.md) | String tables (JSON / CSV), `tr()` in Lua / graphs / C++ / C#, UI `text_key`, font fallback, the Strings panel, the language picker |
| [Autotiles and the sprite slicer](autotiles-and-sprite-slicer.md) | 47-blob, Wang 16 and rule-tile terrain brushes for TileMaps; slicing sprite sheets into `.anim.json` clips |
| [Terrain and foliage](terrain-and-foliage.md) | 3D heightmap terrains: sculpt / paint / foliage brushes, splat layers, LOD chunks, Jolt heightfield, instanced foliage with wind, Lua `terrain` |
| [Weather](weather.md) | Optional `ts.weather`: rain, snow, dust, fog, storms with lightning, wind, time of day, 2D and 3D, `weather.json`, Lua `weather` |
| [Particles and lighting](particles-and-lighting.md) | `ts_fx` emitters and presets, 2D lighting, 3D lights |
| [Export and the Freight Office](export.md) | Export UI and CLI, profiles, `.tspak`, the output folder |
| [Releasing the engine](release.md) | `VERSION`, `tools/release/build_release.ps1`, the engine installer and zip, silent install test |
| [Distributing a game](distribution.md) | Windows setup installer (Inno Setup), portable zip, app-local VC++ runtime, dependency check, GPU check |
| [3D, VR and animation](3d-vr-animation.md) | `ts_render3d`, the 3D editor, VR / OpenXR, 2D sprite and 3D skeletal animation |
| AI toolkit (`ts.ai`) | See "Pathfinder AI toolkit (ts.ai)" in the root `README.md`: behaviour trees, 2D grid / 3D navmesh paths, crowds, perception, the BT panel |
| [Quests: Bounty Board](quests.md) | Optional `ts.quests`: quest files, objectives, journal / tracker, Lua `quests`, hooks |
| [Dialogue: Parley](dialogue.md) | Optional `ts.dialogue`: branching `.dlg.json`, conditions / actions, Lua `dialogue` / `story`, the Parley panel |
| [Saves: Strongbox](saves.md) | Optional `ts.saves`: slots, autosave, `story.json`, Lua `saves` |
| [Profiler and debug tools: Assay Office](profiler.md) | Optional `ts.profiler`: CPU scopes, timeline, GPU timers, draws / memory / assets, net stats, F3 overlay, console, captures |
| [Persistent world: Town Hall](mmo.md) | `ts_server --mmo`: accounts (Argon2i), SQLite riders + Strongbox bridge, zones + interest, anti-cheat movement, chat, resume, admin console, bot load test, editor panel |
| [Multiplayer (the posse)](multiplayer.md) | `ts_server`, `ts_client`, protocol, prediction, Play with Posse |

## Coming soon

- **Physics (Jolt / Box2D):** in progress, not documented yet. Right now `src/physics/Physics2D.h` is only a stub class (`init`, `shutdown`, `step`), and the repository contains no Jolt or Box2D code. 2D collision currently lives in `src/runtime/World_Collision.cpp` (see the editor guide). 3D collision is a capsule against static box / capsule colliders in `World3D`.

## Note on the top-level README

The "Build Requirements" section of the root `README.md` mentions "Vulkan/DirectX/OpenGL". The source only has OpenGL renderers: a fixed-function GL path for 2D `ts_game` and an OpenGL 3.3 core renderer in `src/render3d`. It has no Vulkan or DirectX backend.
