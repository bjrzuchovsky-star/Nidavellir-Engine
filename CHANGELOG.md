# Changelog

All notable changes to TombStone Engine. The version number lives in the `VERSION` file at the repo root.

## 0.2.3 — 2026-10-29

- Fix: scripted characters (C#/C++ Move with gravity) no longer fall through terrain heightfields or mesh colliders, with or without the Rider plugin or physics.
- Scripted `Move` (the fps C# and C++ samples, unchanged) stands and walks on terrain and on mesh colliders. Previously it only pushed out of box colliders, so gravity fell straight through the heightfield.

## 0.2.2 — 2026-10-29

- C# templates compile again. `Player(slot)` is kept so older scripts still call it; `PlayerId(slot)` is the same id. `Position(e, out x, out y)` is now `TryPosition`, because `Position` is the vector the character sample reads.
- `vswhere` is asked for every product (`-products *`), so Visual Studio 2022 Build Tools count as a C++ toolchain, not only the full IDE.

C# and C++ gameplay scripts can actually drive a game. The acceptance script is `samples/scripting/fps-cs/scripts/character_movement.cs`: it compiles unchanged, and Play runs it with no Rider plugin. `samples/scripting/fps-cpp/` is the C++ twin. `samples/scripting/2d/` is a small coin in both languages.

### C# (.NET 8)
- Every `scripts/*.cs` builds into one `ProjectScripts.dll` (net8.0) and hot-reloads on save. Compile errors land in the Telegraph with file:line; a failed build keeps the last good assembly. Float, int, bool, string and vector fields survive a reload unless the script replaces `Save` / `Load`.
- The C# build lists each source once and skips `obj`, `bin`, `cache` and `build` (a recursive `scripts/**/*.cs` glob was compiling a second copy, and every CS0101 / CS0111 then pointed at the later file only). Two different files that declare the same script class are named in the console, with a hint to rename one.
- API matches the sample: `OnStart`, `OnUpdate(float)`, `Prop(name, default)` (typed), `Toast(title, message)`, `Position` (with `.Y` and a readable `ToString`), `Vector3`, `Transform.Right` / `Forward`, `Move(Vector3)`, `Rotate`, `Find` + `SetLocalRotation`, `Input.GetAxis` / `GetButtonDown`, `Log`, `Sound`.
- `OnFixedUpdate` and `OnLateUpdate` run on the same fixed tick. `OnEnable` / `OnDisable` run with start and destroy.
- Without the .NET 8 SDK the editor says C# is switched off, links the install page, and everything else keeps working. An exported game ships the compiled assembly and needs the .NET 8 **runtime** only when the claim uses C# scripts. See [docs/scripting.md](docs/scripting.md).

### C++
- The same names on `ts::Script` (`OnStart`, `OnUpdate`, `Prop`, `Move`, `Input::GetAxis`, …). Older `on_start` / `on_tick` scripts still run.
- One module per claim, hot-reload on save, errors with file:line, last good module kept. `save()` / `load()` keep the fields you write out.
- A missing MSVC toolchain (Windows) or C++ compiler (Linux) is reported in plain language instead of a raw CMake failure.

### Play
- A C# or C++ script can move, jump, fall and look with no Player component and no Rider 3D plugin. `Move` separates the body from static colliders. Mouse look is captured in Play for that kind of claim.
- If Rider 3D is on **and** the player object also has a C# or C++ script, Play warns that the two will fight.

### Smokes
- `--smoke-cs` and `--smoke-cpp` compile, load, tick, round-trip a Prop, walk the sample controller and hot-reload it. They no longer skip when the SDK and a compiler are present. CI installs the .NET 8 SDK and requires both.

- Scripts can `GetComponent` / `AddComponent` / `RemoveComponent` (built-in components and other scripts), `Instantiate` a named entity or a `.prefab.json`, `Destroy` after a delay, and `FindWithTag` / `SetTag`.
- Lifecycle on both languages, in order: `Awake`, `OnEnable`, `OnStart`, `OnFixedUpdate` (before physics), `OnUpdate`, `OnLateUpdate`, then `OnDisable` and `OnDestroy`.
- `OnCollisionEnter`, `OnCollisionStay` and `OnCollisionExit` fire in 2D and 3D when solid colliders overlap, separate from triggers.
- `Physics.Raycast` / `Overlap` in C# and `Raycast` / `Overlap` in C++ query solid colliders directly, with no physics plugin required.
- The Inspector lists public fields and `[SerializeField]` members, with `[Header]`, `[Range]` and `[Tooltip]`. C++ uses the same tags in comments above the field. Vectors, colours, enums and entity references are included.
- C# coroutines (`StartCoroutine`, `WaitForSeconds`) and C++ `After(seconds, fn)`, plus a small event you can subscribe to and invoke.
- `PlayAnimation(clip)`, `SetFov` and the existing `Sound` call cover clip playback, camera field of view and audio from both languages. Loading a different scene from a script is not yet.

Not in this patch yet (see the parity list in docs/scripting.md): collision callbacks beyond triggers, attribute Inspector, coroutines, debugger attach, and editor scripting.

### Split meshes and mesh-fitted colliders
- **Split into separate assets.** A model with several meshes (glTF / GLB, FBX, OBJ, DAE, `.blend`, Source `.mdl`, and the other formats the universal importer reads) can be separated into one asset per part. Offer it on **Import...** before you pick the file, or right-click a model in the Asset Browser. Each part keeps its materials and textures, takes its name from the node, and gets its own `.meta` in `.tombstone/meta`. **Keep original position** leaves the pivot at the model's origin; **Recentre pivot** moves it to the part's centre and remembers where to put it back. The original file is left alone, and an existing name is not overwritten.
- **Place as parts in the scene** makes a parent object and one child per part, so the parts can be moved on their own. **Fit a collider to each part** gives every child a collider fitted to its mesh. Placing, and a split from the right-click menu, are one undo step each (the new files go away with the undo). The new assets travel with the Vault and a posse like any other asset.
- **Mesh-fitted colliders.** Besides the tight box and the capsule: an **oriented box** (only when it is tighter than the plain box; a crate stays square), a **convex hull** with a vertex limit, **convex pieces** for a concave dynamic prop (our own approximate convex decomposition), and a **triangle mesh** for static geometry. **Fit to mesh** is on the collider. **View > Colliders + joints** draws the hull or the triangles. Generated hulls are saved in the model's `.meta` and rebuilt when the mesh is reimported. Jolt uses them; a triangle mesh on a dynamic body falls back to the convex hull.
- **Fit to sprite** (2D) traces a polygon collider from the sprite's opaque pixels and cuts a concave shape into convex pieces for Box2D. The platformer still uses the bounding box.
- `ts_admin --smoke-mesh-parts` (also in `--smoke`). Docs: [docs/model-import.md](docs/model-import.md), [docs/physics.md](docs/physics.md).

## 0.2.1 — 2026-10-26

A patch for team claims: riders now see the claim's files when they ride in and can place their own. Run `TombStone-0.2.1-update.exe` over 0.2.0; it keeps your claims, settings and the Vault.

### Team claims: riders see and place the posse's files
- **Fixed:** a rider who joined could not see the claim's assets, and assets they imported or placed never reached anyone else. Files only travelled when every editor typed the Team panel's separate "Vault URL (files)", which was empty by default and silently skipped.
- Riding in now brings every file under `assets/` and its `.meta` (same GUIDs and import settings) with nothing to type: `ts_vault --team` tells riders its HTTP port and they use the address they dialled; **Host from my editor** without a Vault carries the files over the team session itself (kept in `.tombstone/team/host/blobs/`). Whatever a rider imports, drops in, pulls from the Vault or places goes to the whole posse, in 2D and 3D.
- Files that arrive mid-ride show up at once: cached textures and models reload and the Asset Browser rescans.
- The Team panel says where files travel and, in red, why they can't: no Vault key, a Vault on 127.0.0.1 (use `ts_vault --host 0.0.0.0`), an older host with no URL, a refused key. Read-only keys see the posse's files and are told theirs stay on this PC.
- The Team panel's Vault URL is now optional (an override). Older hosts fall back to Settings > Vault.
- `ts_admin --smoke-team` checks a rider joining a ts_vault posse (files + identical `.meta`, both ways), a read-only rider, a LAN editor host with no Vault (3D models both ways) and the error for an older host.

### Windows
- Every shipped exe carries version info (Company **TombStone Team**, Product **TombStone Engine**), an application manifest (per-monitor DPI aware, Windows 10 / 11) and the Dusk tombstone icon (`ts_game` takes each game's own). The installers carry the same publisher info and icon.

### Docs
- New [docs/security.md](docs/security.md): who publishes TombStone, checking downloads against `SHA256SUMS.txt`, why the builds are unsigned (SmartScreen / antivirus warnings) and how to report a false positive.

## 0.2.0 — 2026-10-23

A big ride: ride a claim together live with your posse (team claims), physics and player movement as Trading Post plugins, Garry's Mod / Source formats, Blender-style gizmos and G / R / S, Unity-style script components, cameras as scene objects with attach and follow tools, and the Field Guide (tooltips, command palette, walkthroughs, readable errors). `TombStone-0.2.0-update.exe` upgrades a 0.1.x install in place and keeps your claims, settings and the Vault. Older claims migrate on first open, with backups in `.tombstone/backup/`.

### Team claims: ride a claim together, live (2D and 3D)
- **Team > Host from my editor...** (LAN, UDP 24650, optional passcode) or **Team > Join a posse...** (a host's address, or `ts_vault --team` on UDP 8741 with your Vault key). Every edit lands on every screen within a few hundred milliseconds; late joiners get a snapshot and catch up.
- Presence: the **Team** panel lists the posse with colours, scenes and locks; remote cursors and selection outlines in the viewport; `[locked: name]` badges in the Hierarchy. Viewers on read-only keys watch but take no locks.
- Soft locks follow your selection. Edits to something another rider holds are turned back. **Take over** a lock its holder has left idle for 15 s.
- **Mid-drag preview:** the posse sees an object move while you drag it; the edit itself lands once, when you let go.
- Deletes, terrain sculpt / paint / foliage strokes (one compact patch per stroke) and files under `assets/` (through the Vault by SHA-256, on a worker thread with a progress bar; deleted files go to `.tombstone/team/trash/`) all travel.
- Ctrl+Z undoes only your own steps.
- **Sessions survive restarts:** the server keeps each claim's session on disk (`<vault>/team/`, or `<claim>/.tombstone/team/host/` when hosting from the editor). A dropped editor rides back in by itself as the same rider and merges what it changed while away; after an editor restart, **Ride back in** uses `.tombstone/team/last.json` (keys and passcodes are never saved).
- `ts_admin --smoke-team` (also in `--smoke`). Docs: [docs/team-claims.md](docs/team-claims.md).
- Not yet: a merge dialog for conflicting edits made while away (last write wins per entity), chat, roles, follow camera and playing together (1.0.0).

### Editor Field Guide: easier to learn, harder to lose work (2D and 3D)
- **Tooltips** on toolbar buttons, Inspector fields, every menu item and Trading Post rows, all kept in one strings table. Settings > Field Guide turns them off and sets the hover delay.
- **Empty states** in the Hierarchy, Inspector, Asset Browser, Scripts, Vault and Telegraph explain what belongs there and give you a button to start.
- **First-run walkthroughs:** separate 2D and 3D tours (8 stops each). You can skip them or tick "Don't show again", and re-run them from **Help > Walkthrough**.
- **Command palette on Ctrl+Shift+P** with fuzzy search over one action registry that covers the menus, panels and settings. **No shortcuts changed:** Ctrl+P is still 2D Play, so the palette is only on Ctrl+Shift+P.
- **Context menus** on the viewport, objects, Hierarchy rows, Asset Browser files and Inspector component headers (copy / paste component values, reset transform, rename, delete...).
- **Readable errors:** what happened, why and what to do, with **Details** and **Copy details**. A scene saved by a newer TombStone opens read-only and is left untouched.
- **Saved layouts** under **Window > Layouts** (save, load, delete, reset), stored per user. `imgui.ini` is untouched.
- **Safety:** confirmation before deleting assets, Trading Post plugins and string keys. An unsaved-changes prompt (Stay / Try saving again / Leave without saving) appears when a save has failed and you leave the editor or close the window. Shortcut labels use one format everywhere.
- Fixed: the stray ImGui "Debug" window that appeared in the editors.
- `ts_admin --smoke-ux` (also in `--smoke`); `ts_admin --ux-shot palette|walkthrough|error <out.png>`. Docs: [docs/editor-guide.md](docs/editor-guide.md#field-guide-help-built-into-both-editors).
- Not yet: a corrupt 2D `scene.json` is still replaced with a fresh scene. The 2D Audio Source header has no context menu, and built-in components have no Move up / down.

### Cameras are scene objects (2D and 3D)
- **Create > Camera** in both editors. A camera is an ordinary object: select it, move it with the gizmo, parent it, give it scripts. It has a viewport icon and a frustum (3D) or view rectangle (2D), with the Play camera drawn in brass. Icons are hidden in Play.
- Inspector: active, priority, perspective / orthographic, FOV, near / far, ortho size, clear colour and viewport rect (split screen, picture-in-picture). 3D adds the Fixed / Orbit / First person rig with distance, height and collision. 2D adds zoom, follow target, smoothing, dead zone and bounds.
- Several cameras per scene: Play looks through the **active camera with the highest priority** (ties go by scene order). **Make active camera** puts one on top in a single undo step. A new camera does not take over from the one already playing.
- 3D: a **Camera preview** inset (rendered to a texture) when a camera is selected; **Look through camera** (Numpad0) and **Align camera to view** (Ctrl+Alt+Numpad0), also on the new **Object** menu and the context menu.
- Scripts switch cameras: Lua `camera.set_active / reset / active / set_priority / set_fov / set_zoom / count`, graph nodes **Set Active Camera**, **Reset Camera** and **Active Camera**, C++ `<tombstone/camera.h>` and C# `SetActiveCamera` / `ResetCamera` / `ActiveCamera` / `SetCameraPriority`.
- 3D Orbit collision: static boxes always, plus a sphere cast along the boom when the physics plugin is on. Without the plugin the sphere cast does nothing.
- VR claims still look through the headset; scene cameras are ignored while it drives the view.
- **Old claims** (3D scene v17 and earlier, 2D v12 and earlier) play as before until opened in the editor. On open the scene is backed up to `.tombstone/backup/scene.json.pre-cameras` and a **Main Camera** is added that matches the old follow camera tick for tick. Then the scene is saved (3D v18, 2D v13) and the change is logged to `.tombstone/migrations.log` and the Telegraph. Nothing migrates if the backup cannot be written.
- `ts_admin --smoke-camera` (also in `--smoke`); `ts_admin --camera-shot camera <out.png>`. Docs: [docs/camera.md](docs/camera.md).
- Not yet: the 2D editor shows the camera's view rectangle rather than a rendered preview inset.

### Attach, follow and snap tools (2D and 3D)
- On the viewport and Hierarchy context menus and the new **Object** menu: **Attach to...** (keep current position or snap to the target), **Attach to bone...** (3D; rides the animated bone), **Detach** (keeps the world transform), **Follow...** (position, rotation or both, keeping the offset, editable in the Inspector), **Remove Follow**, **Snap to object**, **Copy transform from** (position / rotation / scale checkboxes) and **Align to surface** (3D: rays down plus the surface normal; 2D: onto the top of the tile or collider below).
- Pick the target from a list, with the **eyedropper** (click it in the viewport or Hierarchy, Esc cancels), or by dragging in the Hierarchy (**Alt+drag** snaps).
- Every tool works on the whole selection as one undo step and refuses loops. Locked objects are not moved.
- Scene format: 3D v18 adds `"follow"`, `"parent_bone"` and `"bone_offset"`; 2D v13 adds `"follow"`.
- `ts_admin --smoke-attach` (also in `--smoke`); `ts_admin --camera-shot attach <out.png>`. Docs: [docs/transforms.md](docs/transforms.md#attach-follow-and-snap-tools).

### Script components: several scripts per object (2D and 3D)
- Objects carry any number of scripts (up to 16) in any mix of Lua, visual graph, C++ and C#, like Unity components. Each one is an Inspector section with its own properties, an on/off checkbox, move up / down and remove, all undoable. Properties are saved per object, per script.
- Order: on one object, events reach scripts in Inspector order; across the world each phase runs stack position by stack position, keeping lua, graph, cpp, cs within a position. Single-script claims behave as before.
- **Add Component** at the bottom of the Inspector: **New Script...** (name, language, template) writes the file under `scripts/` and attaches it; **Script** picks an existing one with search. The 2D menu also adds Collider, Player, Camera and Spawn Point.
- Templates for every language: Blank, Gate, Pickup, Sign and Ticker. C++ ones compile against `script_sdk.h`, C# ones against the SDK.
- Drag a script from the Asset Browser or Scripts panel onto an object in the viewport, a Hierarchy row or the Inspector to attach it. The Asset Browser now shows scripts.
- **Open in VS Code / Visual Studio** on each script, with detection and a **Settings > Tools > Open scripts in** default. TombStone writes the VS Code workspace and settings, `compile_commands.json` for C++ and a `.csproj` referencing the C# SDK. A missing editor gives a status message rather than an error.
- Script errors name the object and the script: `"Wagon" (scripts/boom.lua): ...`.
- Scene format: 2D v12 and 3D v17 add a `"scripts"` array with `"enabled"`. Older scenes with one `"script"` still load.
- `ts_admin --smoke-scripts-multi` (also in `--smoke`); `ts_admin --scripts-shot <out.png>`. Docs: [docs/scripting.md](docs/scripting.md#script-components-several-per-object).
- Not yet: the 3D Add Component menu lists scripts only (other 3D components stay on the + buttons).

### Blender-style transforms and gizmos (2D and 3D)
- **G / R / S** move, rotate and scale with the mouse over the viewport in both editors. **X / Y / Z** lock an axis; press it twice for the local axis. **Shift+X / Y / Z** lock a plane. Type exact values (with `-` and Backspace, Tab for the next component); the viewport header shows them. **Ctrl** flips snap and **Shift** gives precision. **Enter** or LMB confirms, as one undo step. **Esc** or RMB cancels and puts everything back. **Alt+G / R / S** clear location, rotation or scale. Multi-select and children work, and locked entities stay put.
- New gizmos in red / green / blue that keep their size on screen and light up under the mouse:
  - **Move:** a free-move centre circle, axis arrows and plane squares.
  - **Rotate:** axis rings, a view ring and a 3D trackball.
  - **Scale:** a uniform outer circle, cube-tipped handles and plane squares.
  - **Transform:** all three together.
- Choose the gizmo from the buttons along the top of the viewport, the toolbar, or the keys **Q / W / E / T / Y** (or 0-4). E is the 3D Rotate gizmo; 2D keeps E for Erase, so its Rotate gizmo is on 2. Choose **Local / Global** from the toolbar checkbox or press `,`.
- Pivot modes **Median Point / Individual Origins / Active Element / Cursor / Bounding Box Centre** are in a dropdown and on the `.` menu. **Shift+right-click** places the 3D or 2D cursor; **Shift+C** resets it.
- **Shortcut changes:**
  - The grid toggle moved from G to `'` in both editors.
  - 3D: the Scale gizmo moved from R to T. Surface snapping moved from holding Ctrl to the toolbar snap target **Surface**.
  - 2D: R rotates over the viewport when the Select tool has a selection, and is the Rect tool otherwise. Shift+right-click no longer opens the context menu. The 2D editor starts in **Tweak** mode with the old box handles.
  - Shift+Tab also toggles snap.
  - No key fires while you type in a text field.
  - See [docs/transforms.md](docs/transforms.md) for the full list.
- `ts_admin --smoke-transform` (also in `--smoke`) covers modal transforms, typed values, locks, snap, pivots, gizmo picking and drags, and undo in both editors. `ts_admin --gizmo-shot 3d|2d <out.png>` renders the gizmos.
- Not yet: mirroring with a negative scale (clamped), and resetting a 2D sprite to its frame size with Alt+S (it resets to 64 × 64).

### Source engine formats (.mdl/.vtf/.vmt/.gma)
- `.gma` addon import: **Asset Browser > Import... > Import Source addon (.gma)...** (or dropping a `.gma` on the editor window) opens a picker (addon name / author, files grouped as Models / Materials / Sounds / Other with checkboxes, CRC32 checked per file) and extracts the chosen files into `assets/Models/<Addon>/`, keeping Source paths. A selected `.mdl` brings its `.vvd` / `.vtx` / `.phy`; existing files are never overwritten; unsafe paths are refused. LZMA-compressed Workshop downloads (`.bin`) are decompressed by a new clean-room LZMA decoder. CLI: `ts_admin --import-gma <file> --claim <dir> [--dest] [--only] [--list]`.
- Source materials are found far more often: each `$cdmaterials` folder is tried against the asset's search path, the folders above the model, the claim (including every addon extracted into it) and the new **Settings > Tools > Source content search folders** (with `garrysmod/`, `addons/*` and `download/` below them; Steam game folders are suggested, read-only). Missing ones keep the checker, and the model's `.meta` lists which `.vmt` files were tried where. **Relink...** in the Asset Browser fixes one by hand (`material_overrides`).
- VMT: proper KeyValues parsing (comments, case, `[$X360]` conditions, fallback blocks, proxies ignored), nested `patch` materials with `insert` / `replace`, `$color2`, `$translucent`, `$alphatest` (+ reference), `$additive`, `$iris`, `$ssbump` skipped. VTF: correct cube-map face count (sphere map before 7.5), format names, header info.
- Renderer: materials have an alpha mode (opaque / mask with cutoff / blend). Masked materials discard below the cutoff; blended ones draw after the opaque meshes.
- `.vtf` files are texture assets (info, thumbnails). Model `importer_version` is now 3, so models imported earlier re-resolve their materials on the next scan; **Reimport** now also reloads the model in the open scene.
- Rights check: files extracted from a `.gma` are tagged in `.tombstone/provenance.json` (origin `gma:<addon>`, author); Source-format files count even untagged. Ship It warns "you need the rights to ship these" with an **I have the rights** acknowledgement per file or per claim (saved, never a block); `ts_admin --ack-rights`.
- `ts_admin --smoke-gmod` (also in `--smoke`): everything above with generated files only. Docs: [docs/model-import.md](docs/model-import.md#source-engine-formats-mdlvtfvmtgma). TombStone is not affiliated with Valve or Facepunch; import and ship only content you have the rights to.

### Physics and movement are Trading Post plugins
- Five new built-in plugins (category **Engine**, off by default), listed in the Trading Post and the Outfitter's Wagon: **Jolt Physics (3D)** `ts.physics.jolt`, **Box2D Physics (2D)** `ts.physics.box2d`, **Rider 3D (first / third person)** `ts.move.rider3d` (mouse look with Look Mouse / Keys, Sensitivity, Invert Y, Smoothing and Capture cursor on Play), **Top-down Rider (2D)** `ts.move.topdown` (with Face mouse) and **Platformer Rider (2D)** `ts.move.platformer`. See [docs/physics.md](docs/physics.md).
- The engine core keeps small interfaces (a physics world for 3D and 2D, rider controllers for 3D and 2D). The editor, scripting (Lua, graphs, C++ and C# hooks including `on_trigger_enter` / `on_trigger_exit`), VR grabbing and exported games all go through them.
- A claim without the plugins still opens and plays. Without a physics plugin, bodies stay put while colliders and triggers still work. Without a movement plugin, the Player stands still. Either way the Telegraph says which plugin to switch on. VR rig locomotion doesn't need a rider plugin.
- Templates list the plugins they need, and a new claim also gets whatever its scenes use. Adding a Player, Rigid Body, Joint, Grabbable or VR Rig in the editor switches on its plugin the next time you press Play, and the Telegraph says so. A plugin you switch off stays off.
- **Older claims migrate on first open.** The editor adds the plugins the claim's scenes use and backs up `project.json` to `.tombstone/backup/project.json.pre-0.2.0` first. It also writes `.tombstone/plugin_split.json` and logs what it did to `.tombstone/migrations.log` and the Telegraph. A claim that uses no physics or Player is only marked. Until a claim is opened, `ts_game`, the servers and export add the plugins in memory, so nothing breaks.
- **Exports ship only the plugins the claim uses.** The list goes into the cooked `project.json` and into `game.json` under `"plugins"`, and the export report has a `plugins:` line. `ts_game` prints what it rides with.
- The plugin split leaves the scene formats (and the Player and collider inspector settings) unchanged; the script components, cameras and attach tools below move them on to Scene3D v18 and 2D v13.
- `ts_admin --smoke-plugin-split` (also in `--smoke`) checks four things. Templates add their plugins automatically. An old claim migrates (backup, marker, log, a second open changes nothing, a plugin switched off stays off). 2D and 3D Play work with and without the plugins. Four exported games ship only their plugins, and each one runs.

## 0.1.2 — 2026-10-23

A small patch release: the Vault without opening a claim. `TombStone-0.1.2-update.exe` upgrades a 0.1.0 or 0.1.1 install in place and keeps your claims, settings and the Vault.

### The Vault
- **Vault** on the Project Manager opens the library without a claim: the same browse, search, kind / tag / 2D / 3D / VR filters, thumbnails, details and versions as the in-claim panel, plus Settings and **Test connection**. See [docs/vault.md](docs/vault.md#the-vault-manager-project-manager).
- **Add files...** sends loose files straight to the Vault (Windows file picker, a pasted path, or files dropped from Explorer on the window), with the usual send dialog (name, author, licence, tags, 2D / 3D / VR). A model brings its sibling files and the textures it names as dependency items.
- **Edit details** (name, author, licence, tags, 2D / 3D / VR, description), **Delete item** and **Delete vN**, each with a confirm. Deleting something another item still needs is refused with the reason, claims keep the copies they pulled, and a deleted version number is never reused.
- **Pull into claim...** with a picker listing your claims, through the regular pull path (metadata in `.tombstone/meta`, sorted type folders, `.vault/lock.json`).
- `ts_vault` API: `PATCH /items/<id>`, `DELETE /items/<id>` and `DELETE /items/<id>/versions/<n>` (write key; **409** while in use). The local library supports the same.
- `ts_admin --smoke-vault-pm` (also in `--smoke`) and `ts_admin --vault-pm-shot <png>`.

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
- New **Empty** template group, shown first in the New Claim dialog: 2D, 2D Top-down, 2D Platformer, 3D, 3D First-person, 3D Third-person, VR and **Truly Blank** (one card with a 2D / 3D switch). Each is a clean, playable starting point with no plugins and, where there's a player, a starter `scripts/player.lua`. The sample claims move to a **Samples** group. Blank 2D / Blank 3D are retired in favour of the Empty group, and the setup and update installers remove them from an existing install.
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
