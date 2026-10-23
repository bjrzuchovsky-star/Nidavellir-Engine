# Editor guide (`ts_admin`)

Source: `src/editor/` (namespace `ts::tombstone::editor`), `apps/admin/main.cpp`.

## Screens

The app moves through these screens:

1. **Loading** (`LoadingScreen`): splash, then it advances to Login on its own.
2. **Login** (`LoginScreen`): username / password, **Dev login**, Settings.
3. **Project Manager** (`ProjectManagerScreen`, `ProjectManager_Templates.cpp`): the list of claims, **+ New Claim...**, Open, delete ("Bury Project?"), **Trading Post**, Settings and Logout.
4. **Settings** (`SettingsScreen`): `projects_root` (**Browse** opens an in-app folder picker), theme (**Dusk** dark, **Parchment** light, under "Lamplight"), `auto_login_dev` and username. **Apply** validates the settings, writes them and reloads the project list.
5. **Editor**: `Editor2DScreen` for 2D claims, `Editor3DScreen` for 3D and VR claims.

The theme and branding live in `src/editor/ui/Theme.*` and `Brand.h`.

## 2D editor

The 2D editor uses an ImGui docking layout. Panels:

- **Hierarchy**: create, duplicate, rename, delete, multi-select, search, drag to reparent.
- **Viewport**: the edit canvas, and Play while riding.
- **Inspector**: transform, tint, layer, and the TileMap, Sprite, Player, Camera2D, Spawn Point, Collision, Animator, Script (one section per script, see [scripting](scripting.md#script-components-several-per-object)), Audio Source, Particles and Light sections, and **Add Component**.
- **Tile Palette**, **Stable** (animation), **Supply Wagon** (assets), **Scripts** and **Telegraph** (console).
- **Network** (posse), **Saloon Band** (mixer), **UI Preview** and **Lighting**.

Each panel has its own `Editor2DScreen_*.cpp`.

Every edit is an undo step (Ctrl+Z / Ctrl+Y). History is snapshot based, capped at 200 steps and kept in memory only (`Workspace2D_History.cpp`). Every edit also autosaves `scene.json` (`SceneIO_Save.cpp`).

| Action | Input |
|---|---|
| Move | LMB drag (Esc cancels) |
| Multi / box select | Ctrl / Shift + click, or drag on empty canvas |
| Duplicate / Delete | Ctrl+D / Del |
| Nudge | Arrows (Shift = ×10, or 4 cells with snap) |
| Snap / grid / cell size | Shift+G (or Shift+Tab) / `'` / `[` `]` |
| Resize / rotate | 8 handles / the round knob (Shift = keep aspect or 15° steps) |
| Move / rotate / scale (Blender style) | G / R / S with the mouse over the viewport, then X / Y, typed values, Enter or Esc; see [transforms](transforms.md) |
| Gizmo | Q Tweak (box handles), W Move, T Scale, Y Transform, or 0-4 (2 = Rotate); `,` Local / Global, `.` pivot |
| Duplicate-drag | Alt+drag |
| Copy / cut / paste | Ctrl+C / X / V (scene.json text on the clipboard; ids are remapped) |
| Group / ungroup | Ctrl+G / Ctrl+Shift+G |
| Lock / hide | Ctrl+L, H (hide is editor-only) |
| Frame selection | F |
| Collision overlay | K |

### TileMaps

TileMaps have the tools Select **V**, Brush **B**, Erase **E**, Fill **U**, Rect **R** and Eyedropper **I** / Alt+click. Brush sizes are 1 / 2 / 3.

Without a tileset image, TileMaps use a built-in palette of 16 colours. Right-click a palette tile to make it solid or walkable; the result is saved under `tile_solidity` in `scene.json`.

### Supply Wagon and sprites

Images live in `<project>/assets/` (png, jpg, bmp, tga, gif, psd, pnm). Drag an image onto the viewport to create a sprite entity. Decoding is handled by `src/gfx/TextureCache`.

### Collision (2D)

Collision code is in `src/runtime/World_Collision.cpp`. It uses axis-aligned box colliders (solid or trigger, static or dynamic) plus solid tiles. Movement resolves X, then Y, with substeps, and overlapping dynamic bodies are pushed apart. This is the engine's own collision code. The Jolt / Box2D physics work is still coming.

## 3D editor

Source: `src/editor/editor3d/`.

- **Camera:** orbit with RMB or Alt+LMB, pan with MMB, wheel to zoom, fly with RMB + WASD / Q / E, F to frame.
- **Gizmos** (`Gizmo3D`, Blender style): Q Tweak, W Move, E Rotate, T Scale, Y Transform (or 0-4). G / R / S move, rotate and scale with the mouse, X / Y / Z lock an axis, numbers type exact values. Shift+G toggles snap (defaults: 0.5 m, 15°, 0.25); Ctrl while dragging flips it. The toolbar Snap target **Surface** sticks moves to the surface under the mouse. End drops the selection to the ground. See [transforms](transforms.md).
- **Panels:** Hierarchy, Inspector, Assets, Environment, Telegraph, Plugins, Animation Preview.
- **Dropping assets:** drop a model (glTF, `.mdl`, OBJ, FBX... see [model import](model-import.md)) in the viewport to place it with a fitted collider. Drop an image on an entity to make it the albedo texture.

## Field Guide (help built into both editors)

Source: `src/editor/ux/`. Shared by the 2D and 3D editors; tips and messages live in `UxStrings.cpp` and `UxTips.inc`.

- **Tooltips:** hover any toolbar button, Inspector field or menu item for a one-line explanation, with its shortcut in brackets. Settings > Field Guide has **Show tooltips** and the hover delay.
- **Empty states:** a panel with nothing in it (Hierarchy, Inspector, Asset Browser, Scripts, Vault, Telegraph) says what goes there and has a button to make the first one.
- **Walkthrough:** the first time you open a 2D or 3D claim, a short tour (8 stops each) points out the main panels. **Skip tour** or Esc ends it; tick **Don't show again** to keep it closed. Re-run it any time from **Help > Walkthrough**.
- **Command palette (Ctrl+Shift+P):** type part of any menu item, panel or setting and press Enter. Matching is fuzzy (`dup` finds Edit > Duplicate), and each row shows its shortcut. Ctrl+P is still Play in 2D; the palette is only on Ctrl+Shift+P.
- **Context menus:** right-click the viewport, an object, a Hierarchy row, an Asset Browser file or an Inspector component header. You get the same actions as the menus (duplicate, rename, delete, copy / paste component values, reset transform, focus, and so on), and each has a tooltip.
- **Readable errors:** failures show *what happened*, *why it probably happened* and *what to do*. **Details** has the raw message and **Copy details** puts everything on the clipboard for a bug report.
- **Saved layouts:** **Window > Layouts** saves the current panel arrangement under a name, loads one, deletes one, or puts back the default. Layouts are kept per user in `TombStoneConfig/layouts/2d` and `.../3d`. `imgui.ini` is left as it is.
- **Safety:** deleting objects, assets, Trading Post plugins and string keys asks first. If a save failed (or the scene is read-only because it came from a newer TombStone), leaving the editor or closing the window asks you to **Stay**, **Try saving again** or **Leave without saving**. Normal edits autosave, so you only see this prompt when something went wrong.
- Shortcut labels use the same format everywhere (`Ctrl+Shift+Z`), and the menus, tooltips and palette all show the same label.

Tests: `ts_admin --smoke-ux` (also in `--smoke`). Screenshots: `ts_admin --ux-shot palette|walkthrough|error <out.png>`.

## Play mode

- **F5 / Ctrl+P:** play and stop.
- **F6:** pause.
- **F10:** step one tick.
- **C:** free camera.
- **K:** collision overlay.

Play runs a copy of the scene in the runtime (`PlaySession`, fixed 60 Hz). Stop restores the editor exactly as it was. Scripts hot-reload while you ride. **Esc never ends a ride.**

## The Telegraph

Source: `src/editor/console/TelegraphLog.*`.

The Telegraph is the shared log for scripts, triggers, toasts and editor notes. It has level filters, search and Clear. Click a line to select its entity. It keeps the last 5000 lines.
