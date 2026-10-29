# Model import

> **Rights:** you are responsible for having the rights to any model you import, and to anything you ship. The engine only reads the file formats. It ships no Valve, Garry's Mod or other third-party models or textures, and its tests build their own tiny files in a temp folder. TombStone is not affiliated with or endorsed by Valve Corporation or Facepunch Studios; their names are used only to say which file formats are read.

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
- Source: each `$cdmaterials` folder plus material name is looked up as `materials/<cd>/<name>.vmt` in the content roots (see [Source engine formats](#source-engine-formats-mdlvtfvmtgma) for the order). Lookups ignore case. The VMT gives `$basetexture`, `$bumpmap` / `$normalmap` (green flipped to OpenGL), `$color` / `$color2` (tint), `$alpha`, `$translucent`, `$alphatest`, `$nocull`, `UnlitGeneric`, and `patch` / `include`.
- VTF 7.0 to 7.5 (with 7.3+ resources): DXT1, DXT1 with one-bit alpha, DXT3, DXT5, RGBA8888, ABGR8888, RGB888, BGR888, BGRA8888, BGRX8888, ARGB8888, BGR565, RGB565, BGRA4444, BGRA5551, BGRX5551, I8, IA88, A8, UV88, RGB888_BLUESCREEN, BGR888_BLUESCREEN, UVWQ8888, UVLX8888, RGBA16161616F and RGBA16161616 (P8 is read as greyscale).
- A texture that can't be found or decoded shows a **magenta checker** and is listed in the asset's warning.

## Source engine formats (.mdl/.vtf/.vmt/.gma)

Source engine content (models from Garry's Mod addons, Half-Life 2 and other Source games) comes in as `.mdl` models with `.vmt` materials and `.vtf` textures, often packed in `.gma` addon archives. You need the rights to anything you import, and to anything you ship (see *Rights check* below).

### Importing a .gma addon

- **Asset Browser > Import... > Import Source addon (.gma)...**, or dropping a `.gma` file onto the editor window, picks a `.gma` (or a compressed Workshop download, often `.bin`). A picker lists the addon's name, author and files, grouped as Models, Materials, Sounds and Other, with a checkbox per group and per file. Every file's CRC32 is checked; damaged files are greyed out and never extracted. Paths that try to leave the folder (`..`, drive letters, absolute paths) are refused.
- **Import selected** copies the chosen files into the claim under `assets/Models/<Addon>/`, keeping their Source paths (`models/...`, `materials/...`, `sound/...`), so a model finds its materials the way the game would. A selected `.mdl` brings its `.vvd` / `.vtx` / `.phy` / `.ani`. Existing files are never overwritten (a second import of the same addon goes to `<Addon>_2`). The Asset Browser then rescans: models, `.vtf` textures (with thumbnails) and sounds are imported.
- From the command line: `ts_admin --import-gma <file.gma|.bin> --claim <claim folder> [--dest assets/Models/MyAddon] [--only models,materials,sound,other] [--list]`. It prints the contents and CRC status, extracts, imports, and lists each model's materials (found, or where they were looked for).
- Compressed downloads: LZMA "alone" streams (5-byte properties + 8-byte size header) are detected and decompressed by our own small LZMA decoder (`src/render3d/import/Lzma.cpp`). Other compression formats are reported as "not a .gma".

### Where materials are looked for

For each material the model names, every `$cdmaterials` folder is tried in the model's order, each against every content root, in this order (first hit wins):

1. the asset's **Material search path** import setting (`;`-separated; relative paths start at the model's folder);
2. folders above the model: any with a `materials/` folder, and the folder holding a `models/` folder (an extracted addon or a game folder);
3. the claim: its root, `assets/`, every pack under `assets/Models/` and `assets/` that has a `materials/` folder (so a model addon and a separate material addon both extracted into the claim find each other), `assets/Materials/`, `assets/Textures/`;
4. **Settings > Tools > Source content search folders** (`;`-separated, **Add...** button): each folder, its `garrysmod/` child, and their `addons/*` and `download/` folders. Settings lists Source game folders it found in your Steam libraries (for example `...\steamapps\common\GarrysMod\garrysmod`) with an **Add** button. These folders are only read, never copied or changed. `.vpk` archives are not read: extract what you need first.

A material that is still missing keeps the magenta checker. The model's `.meta` info lists every material (`source_materials`: status `ok` / `partial` / `missing` / `override`, the `.vmt` and `.vtf` used, and for missing ones exactly which `materials/<cd>/<name>.vmt` files were tried in which folders); the warning says the same, and the Asset Browser shows it under **Source materials** in the model's details. **Relink...** picks a `.vmt`, `.vtf` or image for one material by hand (saved in the model's `material_overrides` setting, relative to the model when it is inside the claim); **Clear** removes it.

### Materials

- `.vmt` files are KeyValues text: the shader name (`VertexLitGeneric`, `LightmappedGeneric`, `UnlitGeneric`, `Eyes` / `EyeRefract`...), keys in any case, `//` comments, `[$X360]`-style conditions skipped, dx-level fallback blocks used for keys the main block lacks, `Proxies` ignored. `patch` materials follow `include` (nested patches too); `insert` adds keys the included material lacks and `replace` overrides them.
- Mapped onto the engine's material: `$basetexture` (or `$iris`) -> albedo, `$bumpmap` / `$normalmap` -> normal map (not `$ssbump`), `$color2` / `$color` -> tint, `$translucent` / `$additive` / `$alpha` < 1 -> alpha blend, `$alphatest` (+ `$alphatestreference`) -> alpha mask, `$nocull` -> double sided, `UnlitGeneric` -> unlit. `$phong`, `$envmap`, `$detail`, `$selfillum` and proxies are ignored.
- The renderer draws alpha-mask materials with a cutoff and alpha-blend materials after the opaque ones (no depth writes, not sorted). Albedo textures are treated as sRGB and normal maps as linear, as for every other format.

### Already-imported models

The model importer version is now 3: models imported before this change are reimported on the next Asset Browser scan, so their materials are looked up again through the new search order. **Reimport** (right-click, or the details pane) does it by hand and reloads the model in the open scene; do that after changing the search folders.

### Rights check (Ship It)

Files extracted from a `.gma` are tagged in the claim's ledger, `.tombstone/provenance.json` (origin `gma:<addon name>`, the addon's author, the source file and the time); imported `.mdl` and `.vtf` assets get `"origin": "source-engine"` in their `.meta` info. Files with Source extensions that have no tag (copied in by hand) count as `source-engine` too. **Ship It** (the Freight Office and `ts_export`) lists the ones that would ship in a *Rights check* warning, "you need the rights to ship these", until you tick **I have the rights to ship all of these** (the claim) or the box next to each file under *Rights*. The acknowledgement is saved in the ledger. It never blocks an export. `ts_admin --ack-rights <claim> [<asset>] [--revoke]` does the same from the command line. The ledger is editor data and never ships.

### Clean-room and licences

The `.gma`, VMT / KeyValues, VTF and material lookup code and the LZMA decoder were written from public descriptions of the formats (the Valve Developer Community wiki pages, the public LZMA specification, which is in the public domain). No Source SDK code or headers are included, and no GPL / LGPL code (VTFLib, gmad) was used or linked.

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
| Collider | `collider` | `auto` | the collider a dropped model gets: **Auto** (the model's own hulls when it has them, a **capsule** for skinned models, else an **oriented box** when that is tighter than a plain box), **Tight box**, **Oriented box**, **Capsule**, **Convex hull**, **Convex pieces**, **Mesh**, **None**. *None* also sets `generate_collider` off, which skips `.phy` / `UCX_` hulls |
| Hull vertices | `hull_vertices` | `64` | most corners a generated convex hull may keep (4 to 256) |
| Convex pieces | `decompose_max` | `8` | how many hulls **Convex pieces** may cut a concave mesh into (1 to 32) |
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

**Fitted colliders.** When you drop a model, its collider is fitted using the asset's *Collider* setting and remembers that it was fitted (`"fit"` in the scene's collider JSON). The capsule fit is upright: it's as tall as the bind pose, and its radius covers most vertices (the 80th percentile of distance from the vertical axis), so T-pose arms don't make it a barrel.

The shapes that follow the mesh:

- **Tight box** is the axis-aligned box around what is drawn.
- **Oriented box** is the same idea turned to sit tighter on a rotated mesh. A crate that is already square keeps a plain box; the rotation is only saved when the oriented box is clearly smaller. The collider's **Rotation** field is that turn, in degrees.
- **Convex hull** is one hull of the drawn vertices, simplified down to *Hull vertices*.
- **Convex pieces** cuts a concave mesh into several convex hulls (an approximate convex decomposition, the same idea as V-HACD, written here rather than linked). Use it for a dynamic prop that is not a box. A triangle mesh cannot be dynamic: Play says so and uses the convex hull instead.
- **Mesh** is the model's triangles, for static ground and walls.

The generated hulls are written into the model's `.meta` (`info.fitted`), keyed by a fingerprint of the drawn vertices and the limits above. Loading the model reads them back instead of building them again. A reimport (or a change to *Hull vertices* / *Convex pieces*) changes the key and writes a fresh cache, and fitted colliders in the open scene are refit. **View > Colliders + joints** draws the hull or the triangles, not only the orange bounds. The inspector's Collider section has the fit mode and a **Fit to mesh** button. Editing the size, offset, rotation or shape by hand makes the collider manual (*(manual)* in the inspector), and reimports then leave it alone.

**Splitting a model into parts.** A file with several meshes (glTF nodes, FBX / OBJ / DAE objects, a Source model, a `.blend` brought in as glTF, and the other formats the universal importer reads) can be separated:

- **Asset Browser > Import... > Split into separate assets**, before you pick the file. Each mesh becomes its own `.gltf` under `<model>_parts/`, with its materials and textures, a sensible name from the node, and its own `.meta` in `.tombstone/meta`.
- Right-click the model and choose **Split into separate assets...** or **Place as parts in the scene...**.

**Keep original position** leaves the vertices where they were, so the pivot stays at the model's origin. **Recentre pivot** moves the pivot to the part's centre and remembers that centre (`origin` in the part's settings); placing the parts puts each child back where it sat. **Fit a collider to each part** gives every new asset a collider from the *Collider* setting. The original file is left as it is. Existing names are not overwritten (`post_2.gltf`).

**Place as parts** makes one parent object and a child per part, so you can move the parts on their own. That place, and a split from the right-click menu, are one undo step each (Ctrl+Z puts the scene back and removes the files that step created). The new assets are ordinary files under `assets/` with ordinary `.meta` files, so the Vault and a posse receive them the same way as anything else you import.

Skinned parts are written in their rest pose, rigid. A model that is already one mesh is left alone ("This model is one piece already.").

**Existing scenes and assets.** Model `.meta` info written by an older importer (`importer_version` below 2) is reimported on the next asset scan. When a scene opens, any box collider whose size and offset match the old raw-vertex bounds of its model exactly is refit as **Auto**. This happens automatically, in one undo step, and is reported in the Telegraph. Colliders you sized by hand are not touched.

**Big skeletons.** A draw skins at most 128 bones. For models with more (many GMod player models with finger and face bones), the importer gives each skinned primitive its own palette of at most 128 bones and splits primitives that use more, so every vertex is skinned. Before this fix, vertices on bones past 128 collapsed towards the origin.

**Hitboxes (Source).** The default hitbox set of a `.mdl` (`$hbox`, studiohdr hitbox set 0) is imported as per-bone boxes: bone, hit group (1 = head, 2 = chest, 3 = stomach, 4 / 5 = arms, 6 / 7 = legs), name and bone-space min / max. You get them in code through `Model::hitboxes` and `render3d::hitbox_matrices(model, palette)`, which give bone-to-model matrices for the rest pose or for any animated palette. **View > Hitboxes** draws them in the 3D editor (head red, the rest teal) and follows the animation preview. The Asset Browser shows how many a model has. Only data from your own files is used; no Valve or GMod content ships with the engine.

## Hot reload

Every model's content hash and timestamp include the files it reads: `.vvd`, `.vtx`, `.phy` for Source; the `.mtl` files for OBJ; the `.bin` for glTF. Editing any of them reimports the model and reloads it in the open editor. Moving or deleting a `.mdl` in the browser moves or deletes its sibling files too.

## Blender

`.blend` files are opened by Blender itself. The importer uses the first Blender it finds, in this order:

1. the asset's *Blender path* import setting (an exe or its folder; if set, nothing else is tried);
2. the `TS_BLENDER` environment variable;
3. **Settings > Tools > Blender executable** in the editor (type a path or use **Browse...**; it's saved in `TombStoneConfig/settings.json` as `blender_path`);
4. `PATH`;
5. the registry: the `.blend` open command, `HKLM` / `HKCU\SOFTWARE\BlenderFoundation` (and `Blender Foundation\*`) install folders, and *Add or remove programs* (Uninstall) entries whose name contains "Blender";
6. `Program Files\Blender Foundation\Blender *` (newest first) and readable Microsoft Store packages;
7. Microsoft Store and winget: `%LOCALAPPDATA%\Microsoft\WindowsApps\blender.exe`, `WinGet\Links\blender.exe` and `WinGet\Packages\BlenderFoundation.Blender*`;
8. **Steam**: the Steam folder from `HKCU\Software\Valve\Steam\SteamPath`, `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath` and `Program Files (x86)\Steam`. Every library listed in its `steamapps\libraryfolders.vdf` is checked for `<library>\steamapps\common\Blender\blender.exe`, so a Blender installed to a second drive (e.g. `E:\SteamLibrary`) is found. On Linux and macOS, `~/.steam/steam`, `~/.local/share/Steam` and `~/Library/Application Support/Steam` are checked;
9. the usual Linux and macOS places (`/usr/bin`, `/snap/bin`, flatpak, `/Applications/Blender.app`).

Settings shows the Blender it found when the field is empty. The importer runs `blender -b <file> --python <script>` to export a `.glb` into `%TEMP%\tombstone_blend\` (keyed by path and timestamp; 300 s timeout), then loads that. If no Blender is found, the asset's error lists every place it looked and points to **Settings > Tools > Blender executable** (or `TS_BLENDER`).

## Source models: details

- Coordinates: Source is Z-up in inches. With the defaults a model is rotated to Y-up and scaled to metres. Triangles are wound to match the vertex normals.
- Single-bone models load rigid; models with more bones get a skeleton (any number of bones; see *Big skeletons*), weights from the `.vvd`, and the bind pose from the bone table.
- Hitbox set 0 becomes `Model::hitboxes` (see *Hitboxes*).
- Sequences become clips named after the sequence (the first animation of each). RLE, Quaternion48 and Quaternion64, Vector48, raw positions and rotations, and sectioned animations are decoded.
- `.phy`: each `COLLIDE_POLY` convex ledge becomes a hull, placed by its bone. When the collider is a ConvexHull (the default when a model with hulls is dropped), each hull becomes its own Jolt convex shape.
- LODs come from the `.vvd` fixups and the `.vtx` LOD tables; if the asked-for LOD doesn't exist, the last one is used.

## Limits

- Source: flexes (face morphs), eyeballs, eye and mouth shaders, jigglebones, attachments, hitbox sets other than the first, and ragdoll constraints are not imported. Delta (additive) animations, animations in `.ani` blocks and `$includemodel` animations are skipped with a warning; blended sequences use their first animation. MDL versions outside 44 to 49 are refused (v53 is Titanfall / CS:GO-era).
- VTF: only the largest mip of frame 0, face 0, slice 0 is used (animated textures show their first frame, cube maps their first face); the ATI formats (ATI1N / ATI2N), the console / special formats past UVLX8888 and palette lookups for P8 are not supported and keep the checker. VMT proxies, `$detail`, `$envmap` and `$phong` are ignored. `.vpk` archives are not read.
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
- `TS_AUDIO=null ts_admin --smoke-mesh-parts` (also part of `--smoke`) splits the sample lantern into post / arm / glass, checks keep-position and recentred pivots and the `.meta` files, places the parts as a parent with children and undoes / redoes that (including the new files), then checks an oriented box, a vertex-limited hull, convex pieces of an L, the hull cache, a triangle-mesh floor in Jolt, and a sprite polygon in Box2D.
- `--smoke-models` also checks the Steam `libraryfolders.vdf` parser on canned files (the current layout with `"path"` keys and escaped backslashes, and the old numbered layout), checks that the editor's Blender setting is used, and checks the "not found" message.
- `TS_AUDIO=null ts_admin --smoke-gmod` builds an addon in memory (our own model, VMTs, DXT1 and BGRA8888 VTFs, a WAV, a Lua file and an unsafe `../` path) and checks: CRC32, `.gma` read / write, per-file CRC mismatch, truncation, an LZMA-compressed download, the import picker selection (a `.mdl` bringing its siblings), extraction into a claim (no overwrites, no unsafe paths) and its provenance records; VTF 7.1-7.5 decode of DXT1 / DXT3 / DXT5 and 11 uncompressed formats pixel by pixel, mips x frames, cube maps with and without the sphere map; VMT comments / case / conditionals / fallback blocks and a two-level `patch` chain; the content root order, `$cdmaterials` order (the first folder wins), a material found in a search folder's `addons/`, the missing-material log, a relink; the asset database info; and the Ship It rights warning with per-asset, revoked and claim-wide acknowledgement.
- All of these are part of `ts_admin --smoke`.
