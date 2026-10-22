# Model import

> **Rights:** you are responsible for having the rights to any model you import. The engine only reads the file formats. It ships no Valve, Garry's Mod or other third-party models or textures, and its tests build their own tiny files in a temp folder.

Every 3D model goes through one loader, `render3d::load_model` (`src/render3d/ModelImport.h`). The 3D editor, Play, the Jolt colliders, exported games and the Asset Browser all use it, so a format that imports in the editor also loads in a shipped game.

## Formats

| Format | Extensions | Reader | What comes in |
|---|---|---|---|
| glTF 2.0 | `.gltf` `.glb` | cgltf v1.14 (MIT) | meshes, PBR materials and textures, first skin, animations |
| Source engine (GMod, HL2, TF2...) | `.mdl` + `.vvd` + `.dx90.vtx` (or `.dx80.vtx`, `.sw.vtx`, `.vtx`) + optional `.phy` | our own clean-room reader (`src/render3d/import/SourceMdl.cpp`) | MDL v44 to v49: meshes per LOD, body groups, skins, skeleton and up to 3 weights per vertex, sequences, `.phy` convex hulls; VMT materials with VTF textures |
| Wavefront | `.obj` + `.mtl` | Assimp 5.4.3 (BSD-3) | meshes, MTL materials and textures |
| Autodesk FBX | `.fbx` (binary and ASCII) | Assimp | meshes, materials, embedded textures, skeleton and weights, animations, FBX up axis |
| Collada | `.dae` | Assimp | meshes, materials, skeleton and weights, animations, `up_axis` |
| Others | `.3ds` `.ply` `.stl` `.x` `.lwo` `.lws` `.ms3d` `.off` `.ase` `.ac` `.md2` `.md3` `.md5mesh` `.smd` `.b3d` `.irrmesh` `.cob` `.xgl` `.zgl` `.nff` `.dxf` `.3mf` `.amf` `.ogex` `.q3o` `.q3s` | Assimp | whatever the format holds (meshes always; materials, skins and animations where the format has them) |
| Blender | `.blend` | Blender itself, run headless | exported to a cached `.glb`, then loaded as glTF |

A `.mdl` that is not a Source model (no `IDST` magic, for example Quake or 3D GameStudio) is handed to Assimp.

Assimp is fetched with `FetchContent` from the v5.4.3 release archive, pinned by SHA-256 (`cmake/FetchAssimp.cmake`). Only importers are built: every exporter is off (`ASSIMP_NO_EXPORT`), and so are the tools, tests, the viewer and Assimp's own glTF, Blend, IFC, USD and M3D importers (glTF stays on cgltf). Turn it off with `-DTS_WITH_ASSIMP=OFF`; then only glTF, Source `.mdl` and `.blend` load.

### Textures

- glTF / Assimp: PNG, JPG, TGA, BMP, PSD, GIF, HDR (stb_image), embedded textures (compressed or raw), and `.vtf`. Texture paths are looked up next to the model, in `textures/` beside it, and in the material search path; a missing extension is tried as `.png`, `.jpg`, `.tga`.
- Source: each `$cdmaterials` folder plus texture name is looked up as `materials/<cd>/<name>.vmt` in the material roots: the material search path, any parent folder that has a `materials/` folder, the folder above a `models/` folder, and the model folder itself. Lookups ignore case. The VMT gives `$basetexture`, `$bumpmap` / `$normalmap` (green flipped to OpenGL), `$color`, `$alpha`, `$nocull`, `UnlitGeneric`, and `patch` / `include`.
- VTF 7.0 to 7.5 (with 7.3+ resources): DXT1, DXT1 with one-bit alpha, DXT3, DXT5, RGBA8888, ABGR8888, RGB888, BGR888, BGRA8888, BGRX8888, ARGB8888, BGR565, RGB565, BGRA4444, BGRA5551, BGRX5551, I8, IA88, A8, UV88, RGB888_BLUESCREEN, BGR888_BLUESCREEN, UVWQ8888, UVLX8888, RGBA16161616F and RGBA16161616 (P8 is read as greyscale).
- A texture that can't be found or decoded shows a **magenta checker** and is listed in the asset's warning.

## Settings (Asset Browser)

Select any model in the Asset Browser. The same settings appear for every format and are stored in the model's `.meta` (`settings`), which is also what the game reads at load time:

