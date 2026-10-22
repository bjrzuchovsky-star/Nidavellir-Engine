# Changelog

All notable changes to TombStone Engine. The version number lives in the `VERSION` file at the repo root.

## 0.1.1 (2026-10-23)

A trail update for 0.1.0: The Vault asset library, empty claim templates, an update notice with an in-place update installer, mouse look, much tighter model bounds and hitboxes, better Blender detection, and asset metadata tucked away in a hidden folder. `TombStone-0.1.1-update.exe` upgrades a 0.1.0 install in place and keeps your claims, settings and the Vault.

### The Vault
- The Vault, a universal asset library that sits outside your claims: send textures, models, sounds, fonts and prefabs from any claim and pull them into any other. A pulled asset keeps its `.meta` GUID, so references to it hold in every claim, and pulls bring their dependencies (a model's textures, a prefab's models). See [docs/vault.md](docs/vault.md).
- Two kinds of library behind one interface: a **local library** shared by every claim on the PC (default `Documents\TombStone\Vault`, override with `TS_VAULT_DIR`) and **`ts_vault`**, a small self-hosted HTTP server with a JSON API and hashed API keys (`--add-key`, `--list-keys`, `--revoke-key`). Both store SHA-256 content-addressed blobs with a SQLite index.
- **View > Vault** panel in the 2D and 3D editors: search, kind / tag / 2D / 3D / VR filters, thumbnail tiles with versions and **UPDATE** / **PINNED** badges, details with versions and dependencies, **Pull**, **Update all**, **Pin**, and settings (API key in a password field, **Test connection**; the key is never logged).
- **Send to Vault...** from the Asset Browser (right-click or the details pane) with name, author, licence presets, tags, 2D / 3D / VR, description, version note and thumbnail. Changed files make a new version (v2, v3, ...). Vault tiles can be dragged into the Asset Browser or straight into the scene.
- Pulled assets show a **V** badge and an origin line (**UPD** when a newer version is waiting). Pulls never overwrite another asset's file or a pulled file you changed locally. Exports leave out pulled assets the game doesn't use.
- `ts_admin --vault push|pull|list|status|update` and `ts_admin --smoke-vault` (also in `--smoke`); `ts_vault --smoke`. `TS_BUILD_VAULT` (on) builds the server.

### Templates
- New **Empty** template group, shown first in the New Claim dialog: 2D, 2D Top-down, 2D Platformer, 3D, 3D First-person, 3D Third-person, VR and **Truly Blank** (one card with a 2D / 3D switch). Each is a clean, playable starting point with no plugins and, where there's a player, a starter `scripts/player.lua`. The sample claims move to a **Samples** group. Blank 2D / Blank 3D are retired in favour of the Empty group.
- Every template thumbnail is rendered by the engine (`ts_admin --bake-thumbnails`). All template content is engine-made or CC0.
- Engine support behind them: a 2D **platformer** controller mode (gravity, grounded jumps, ceiling hits) alongside top-down, playable empty 2D scenes, a 3D **first-person** view with eye height, and a new 3D **Camera** component (field of view) used as the Play camera when there's no player. New inspector fields: Movement, Gravity, Jump Speed, View, Eye height and Camera. The new `scene.json` fields are optional, so older scenes load unchanged.
- `--smoke-templates` now creates every template in a temp folder, opens it in its editor and runs Play headless (top-down walk, platformer land and jump, first-person eye height and look, third-person orbit, VR snap turn and teleport).

### Updates
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
- `.blend` import finds Blender in more places: Steam libraries on any drive (parsed from `libraryfolders.vdf`; Steam found through the registry), *Add or remove programs* entries, Microsoft Store / winget installs and the `Blender Foundation\*` registry keys. There's a new editor setting, **Settings > Tools > Blender executable** (Browse..., saved), and `TS_BLENDER` still overrides it. The "not found" error lists where it looked and points to the setting.

### Assets
- Asset metadata moved to one hidden folder: every `.meta` now lives in `<claim>/.tombstone/meta/<asset path>.meta` instead of next to its asset, so the Asset Browser and the folders on disk show only real assets. The first time an existing claim is opened, its sidecar `.meta` files move there automatically: GUIDs are kept, a backup goes to `.tombstone/meta_backup/<time>/`, the moves are logged in `.tombstone/meta.log`, and `.tombstone/meta.json` stamps the layout as version 2. Running it again does nothing. Editor renames, moves and deletes carry the `.meta`. Files moved in Explorer get their GUID back on the next scan (matched by content hash, or by a unique file name + type when the file was also edited; the relink is logged). Orphaned `.meta` files are reported in the toolbar and only moved to `.tombstone/meta_orphans/` or deleted after you confirm. Exports still pack model `.meta` files (as sidecars, for the runtime importer). See [docs/assets.md](docs/assets.md).
- Sorted imports: files brought in from outside go to type folders by default: `assets/Models/<Name>/` (a model with its sibling files and textures), `assets/Textures/`, `Materials/`, `Audio/`, `Fonts/`, `Scenes/`, `Animations/`, `Other/`, plus the existing `prefabs/` and `scripts/`. This applies to files dropped from Explorer onto the editor window (new), Asset Browser > **Import...** (new), the Supply Wagon image import and new Vault pulls. Dropping on an Asset Browser folder puts files there instead. Per-claim options (`.tombstone/import.json`): **Sort imports into type folders** (on) and **Keep source folder structure for model packs** (on; off sends a model's textures to `assets/Textures/<Name>/` and rewrites glTF / OBJ texture paths).
- The Vault reads and writes `.meta` files in the metadata folder. Library items keep naming them `<asset>.meta`, so items sent from sidecar-layout claims still pull, converted on arrival.
- `ts_admin --smoke-meta` (also in `--smoke`): metadata folder, migration, relinks, orphans, sorted imports, and Vault round trips with both layouts.

### Release tooling
- `build_release.ps1` also writes `TombStone-<ver>-update.exe`: same AppId, upgrades in place with a minimal wizard, and keeps claims, settings and the Vault. It's listed in `SHA256SUMS.txt`, and `-TestInstall` runs it over the test install.
- The installers' publisher is now **TombStone Team**. The AppId is unchanged, so 0.1.1 upgrades a 0.1.0 install in place.

## 0.1.0 (2026-10-22)

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
