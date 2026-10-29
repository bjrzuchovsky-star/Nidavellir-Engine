# Physics and rider movement (Trading Post plugins)

From 0.2.0, physics and the built-in player movement are **Trading Post plugins**, not part of every game. A claim switches on only what it uses, and an exported game loads only those plugins. Brenden's rule: optional features ship as store plugins.

| Plugin | Name | Types | What it brings |
|---|---|---|---|
| `ts.physics.jolt` | Jolt Physics (3D) | 3D, VR | Rigid bodies, joints, the Jolt character for the rider, VR throws, terrain heightfields |
| `ts.physics.box2d` | Box2D Physics (2D) | 2D | 2D rigid bodies and joints, Airship Works Box2D hulls |
| `ts.move.rider3d` | Rider 3D (first / third person) | 3D, VR | The 3D Player: WASD / stick walking, jump, first- or third-person camera, mouse look (Look Mouse / Keys, Sensitivity, Invert Y, Smoothing, Capture cursor on Play) |
| `ts.move.topdown` | Top-down Rider (2D) | 2D | The 2D Player in Top-down mode, including Face mouse |
| `ts.move.platformer` | Platformer Rider (2D) | 2D | The 2D Player in Platformer mode (gravity, grounded jumps) |

All five are built in (category **Engine**, off by default) and show in the Trading Post and the Outfitter's Wagon like any other plugin. Switching one on or off saves `project.json` at once; during Play the change takes hold when the ride ends.

## The interfaces in core

