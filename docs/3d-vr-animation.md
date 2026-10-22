# 3D, VR and animation

## 3D renderer

Source: `src/render3d`, `ts_render3d`.

- **OpenGL 3.3 core** forward renderer. GL functions are loaded through `glfwGetProcAddress`.
- **Passes:** sun shadow map, sky, meshes, then unlit lines.
- **Materials:** PBR-lite with albedo, normal and metal-rough textures, plus emissive. GGX specular.
- **Meshes:** primitives `cube`, `plane`, `sphere`, `capsule`, plus glTF 2.0 `.gltf` / `.glb` via cgltf, and Source `.mdl`, OBJ, FBX, DAE and more (see [model import](model-import.md)).
- **Headless:** `make_null_renderer3d()`.

## 3D scenes and runtime

Source: `src/render3d/Scene3D.*`, `src/world3d/`.

- **Scene file:** `scene.json` with `"dimension": "3d"`.
- **Entities** have a transform plus optional mesh, material, light, collider (box or capsule, optionally a trigger), player, script, audio, particles, animator, `vr_rig` and `grabbable` components. They can also have a `parent`.
- **Play:** the rider is a capsule.
  - WASD to move, Space to jump, E to interact (within 2.5 m).
  - The mouse looks about (Player **Look**: Mouse, the default, or Keys). See [mouse look](projects-and-templates.md#mouse-look-3d-player).
  - Collision is the capsule against static colliders, with gravity.
- **Lua additions:** `pos3`, `set_pos3`, `move3`, `yaw`, `set_yaw`, `spin`, `scale3`, `set_scale3`.

## VR

Source: `src/xr`, `src/world3d/World3D_VR.cpp`, `src/world3d/VrOverlay.*`.

**Runtime.** OpenXR through the Khronos loader. On Windows it renders with `XR_KHR_opengl_enable`. When there is no headset, or on Linux, it falls back to a desktop simulator. The tested runtime is SteamVR.

**Controls:**

- **Trigger:** laser. It calls `on_vr_trigger` on the target, or `on_interact` if the script has no `on_vr_trigger`.
- **Grip:** grab or pull a Grabbable, throw on release.
- **Sticks:** left stick walks, right stick teleports and snap turns.
- **Menu:** opens the comfort menu.

**Comfort settings** are on the VR Rig component.

**Simulator keys:**

- **Look and move:** hold RMB to look, WASD to walk, Q / E to snap turn.
- **Teleport:** hold T to aim, release to jump.
- **Hands:** LMB is the trigger, G or MMB is the grip, hold Shift for the left hand.
- **Other:** M or Tab opens the menu, Ctrl crouches.

**Lua `vr` table:**

- `vr.active()`
- `vr.head()`: x, y, z, yaw
- `vr.hand("left"|"right")`: x, y, z, trigger, grip
- `vr.held(hand)`, `vr.pointed(hand)`: an entity or nil
- `vr.rig()`
- `vr.teleport(x, y, z)`
- `vr.haptic(hand, amplitude, seconds)`

**Hooks:** `on_grab`, `on_release`, `on_point`, `on_unpoint`, `on_vr_trigger`.

**Editor:**

- **Preview in headset** shows a Headset Mirror window.
- **Play (simulator)** runs the scene in the simulator.

**ts_game options:**

- `--sim` forces the simulator.
- `--desktop` forces the desktop window.
- `--xr-probe` checks the OpenXR runtime.
- `TS_VR=sim` picks the simulator.

### SteamVR on Windows

1. Start SteamVR.
2. In SteamVR, choose *Settings → OpenXR → Set SteamVR as OpenXR runtime*.
3. Run `ts_game --xr-probe`.

Smoke test: `ts_admin --smoke-vr`.

## 2D sprite animation (the Stable)

Source: `src/scene/Animation.*`, `src/runtime/World_Anim.cpp`, `Editor2DScreen_Stable.cpp`.

- **Animation sets** are `<sheet>.anim.json` files with a grid and named clips.
- **Clips** can use grid ranges or explicit rects, set `fps` or per-frame `ms`, and use the modes `loop`, `once` or `ping_pong`.
- **Riders** pick `walk_<dir>` / `idle_<dir>` clips automatically.
- **Scripts** use `e:play(clip, hold)` and `e:release()`.

Example: `templates/dusty-town/project/assets/rider.anim.json`.

## 3D skeletal animation

Source: `src/render3d/Skeleton.*`, `Animator.*`, `GltfSkinned.cpp`, `src/world3d/World3D_Anim.cpp`, `Editor3DScreen_Anim.cpp`.

**Import and skinning:**

- The first glTF skin in a file is imported, with 4 weights per vertex.
- Clips support LINEAR, STEP and CUBICSPLINE interpolation.
- Skinning runs on the GPU: a std140 `Bones` block with at most 128 bones. Shadows are skinned too.

**Animator3D component** (scene v12):

- **Parameters:** float, bool, trigger.
- **Layers** with bone masks.
- **States:** a single clip or a 1D blend space.
- **Transitions:** crossfades, including from any state (`*`).
- **Events** call `on_anim_event(name, time)`.
- **Root motion** is optional and moves on XZ only.

**Lua:**

```lua
anim.play(e, "wave", 0.2); anim.crossfade(e, "run", 0.4)
anim.set_param(e, "speed", 3.0); anim.trigger(e, "jump")
anim.param(e, "speed"); anim.state(e, 1); anim.stop(e, 2, 0.3)
```

**Graph nodes:** `anim.play`, `anim.crossfade`, `anim.set_param`.

**Editor:** the Animator inspector, and **View → Animation Preview**.

**Sample assets (CC0):**

- `drifter.gltf` (Ghost Town 3D) has the clips `idle`, `walk`, `run`, `jump` and `wave`.
- `hand.gltf` (Haunted Saloon VR) has a `grip` clip. Set it as the VR hand with `VrRig3D.hand_model`.

**Limits:**

- Only the first skin in a file is used.
- Nodes between joints that are not joints themselves are ignored.

Smoke tests: `--smoke-3d`, `--smoke-anim3d`.
