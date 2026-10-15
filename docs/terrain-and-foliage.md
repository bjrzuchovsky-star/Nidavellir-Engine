# Terrain and foliage (3D)

A 3D scene can hold any number of heightmap terrains. Each one has sculpt brushes, four splat-painted texture layers, LOD chunks, a Jolt heightfield collider and instanced foliage with wind sway and distance fade. Sources: `src/terrain/` (data, brushes, chunks, foliage, the view), `src/render3d/GlTerrainFoliage.*` (shader modules and the instanced foliage pass), `src/world3d/World3D_Terrain.cpp` (Lua) and `src/editor/editor3d/Editor3DScreen_Terrain.cpp` (editor).

## Creating a terrain

In the 3D editor, use **Create > Terrain (hills)**, or open **View > Terrain + Foliage** and click **Create terrain** with a resolution (65 to 1025 vertices per side), a size in metres, a look (meadow, desert or snowy) and optional hills. A new terrain gets its data file `terrain/terrain_N.tsterrain`, layer textures and foliage types for its look. If the scene has no wind, it also gets a light breeze so the plants move.

A terrain is the `terrain` component on an entity (Scene3D **v16**). It sits axis-aligned with its centre at the entity's position; rotation and scale are ignored.

```json
"terrain": {
  "data": "terrain/valley.tsterrain", "resolution": 129, "size": 128,
  "chunk": 32, "lod_distance": 45, "skirt": 1, "roughness": 0.9,
  "layers": [{"texture": "builtin:grass", "tint": [1,1,1], "tiling": 4}, ...4 entries],
  "foliage": [{"name": "pine", "mesh": "foliage:pine", "density": 0.02, ...}],
  "foliage_distance": 1, "collider": true, "friction": 0.8
}
```

Heights, splat weights and foliage instances live in the binary `.tsterrain` file ("TSTR" v1), not in `scene.json`. The editor writes it every time the scene is saved, including autosaves after each edit.

## Brushes

With **Brushes on** checked in the panel, the left mouse button paints on the terrain under the cursor and does not pick or move objects. A ring shows the radius and the hardness. Alt still orbits the camera.

| Tab | Brush | Notes |
|---|---|---|
| Sculpt | Raise / Lower | `strength` metres per second at the centre |
| | Smooth | Blends towards the neighbours' average |
| | Flatten | Pulls towards the height under the first point of the stroke |
| | Noise | Deterministic value noise; `noise size` sets the feature size |
| Paint | Layer 1 to 4 | Moves splat weight to the chosen layer; the four weights always sum to 255 |
| Foliage | Paint | Adds the chosen type on a jittered grid (density x fill). Painting the same spot twice adds nothing new |
| | Erase | Removes the chosen type, or every type, inside the brush |

Modifiers: **Ctrl** inverts the brush (raise and lower swap, paint goes back to layer 1, foliage erases). **Shift** smooths while in Sculpt. **[** and **]** resize the brush. Each stroke is one undo step; undo and redo restore the heightfield, splat and foliage exactly. **Generate** offers hills (seed, amplitude, feature size), Flatten all and Clear foliage, all undoable. Plants follow the ground as you sculpt.

## Layers

Each of the four layers has a texture, a tint and a tiling in metres per repeat. The texture can be built-in (`builtin:grass`, `builtin:dirt`, `builtin:rock`, `builtin:sand`, `builtin:snow`) or any project image. The terrain shader blends the layers by the splat map.

## LOD and rendering

The heightfield is split into chunks of `chunk` quads (16, 32 or 64). A chunk drops to LOD 1 at `lod_distance` metres from the camera, and each further level starts at twice the previous distance. Each level halves the grid. Skirts hide cracks between levels. Edits only rebuild the chunks they touched, using the terrain's edit log, so sculpting stays interactive on large terrains.

Shader code is kept modular so other renderer work can change the lighting without touching terrain code. `GlTerrainFoliage.cpp` provides the GLSL functions `ts_surface_albedo` (splat blend) and `ts_apply_fog`. The mesh shader calls them at its `//@ts:surface` marker, and `ts_sky_fog` is used at the `//@ts:sky` marker. The terrain material uses texture units 4–8.

## Foliage

Foliage types are listed in the panel. Each type sets:

- the mesh: a built-in (`foliage:grass`, `tall_grass`, `pine`, `oak`, `bush`, `rock`, `cactus`) or a project glTF;
- density per m², scale range, two colours;
- wind sway;
- fade start and end distances (scaled by `foliage_distance`);
- maximum slope;
- an optional "only on layer" rule;
- shadows;
- a static capsule collider (useful for trees).

Instances are gathered from a 16 m grid near the camera and drawn with one instanced call per type. Near the fade distance they thin out with a dither instead of popping. Wind comes from the scene environment (`wind_dir`, `wind_strength`, `wind_gust`) and from weather.

## Physics

With `collider` on, Play builds a Jolt heightfield body for the terrain, plus a capsule for each instance of a colliding foliage type. Riders, rigid bodies and raycasts all hit it.

## Lua

```lua
local y = terrain.height(x, z)          -- nil off every terrain
local nx, ny, nz = terrain.normal(x, z)
local layer, weight = terrain.layer(x, z)  -- 1-based dominant layer
local n = terrain.foliage(x, z, radius) -- plants within radius
```

## Smoke and screenshot

`ts_admin --smoke-terrain` covers:

- brushes, LOD chunks, the `.tsterrain` round trip and the Scene3D v16 round trip;
- a Jolt heightfield raycast and a rider standing on a hill;
- foliage paint and erase determinism;
- null-renderer chunk and foliage counts, plus fade;
- the Lua `terrain` table;
- editor strokes with undo and redo.

`ts_admin --terrain-shot out.png` renders a demo valley offscreen. It needs a GL context; on Linux, Xvfb works.