The engine, the editor, scripting (Lua, graphs, C++ and C# hooks such as `on_trigger_enter` / `on_trigger_exit`), VR grabbing and exported games only talk to small interfaces:

- `physics3d::World` (`src/physics3d/Physics3D.h`) and `physics2d::World` (`src/physics2d/Physics2D.h`): abstract physics worlds. `create_world()` returns the active plugin's world, or `nullptr` when no physics plugin is on.
- `world3d::RiderController3D` (`src/world3d/RiderController3D.h`): look (mouse / keys / stick, clamp, smoothing) and steering (wish velocity, acceleration, facing, jump) for the 3D Player. Gravity, the move-and-slide sweep and kill-Y stay in the world.
- `runtime::RiderController2D` (`src/runtime/RiderController2D.h`): one per 2D Player mode.
- `core::Providers<T>` (`src/core/Providers.h`): the hand-off. Hosts **offer** a factory per plugin id at start-up (`offer_builtin_providers`, `offer_3d_providers`); a plugin **acquires** it in `on_load` and releases it in `on_unload`. Only offered and acquired providers are live.

The implementations live with their plugins: `src/plugins/physics_jolt` (`JoltWorld`), `src/plugins/physics_box2d` (`Box2DWorld`), `src/plugins/rider3d` and `src/plugins/movement2d`. Registration, the needs scan and the migration are in `src/plugins/split`.

Scene formats stay compatible. A 3D collider may now carry `"rotation"` (degrees) for an oriented box, and `"fit": "obb"` or `"decompose"`. A 2D collider may set `"shape": "polygon"` with `"polygons"` (convex outlines in entity pixels, traced from a sprite). Older files load as they did.

## Mesh-fitted colliders

**Fit to mesh** on a 3D collider, and the asset's *Collider* setting, can build an oriented box, a simplified convex hull, several convex pieces, or a triangle mesh. The pieces are cached in the model's `.meta` and rebuilt when the mesh is reimported. **View > Colliders + joints** draws the wireframe. Jolt simulates them: a triangle mesh is static only, and convex pieces are what you want on a dynamic concave prop. See [model import](model-import.md#bounds-colliders-and-hitboxes).

In 2D, **Fit to sprite** traces a polygon from the sprite's opaque pixels and cuts a concave shape into convex pieces Box2D can use (at most 8 corners each). The platformer still blocks on the polygon's bounding box; the Box2D body uses the polygon. A hole in the sprite is not traced.

## Without the plugins

A claim with no physics or movement plugin still opens and plays:

- **No physics plugin:** the physics world is a no-op. Rigid bodies and joints stay where they were put; colliders still block the rider (the built-in capsule-vs-colliders fallback in 3D, tile / collider collision in 2D), and triggers still fire `on_trigger_enter` / `on_trigger_exit` (overlap tests). VR grabbables can still be picked up and carried, but are not thrown. The Telegraph says once which plugin to switch on, and only when the scene has bodies.
- **No movement plugin:** the Player stands still (gravity and collision still apply in 3D) and the Telegraph names the plugin to switch on. VR rig locomotion (smooth, snap turn, teleport) is part of the VR runtime and does not need a rider plugin.

## Templates and new claims

Every template lists the plugins it needs in `template.json` (`"plugins"`), and creating a claim also adds whatever its scenes use (Player, Rigid Body, Joint, Grabbable, VR Rig, Airship Works Box2D hulls). New claims are marked as past the split (`.tombstone/plugin_split.json`), so they never migrate.

| Template | Physics / movement plugins |
|---|---|
| Empty 2D, Empty 3D, Truly Blank | none |
| Empty 2D Top-down, Dusty Town, Frontier Survival, Sky Pirates | `ts.move.topdown` |
| Empty 2D Platformer | `ts.move.platformer` |
| Empty 3D First-person / Third-person, Empty VR, Ghost Town 3D, Haunted Saloon VR | `ts.physics.jolt`, `ts.move.rider3d` |

In the editor, adding a Player, Rigid Body, Joint, Grabbable or VR Rig switches on the plugin it needs when you next press Play (and the starter scene a fresh claim gets does the same). The Telegraph says what was switched on. A plugin you switch off while the scene already uses it stays off.

## Migrating a claim from before 0.2.0

The first time the editor opens a claim from 0.1.x:

1. It scans the claim's scenes and prefabs for what they use (`required_plugins`).
2. If something is missing, it copies `project.json` to `.tombstone/backup/project.json.pre-0.2.0` (then `-2`, `-3`, ...) and adds the plugins to `"plugins"`. A claim with no list starts from the defaults for its type. Other keys and their order are kept.
3. It writes the marker `.tombstone/plugin_split.json` and appends what it did to `.tombstone/migrations.log`, for example:
   `0.2.0 plugin split: physics and movement are Trading Post plugins now; switched on ts.physics.jolt,ts.move.rider3d for this claim (backup: .tombstone/backup/project.json.pre-0.2.0)`
4. The same lines go to the Telegraph.

A claim that uses neither physics nor a Player is only marked; its `project.json` is left alone. The migration runs once: afterwards the claim's list is yours to change. Until a claim is opened in the editor, `ts_game`, `ts_server`, `ts_client`, export validation and the exporter ride it with the needed plugins added in memory (and print a note), so nothing breaks.

To undo a migration, copy the backup over `project.json`. Delete `.tombstone/plugin_split.json` to let it run again.

## Export

The exporter ships only the plugins the claim uses: the cooked `project.json` in the pak carries the effective list plus `"plugin_split": "1"`, `game.json` lists them under `"plugins"` (editor-only plugins such as `ts.posse` are left out), and `export_report.txt` has a `plugins:` line. Dynamic plugin DLLs are copied only for those plugins. The physics and movement plugins are built into `ts_game`, so they add no files; a game without them simply never loads them. Splitting Jolt and Box2D into their own DLLs is a later step.

`ts_game` prints `[ts_game] physics / movement: ...` (or `none`) at start-up.

## Smoke

`ts_admin --smoke-plugin-split` (also part of `--smoke`) checks:

- the five catalog entries;
- templates declare and switch on their plugins, new claims are marked, and opening one turns the providers on (and closing it, off);
- migration of an old 3D claim (plugins added, other keys kept, backup, marker, log, Telegraph lines), the in-memory runtime list before migrating, reopening changes nothing, Jolt switched off stays off, a 2D claim without a list, and a claim with no physics only being marked;
- Play in 2D (top-down and platformer) and 3D without any plugin (no physics world, rider stands still, crate stays put, a warning naming the plugin), with them (walking, falling crates) and with a rider plugin but no Box2D;
- four exports (top-down only, Jolt + Rider 3D, bare 3D, and an unmigrated 2D claim): `game.json` and the cooked `project.json` list only the used plugins, and each exported game rides and reports what it loaded.

The other smokes ride with every provider on, so they test the engine exactly as before.

`ts_admin --trading-post-shot <out.png>` saves a screenshot of the Trading Post's Engine category over a fresh Empty 3D First-person claim (it never writes `imgui.ini`).