| Setting | Key | Default | Notes |
|---|---|---|---|
| Unit scale | `unit_scale` | `0.0254` for `.mdl` (inches to metres), `1` otherwise | baked into the vertices, skeleton and hulls |
| Up axis | `up_axis` | `z` for `.mdl`, `auto` otherwise | `auto` uses the file's own axis (FBX `UpAxis`, Collada `up_axis`); `z` rotates Z-up to the engine's Y-up |
| Placement scale | `scale` | `1` | the entity scale used when you drop the model |
| Material search path | `material_path` | empty | extra folders for textures and Source materials; separate with `;` |
| LOD | `lod` | `0` | Source models with more than one LOD |
| Skin | `skin` | `0` | Source skin family |
| Body groups | `body_groups` | first option | Source, one combo per body group |
| Collider | `generate_collider` | on | Source `.phy` hulls become Jolt convex hulls |
| Animations | `import_animations` | on | |
| Blender path | `blender_path` | empty | `.blend` only; a `blender.exe` or its folder |

Body groups and skins are chosen per asset. To place the same model with two different skins, copy the model files under a second name.

## Hot reload

Every model's content hash and timestamp include the files it reads: `.vvd`, `.vtx`, `.phy` for Source; the `.mtl` files for OBJ; the `.bin` for glTF. Editing any of them reimports the model and reloads it in the open editor. Moving or deleting a `.mdl` in the browser moves or deletes its sibling files too.

## Blender

For `.blend`, the importer looks for Blender in this order: the *Blender path* setting, the `TS_BLENDER` environment variable, `PATH`, the registry (the `.blend` open command and `HKLM\SOFTWARE\BlenderFoundation`), `Program Files\Blender Foundation\Blender *` (newest first), Steam, and the usual Linux and macOS paths. It runs `blender -b <file> --python <script>` to export a `.glb` into `%TEMP%\tombstone_blend\` (keyed by path and timestamp; 300 s timeout), then loads that. Without Blender the asset shows: *"Blender was not found. Install Blender (blender.org) or export the file to glTF / FBX"*.

## Source models: details

- Coordinates: Source is Z-up in inches. With the defaults a model is rotated to Y-up and scaled to metres. Triangles are wound to match the vertex normals.
- Single-bone models load rigid; models with more bones get a skeleton (at most 128 bones), weights from the `.vvd`, and the bind pose from the bone table.
- Sequences become clips named after the sequence (the first animation of each). RLE, Quaternion48 and Quaternion64, Vector48, raw positions and rotations, and sectioned animations are decoded.
- `.phy`: each `COLLIDE_POLY` convex ledge becomes a hull, placed by its bone. When the collider is a ConvexHull (the default when a model with hulls is dropped), each hull becomes its own Jolt convex shape.
- LODs come from the `.vvd` fixups and the `.vtx` LOD tables; if the asked-for LOD doesn't exist, the last one is used.

## Limits

- Source: flexes (face morphs), eyeballs, eye and mouth shaders, jigglebones, attachments, hitboxes and ragdoll constraints are not imported. Delta (additive) animations, animations in `.ani` blocks and `$includemodel` animations are skipped with a warning; blended sequences use their first animation. MDL versions outside 44 to 49 are refused (v53 is Titanfall / CS:GO-era).
- VTF: only the largest mip of frame 0, face 0, slice 0 is used; the ATI formats (ATI1N / ATI2N) and palette lookups for P8 are not supported. VMT proxies, `$detail`, `$envmap` and `$phong` are ignored.
- Skins are limited to 128 bones and 4 weights per vertex.
- Assimp: DDS textures show magenta. Morph targets and cameras or lights in the file are not imported. FBX files in centimetres get a warning; set *Unit scale* to `0.01`.
- `.blend` needs Blender on every machine that loads it, including players' machines. For a shipped game, export to glTF.
- Files found only through a material search path outside the project are not exported with the game.
- Body groups and skins are per asset, not per entity.

## Clean-room note

The Source reader was written from public descriptions of the formats: the Valve Developer Community wiki pages on MDL, VVD, VTX, PHY, VMT and VTF, and the structure layouts in the public Source SDK 2013 headers (`studio.h`, `optimize.h`, `vtf.h`), used only as a reference for field offsets and sizes. No GPL code was used. Assimp (BSD-3-Clause, © the assimp team) and cgltf (MIT, © Johannes Kuhlmann) are fetched at configure time and not committed.

## Smoke

- `TS_AUDIO=null ts_admin --smoke-mdl` builds a synthetic Source model (2 bones, body groups, 2 skins, 2 LODs, a sequence, a `.phy` hull, DXT1 and BGRA8888 VTFs) and checks the vertex, bone, material and hull counts for MDL v44, v48 and v49, the `.dx80` fallback, every setting, 9 more VTF formats, and the asset database (siblings, hot reload, moves).
- `TS_AUDIO=null ts_admin --smoke-models` runs the above and then writes tiny OBJ + MTL + TGA, Collada (skinned and animated, Z-up), ASCII FBX, PLY, STL and DirectX `.x` files, imports each, checks hot reload from the `.mtl`, and checks the `.blend` message without Blender.
- Both are part of `ts_admin --smoke`.
