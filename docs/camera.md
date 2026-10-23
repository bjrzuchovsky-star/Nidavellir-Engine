# Cameras

From 0.2.0 the Play camera is an object in the scene, in both editors. You can select it, move it with the gizmo, parent it, give it scripts and have several of them. Play looks through the **active camera with the highest priority**.

## Creating a camera

- **Create > Camera** (or **Object > Create camera**). In 3D the camera starts where the editor view is, looking the same way. In 2D it is placed at the centre of the view and follows the rider.
- A new camera has priority 0, so the camera that is already playing keeps playing (ties go by scene order). Use **Make active camera** (context menu, Object menu or the Inspector) to put a camera on top. It turns the camera on and lifts its priority above the others in one undo step.
- Cameras draw an icon in the viewport, plus a **frustum** in 3D or a **view rectangle** in 2D. The camera Play will use is drawn in brass and labelled **Play**. Selected cameras are orange. Icons and frusta are hidden in Play.

## Inspector

| Field | 3D | 2D | Notes |
|---|---|---|---|
| Active camera | ✓ | ✓ | Off = never picked |
| Priority | ✓ | ✓ | Highest active wins; ties go by scene order |
| Projection | Perspective / Orthographic | always orthographic | |
| FOV | ✓ | | Vertical, degrees |
| Near / far clip | ✓ | | Metres |
| Ortho size | ✓ | | Half the view height, metres |
| Zoom | | ✓ | Screen px per world px |
| Clear colour | ✓ | ✓ | Solid background instead of the sky |
| Viewport rect | ✓ | ✓ | x, y, w, h as 0..1 of the window (split screen, picture-in-picture) |
| Rig | Fixed / Orbit / First person | | Orbit: distance, height, collision |
| Follow target | ✓ (Orbit / First person) | ✓ | 0 = the rider |
| Smoothing | | ✓ | Catch-up time, s (0 = locked on) |
| Dead zone | | ✓ | World px box the target roams in without moving the camera |
| Bounds | | ✓ | World box the view may not leave |

A **Fixed** 3D camera looks out of its own transform, so you can parent it to a cart or a door, or give it a Follow constraint (see [transforms.md](transforms.md#attach-follow-and-snap-tools)).

### Preview

In 3D, selecting a camera shows a **Camera preview** inset in the corner of the viewport. It renders the view through that camera to a texture every frame. 2D shows the camera's view rectangle (with the dead zone and bounds when selected) rather than an inset; a 2D preview inset is not in yet.

## Editor view tools (3D)

| Action | Key | Menu |
|---|---|---|
| Look through camera (toggle) | **Numpad0** | Object > Look through camera, context menu |
| Align camera to view | **Ctrl+Alt+Numpad0** | Object > Align camera to view, context menu |

Look through uses the selected camera, or the Play camera when no camera is selected. Orbiting, panning or Numpad0 again takes you back to the editor view. Align camera to view moves the selected (or Play) camera to the editor view in one undo step, carrying its children.

## Orbit collision (3D)

An Orbit camera with **Collision** on keeps its boom out of walls. Static boxes are always checked. When a physics plugin (Jolt, `ts.physics.jolt`) is switched on, a **sphere cast** of the collision radius runs along the boom as well, and the nearer hit wins. Without the plugin the sphere cast does nothing, and the camera falls back to the static boxes only.

## Switching cameras from scripts

Every language reaches the same `camera` table through the plugin bridge.

| | Lua | Graph node | C++ (`#include <tombstone/camera.h>`) | C# |
|---|---|---|---|---|
| Look through a camera | `camera.set_active(id or name)` | **Set Active Camera** (`camera`) | `ts::camera::set_active(*this, cam)` | `this.SetActiveCamera(cam)` |
| Back to the priority rule | `camera.reset()` | **Reset Camera** | `ts::camera::reset(*this)` | `this.ResetCamera()` |
| Camera in use | `camera.active()` | **Active Camera** | `ts::camera::active(*this)` | `this.ActiveCamera()` |
| Change a priority | `camera.set_priority(id, n)` | Plugin Call | `ts::camera::set_priority(*this, cam, n)` | `this.SetCameraPriority(cam, n)` |
| Field of view (3D) / zoom (2D) | `camera.set_fov(id, deg)` / `camera.set_zoom(id, z)` | Plugin Call | via `plugin("camera", ...)` | via `Plugin("camera", ...)` |
| How many cameras | `camera.count()` | Plugin Call | | |

`set_active` overrides the priority rule until `reset()` is called. It takes an entity, an id or a name (0 = the calling object), and returns false for anything that is not a camera.

```lua
-- scripts/cellar_door.lua: look through the cellar camera while the rider is inside
function on_trigger_enter(other) camera.set_active("Cellar Camera") end
function on_trigger_exit(other)  camera.reset() end
```

## VR

VR claims still look through the headset. The scene cameras are ignored while a headset is driving the view (`camera.active()` returns 0), and the VR rig is unchanged. A camera's preview and Look through still work in the editor.

## Old claims: migration on open

Claims saved before 0.2.0 had a built-in follow camera (3D scene v17 and earlier, 2D scene v12 and earlier). They play exactly as before until you open them in the editor. On open, the editor:

1. Copies the scene to `.tombstone/backup/scene.json.pre-cameras` (it does nothing if the backup cannot be written).
2. Adds a **Main Camera** that does what the old camera did. In 3D it is an Orbit camera (or First person, if the rider was set up that way) following the rider, with the rider's old camera distance and height. In 2D it is a camera locked onto the rider (smoothing 0, zoom 1), as the old built-in view was.
3. Saves the scene (3D v18, 2D v13), writes a line to `.tombstone/migrations.log` and notes it in the Telegraph.

The migration is not an undo step. To go back, restore the backup. The smokes ride an old claim before and after migration and check that the view is the same on every tick. Opening a migrated claim again does nothing.

## Tests and screenshots

- `ts_admin --smoke-camera` (also part of `--smoke`) covers create, the fields and their round trip, priority, make active, move / parent / align / look through, undo, Play using the active camera's projection, clear colour and viewport, Lua / graph / bridge switching, orbit follow, the boom sphere cast with and without physics, VR, and old-claim migration in both 2D and 3D.
- `ts_admin --camera-shot camera <out.png>` produces the screenshot above, and `--camera-shot attach <out.png>` the one in [transforms.md](transforms.md#attach-follow-and-snap-tools).
