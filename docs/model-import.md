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
| LOD | | | also picks between `_LOD0` / `_LOD1` / `LOD2` meshes in glTF / FBX / OBJ... (see *Bounds, colliders and hitboxes*) |
| Collider | `collider` | `auto` | the collider a dropped model gets: **Auto** (the model's own hulls when it has them, a **capsule** for skinned / character models, else a **tight box**), **Tight box**, **Capsule**, **Convex hull**, **Mesh**, **None**. *None* also sets `generate_collider` off, which skips `.phy` / `UCX_` hulls |
| Animations | `import_animations` | on | |
| Blender path | `blender_path` | empty | `.blend` only; a `blender.exe` or its folder |

Body groups and skins are chosen per asset. To place the same model with two different skins, copy the model files under a second name.

## Bounds, colliders and hitboxes

A model's bounds (the orange selection box, picking, focus, drop to ground, align, and fitted colliders) are the box around **what is drawn**:

- only vertices that triangles use (stray or unused vertices don't count);
- after the unit scale and up axis are applied;
- skinned meshes in their **rest pose**, posed by the same skinning palette the renderer uses. Raw bind-space vertices can be at a very different scale or place: FBX rigs in centimetres with a 0.01 armature, or rigs whose bind pose isn't their rest pose;
- without collision or extra-LOD meshes. Meshes or nodes named `UCX_*`, `UBX_*`, `UCP_*`, `USP_*`, `UMB_*` (Unreal), `*-colonly` / `*_colonly` (Godot), `*_collision`, `*_collider`, or under a node called `collision` / `colliders` aren't drawn. They become the model's convex collision hulls instead (when *Collider* isn't *None*). For meshes tagged `_LOD<n>` / `LOD<n>`, only the chosen *LOD* is kept, falling back to the finest one present.
- Bones, empties and helper nodes never count, however far away they are.

**Blender FBX and bone-parented meshes.** Blender's FBX exporter writes a mesh that is parented to a bone (a prop in a hand, armour plates, eyes, a jaw) relative to the *armature* plus the bone's length along +Y, while FBX says it should be relative to the bone. Read literally, such a rig comes in exploded: parts float metres from the body and the box grows to cover them. When the file's creator is Blender, the importer tries both readings and uses the armature one when it puts clearly more of those meshes on the skeleton. The import note then says *"Blender FBX: N bone-parented meshes placed relative to the armature"*. The bone length isn't in the file. It's taken from a connected child bone (Blender's *Add Leaf Bones* `_end` bones give it exactly). Without one, it's estimated, so parts on bones without a connected child (often the head) can be a few percent off in depth. Exporting with *Add Leaf Bones* on, or *Apply Transform* / joining props into the skinned mesh, avoids that. Files from other tools are read as written.

**Fitted colliders.** When you drop a model, its collider is fitted using the asset's *Collider* setting and remembers that it was fitted (`"fit"` in the scene's collider JSON). The capsule fit is upright: it's as tall as the bind pose, and its radius covers most vertices (the 80th percentile of distance from the vertical axis), so T-pose arms don't make it a barrel. The inspector's Collider section has a fit mode and a **Fit to mesh** button. Fitted colliders are refit when the model is reimported or the scene is opened. Editing the size, offset or shape by hand makes the collider manual (*(manual)* in the inspector), and reimports then leave it alone.

**Existing scenes and assets.** Model `.meta` info written by an older importer (`importer_version` below 2) is reimported on the next asset scan. When a scene opens, any box collider whose size and offset match the old raw-vertex bounds of its model exactly is refit as **Auto**. This happens automatically, in one undo step, and is reported in the Telegraph. Colliders you sized by hand are not touched.

**Big skeletons.** A draw skins at most 128 bones. For models with more (many GMod player models with finger and face bones), the importer gives each skinned primitive its own palette of at most 128 bones and splits primitives that use more, so every vertex is skinned. Before this fix, vertices on bones past 128 collapsed towards the origin.

**Hitboxes (Source).** The default hitbox set of a `.mdl` (`$hbox`, studiohdr hitbox set 0) is imported as per-bone boxes: bone, hit group (1 = head, 2 = chest, 3 = stomach, 4 / 5 = arms, 6 / 7 = legs), name and bone-space min / max. You get them in code through `Model::hitboxes` and `render3d::hitbox_matrices(model, palette)`, which give bone-to-model matrices for the rest pose or for any animated palette. **View > Hitboxes** draws them in the 3D editor (head red, the rest teal) and follows the animation preview. The Asset Browser shows how many a model has. Only data from your own files is used; no Valve or GMod content ships with the engine.

## Hot reload

Every model's content hash and timestamp include the files it reads: `.vvd`, `.vtx`, `.phy` for Source; the `.mtl` files for OBJ; the `.bin` for glTF. Editing any of them reimports the model and reloads it in the open editor. Moving or deleting a `.mdl` in the browser moves or deletes its sibling files too.

## Blender

For `.blend`, the importer looks for Blender in this order: the *Blender path* setting, the `TS_BLENDER` environment variable, `PATH`, the registry (the `.blend` open command and `HKLM\SOFTWARE\BlenderFoundation`), `Program Files\Blender Foundation\Blender *` (newest first), Steam, and the usual Linux and macOS paths. It runs `blender -b <file> --python <script>` to export a `.glb` into `%TEMP%\tombstone_blend\` (keyed by path and timestamp; 300 s timeout), then loads that. Without Blender the asset shows: *"Blender was not found. Install Blender (blender.org) or export the file to glTF / FBX"*.

## Source models: details

- Coordinates: Source is Z-up in inches. With the defaults a model is rotated to Y-up and scaled to metres. Triangles are wound to match the vertex normals.
- Single-bone models load rigid; models with more bones get a skeleton (any number of bones; see *Big skeletons*), weights from the `.vvd`, and the bind pose from the bone table.
- Hitbox set 0 becomes `Model::hitboxes` (see *Hitboxes*).
- Sequences become clips named after the sequence (the first animation of each). RLE, Quaternion48 and Quaternion64, Vector48, raw positions and rotations, and sectioned animations are decoded.
- `.phy`: each `COLLIDE_POLY` convex ledge becomes a hull, placed by its bone. When the collider is a ConvexHull (the default when a model with hulls is dropped), each hull becomes its own Jolt convex shape.
- LODs come from the `.vvd` fixups and the `.vtx` LOD tables; if the asked-for LOD doesn't exist, the last one is used.

## Limits

- Source: flexes (face morphs), eyeballs, eye and mouth shaders, jigglebones, attachments, hitbox sets other than the first, and ragdoll constraints are not imported. Delta (additive) animations, animations in `.ani` blocks and `$includemodel` animations are skipped with a warning; blended sequences use their first animation. MDL versions outside 44 to 49 are refused (v53 is Titanfall / CS:GO-era).
- VTF: only the largest mip of frame 0, face 0, slice 0 is used; the ATI formats (ATI1N / ATI2N) and palette lookups for P8 are not supported. VMT proxies, `$detail`, `$envmap` and `$phong` are ignored.
- 4 weights per vertex (Source: 3). More than 128 bones are split into per-primitive palettes.
- Assimp: DDS textures show magenta. Morph targets and cameras or lights in the file are not imported. FBX files in centimetres get a warning; set *Unit scale* to `0.01`.
- `.blend` needs Blender on every machine that loads it, including players' machines. For a shipped game, export to glTF.
- Files found only through a material search path outside the project are not exported with the game.
- Body groups and skins are per asset, not per entity.

## Clean-room note

The Source reader was written from public descriptions of the formats: the Valve Developer Community wiki pages on MDL, VVD, VTX, PHY, VMT and VTF, and the structure layouts in the public Source SDK 2013 headers (`studio.h`, `optimize.h`, `vtf.h`), used only as a reference for field offsets and sizes. No GPL code was used. Assimp (BSD-3-Clause, © the assimp team) and cgltf (MIT, © Johannes Kuhlmann) are fetched at configure time and not committed.

## Smoke

- `TS_AUDIO=null ts_admin --smoke-mdl` builds a synthetic Source model (2 bones, body groups, 2 skins, 2 LODs, a sequence, a `.phy` hull, DXT1 and BGRA8888 VTFs) and checks the vertex, bone, material and hull counts for MDL v44, v48 and v49, the `.dx80` fallback, every setting, 9 more VTF formats, and the asset database (siblings, hot reload, moves).
- `TS_AUDIO=null ts_admin --smoke-models` runs the above and then writes tiny OBJ + MTL + TGA, Collada (skinned and animated, Z-up), ASCII FBX, PLY, STL and DirectX `.x` files, imports each, checks hot reload from the `.mtl`, and checks the `.blend` message without Blender.
- `TS_AUDIO=null ts_admin --smoke-bounds` (also run by `--smoke-models`) checks bounds against the drawn vertex extents. Its cases: a skinned rig with a 0.01 armature, a helper bone 500 m away and a stray vertex at 10 km; the same rig as Collada through Assimp at two unit scales; glTF with a unit scale; OBJ with `UCX_` and `_LOD1` meshes; a 200-joint skeleton split into palettes of 128 or fewer that pose exactly like the full one; and the box / capsule / convex collider fits. `--smoke-mdl` also checks the Source hitbox set: a named head box, swapped min / max, and placement at rest and posed.
- All of these are part of `ts_admin --smoke`.
