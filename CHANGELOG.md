# Changelog

All notable changes to TombStone Engine. The version number lives in the `VERSION` file at the repo root.

## Unreleased

### Editor
- "New update available" notice on the Project Manager. On open (at most every 6 hours, cached in settings), a background check asks GitHub for the latest release and compares it with the built-in `VERSION`. The notice has **Update** (downloads `TombStone-<ver>-update.exe`, falling back to `-setup.exe`, checks it against `SHA256SUMS.txt`, runs it and closes the editor), **Release notes**, **Later** and **Skip this version**. There's a Settings toggle, **Check for updates on start** (on by default). Errors and offline stay quiet (log only). The check uses the Vault's HTTP client (WinHTTP, Windows). `TS_FAKE_UPDATE=9.9.9` forces the notice for testing. See [docs/updates.md](docs/updates.md).
- `ts_admin --smoke-update-check` (also in `--smoke`): canned release JSON and a loopback server, no internet.

### Player movement
- Mouse look for the built-in 3D Player, on by default. First person: the mouse turns the body and tilts the eyes (pitch clamped to ±89°), and WASD walks where you look. Third person: the mouse orbits the camera (clamped pitch) and the scroll wheel zooms. The rider walks relative to the camera and turns to face where it walks, and the camera is still pulled in front of static colliders.
- Play captures the cursor (raw mouse motion where supported). In the editor, Esc, focus loss, Pause, Free cam or an in-game menu frees it, a click in the viewport takes it back, and a second Esc stops Play. In `ts_game`, the pause menu (Esc) and focus loss free it, and resuming or a click captures it again.
- New Player inspector settings: **Look** (Mouse / Keys), **Sensitivity**, **Invert Y**, **Smoothing** (off by default) and **Capture cursor on Play**. Keys look keeps the old Q / Z, Page Up / Page Down and right-drag. Q / Z and a gamepad's right stick turn in both modes, and `ts_game` 3D also walks with pad 1's left stick and jumps with A.
- 2D top-down **Face mouse**: the sprite turns toward the mouse cursor or the right stick, and the collider doesn't turn. It's on in the Empty 2D Top-down template. Platformer riders are unchanged.
- The Empty 3D First-person, 3D Third-person and 2D Top-down templates and the Ghost Town 3D sample use mouse look. VR is unchanged (headset look).
- `scene.json` fields are all optional, and only non-default values are written, so there's no version bump: Player `look`, `mouse_sensitivity`, `invert_y`, `look_smoothing`, `capture_cursor`, and 2D `face_mouse`.
- `ts_admin --smoke-mouselook` (also in `--smoke-templates`) drives synthetic mouse deltas through yaw, pitch, the pitch clamp, invert Y, smoothing, the right stick, the third-person orbit, zoom and wall pull-in, and the top-down facing angle.

### Model import
- Fixed: the selection box ("hit box") and auto colliders of imported character models were far bigger than the model. Bounds came from the raw bind-space vertices of every mesh, while skinned meshes are drawn through the skeleton. FBX-style rigs in centimetres with a 0.01 armature were boxed 100x too big, and on Source models with more than 128 bones, vertices on the extra bones collapsed while the box stayed full size. Bounds now cover what is drawn: the vertices triangles use, in the rest pose, after the unit scale and up axis. Bones, empties and helpers don't count.
- Fixed: FBX files from Blender with meshes parented to bones (props, armour plates, eyes and jaws on a character rig) came in exploded, parts metres from the body, and the box grew to cover them. Blender writes those meshes relative to the armature plus the bone's length, not relative to the bone. When the file says Blender made it and that reading puts the parts on the skeleton, the importer now uses it (an import note says how many meshes it moved).
- Collision meshes (`UCX_` / `UBX_` / `UCP_` / `USP_` / `UMB_`, `-colonly`, `_collision`) become convex collision hulls instead of being drawn. Meshes tagged `_LOD<n>` keep only the chosen LOD.
- Skeletons with more than 128 bones get a palette per primitive (split as needed), so every vertex is skinned.
- New *Collider* import setting: Auto (model hulls, else a capsule for skinned models, else a tight box) / Tight box / Capsule / Convex hull / Mesh / None. The inspector has a fit mode and **Fit to mesh**. Fitted colliders (`"fit"` in the scene JSON) follow model reimports. Old scenes' box colliders that were sized from the old bounds are refit automatically when the scene opens (one undo step).
- Source `.mdl` hitbox sets (`$hbox`) are imported as per-bone hitboxes (`Model::hitboxes`, `hitbox_matrices`), and **View > Hitboxes** shows them in the 3D editor.
- Model `.meta` info carries `importer_version` (now 2). Older info is reimported on the next scan.
- Physics hulls and triangle colliders of skinned models use the rest pose too.
- `ts_admin --smoke-bounds` (also in `--smoke-models` / `--smoke`) and Source hitbox checks in `--smoke-mdl`.

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
