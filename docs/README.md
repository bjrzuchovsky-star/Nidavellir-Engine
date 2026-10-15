# TombStone Engine documentation

These pages are written from the source in this repository (`src/`, `apps/`, `sdk/`, `templates/`, `cmake/`). Where a page cites a file, that file is the place to check the details.

| Page | Covers |
|---|---|
| [Getting started (Windows, VS2022, CMake)](getting-started.md) | Requirements, configure and build, the executables, first run, smoke tests |
| [Editor guide](editor-guide.md) | Admin flow, the 2D and 3D editors, tools, TileMaps, the Stable, Play mode, the Telegraph |
| [Project types and templates](projects-and-templates.md) | 2D / 3D / VR, `project.json`, claim templates, plugins per project |
| [The Trading Post](trading-post.md) | Plugin store, sources, the plugin cache, signed indexes, the plugin system |
| [Scripting](scripting.md) | Lua, visual graphs (Brand Book), C++ scripts with hot reload, C# |
| [Audio](audio.md) | `ts_audio`, buses, Audio Source, Lua `audio.*`, the Saloon Band mixer |
| [UI and fonts](ui-and-fonts.md) | `ts_ui`, fonts, widgets, `.ui.json`, Lua `ui.*` |
| [Particles and lighting](particles-and-lighting.md) | `ts_fx` emitters and presets, 2D lighting, 3D lights |
| [Export and the Freight Office](export.md) | Export UI and CLI, profiles, `.tspak`, the output folder |
| [3D, VR and animation](3d-vr-animation.md) | `ts_render3d`, the 3D editor, VR / OpenXR, 2D sprite and 3D skeletal animation |
| [Multiplayer (the posse)](multiplayer.md) | `ts_server`, `ts_client`, protocol, prediction, Play with Posse |

## Coming soon

- **Physics (Jolt / Box2D):** in progress, not documented yet. Right now `src/physics/Physics2D.h` is only a stub class (`init`, `shutdown`, `step`), and the repository contains no Jolt or Box2D code. 2D collision currently lives in `src/runtime/World_Collision.cpp` (see the editor guide). 3D collision is a capsule against static box / capsule colliders in `World3D`.
- **AI (`ts.ai`):** in progress. Its only appearance in the source is a "Coming soon" entry in the Trading Post (`Pathfinder (AI Toolkit)`, `src/store/TradingPost.cpp`).

## Note on the top-level README

The "Build Requirements" section of the root `README.md` mentions "Vulkan/DirectX/OpenGL". The source only has OpenGL renderers: a fixed-function GL path for 2D `ts_game` and an OpenGL 3.3 core renderer in `src/render3d`. It has no Vulkan or DirectX backend.
