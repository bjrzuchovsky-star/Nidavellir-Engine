# Changelog

All notable changes to TombStone Engine. The version number lives in the `VERSION` file at the repo root.

## Unreleased

### Editor
- "New update available" notice on the Project Manager. On open (at most every 6 hours, cached in settings), a background check asks GitHub for the latest release and compares it with the built-in `VERSION`. The notice has **Update** (downloads `TombStone-<ver>-update.exe`, falling back to `-setup.exe`, checks it against `SHA256SUMS.txt`, runs it and closes the editor), **Release notes**, **Later** and **Skip this version**. There's a Settings toggle, **Check for updates on start** (on by default). Errors and offline stay quiet (log only). The check uses the Vault's HTTP client (WinHTTP, Windows). `TS_FAKE_UPDATE=9.9.9` forces the notice for testing. See [docs/updates.md](docs/updates.md).
- `ts_admin --smoke-update-check` (also in `--smoke`): canned release JSON and a loopback server, no internet.

### Release tooling
- `build_release.ps1` also writes `TombStone-<ver>-update.exe`: same AppId, upgrades in place with a minimal wizard, and keeps claims, settings and the Vault. It's listed in `SHA256SUMS.txt`, and `-TestInstall` runs it over the test install.

## 0.1.0 (unreleased)

The first release: a C++20 engine and editor for 2D, 3D and VR games with multiplayer, scripting in four languages, and a Windows installer for both the engine and exported games.

### Editor (`ts_admin`)
- Loading, login, project manager and settings screens in a frontier theme (Dear ImGui, docking).
- 2D editor: hierarchy, inspector and viewport, tile tools with a Tile Palette, the Supply Wagon asset shelf, multi-select, marquee, snap to grid, nudge, duplicate and delete, parenting, object snap, rotation handles, clipboard, context menus and hotkeys. Every edit can be undone and redone.
- 3D editor: translate, rotate and scale gizmos with snap, hierarchy, inspector, assets and environment panels (sky, sun, shadows, ambient), parenting, align, drop to ground, picking, glTF drop, Play / Stop.
- Asset Browser in both editors: `.meta` GUIDs, references that survive moves, Find references, safe delete, per-asset import settings and hot reload.
- Prefabs (`.prefab.json`) in 2D and 3D: instances, overrides, apply and revert, with undo.
- Telegraph console (levels, search, click a line to select its entity) and Play / Pause / Step.
- Plugins panel (Outfitter's Wagon) and the Trading Post plugin store: built-in, local and signed remote indexes, and a plugin cache.
- Project templates: Blank 2D / 3D, Dusty Town, Frontier Survival, Sky Pirates, Ghost Town 3D and Haunted Saloon VR.

### Runtime and rendering
- `ts_game`: the standalone runtime for 2D, 3D and VR projects. It reads the `game.json` export manifest and mounts `game.tspak`.
- 2D: tile maps, sprites, colliders with tile solidity, triggers, `.anim.json` sprite animation, and 2D lighting (point and spot lights, day-night ambient, soft tile shadows).
- 3D: OpenGL 3.3 core forward renderer with PBR-lite materials, a sun shadow map with PCF, point lights, a gradient sky, fog, billboards and an offscreen readback.
- Skeletal animation: glTF skins and clips, GPU skinning (128 bones), the Animator (parameters, layers and masks, blend spaces, transitions, events, root motion) and the Animation Preview panel.
- Model import: glTF / GLB (cgltf); Source engine `.mdl` (clean-room reader with VMT / VTF and `.phy` hulls); OBJ, FBX, DAE, 3DS, PLY, STL, X and more through Assimp 5.4.3; `.blend` through a local Blender.
- Terrain and foliage: sculpt brushes, four splat layers, LOD chunks, a Jolt heightfield collider, and instanced foliage with wind.
- Particles (`ts_fx`): 10 presets, deterministic simulation, in 2D and 3D, replicated over the network.
- In-game UI (`ts_ui`): embedded Rye and Arvo fonts (SIL OFL), widgets, `.ui.json`, a pause menu and toasts, and a world-space panel in VR.
- Localisation: string tables (JSON / CSV), `tr()` in every script language, UI `text_key`, a language picker.
- Audio (`ts_audio`, miniaudio): buses, 3D spatial sound, music decks with crossfade, the Saloon Band mixer, Audio Source components, sound replicated to clients.

### VR
- OpenXR (SteamVR and others) through the OpenXR loader, plus a desktop VR simulator.
- Smooth, snap and teleport locomotion, laser pointer, grab, force grab and throw, a comfort menu with vignette, haptics, and the Lua `vr` table.

### Physics
- Jolt 3D physics for World3D and VR (physical grabs, a physics gun), Box2D 2D rigid bodies and joints.
- Physics inspectors, gizmos and settings in the editor; a scripting API in Lua, graphs, C++ and C#.

### Scripting
- Lua 5.4 (sandboxed, hot reload, contained errors), visual script graphs edited in the Brand Book (84 node types).
- C++ gameplay scripts (header-only SDK, built in the background and hot-reloaded) and C# scripts (.NET 8 hosted in process).
- A plugin call bridge from every language.

### Multiplayer
- `ts_server` (authoritative, headless) and `ts_client` over ENet: client prediction and reconciliation, snapshot interpolation, a link simulator, and Play with Posse in the editor.
- Town Hall persistent world (`ts_server --mmo`): accounts (Argon2i), SQLite, zones with interest management, server-checked movement, chat, an admin console and a bot load test.

### Gameplay plugins
- `ts.ai` Pathfinder: behaviour trees, grid and navmesh paths (Recast / Detour), crowds and perception, with a Behaviour Trees panel.
- Story: Bounty Board quests, Parley dialogue, Strongbox saves.
- Frontier Survival (`ts.frontier`), Airship Works (`ts.shipyard`), weather (`ts.weather`) and the Assay Office profiler (`ts.profiler`).

### Shipping
- The Freight Office: validates a project, cooks it into `.tspak`, and exports the runtime with plugin DLLs, a portable zip and an Inno Setup installer. The export uses an app-local C++ runtime, checks DLL dependencies, and runs a GPU check at install time.
- Engine release tooling (new in this version): the `VERSION` file, `tools/release/build_release.ps1` and `tools/release/tombstone.iss` build `TombStone-<version>-setup.exe` and `TombStone-<version>-win64.zip`. The release contains ts_admin, ts_game, ts_server, ts_client, plugins, templates, samples, fonts, docs, the script SDK and third-party licences.
- `ts_admin --version`. An installed engine also finds its script SDK and plugins next to the executables.

### Fixes in this release
- C++ script compile errors map to the right `file:line` when the compiler and the project spell the folder differently: 8.3 short names, letter case, `..`. This was the cause of the Windows CI failure.
- A leaving multiplayer client is acknowledged at once, even when the server runs in the same process, because the in-process server keeps being serviced while the client waits. The posse smoke waits on the condition rather than on fixed timing.

### Tests
- `ts_admin --smoke` runs the full headless suite; there are single-area flags (`--smoke-3d`, `--smoke-models`, `--smoke-installer`, ...). `ts_server --smoke` and `ts_client --smoke` cover multiplayer. CI runs everything on windows-latest (MSVC) and ubuntu-latest (GCC).
