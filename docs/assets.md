# Assets: browser, import pipeline, GUIDs

Both the 2D and 3D editors have an **Asset Browser** panel (docked at the bottom; View → Asset Browser).

## Browser
- Folder tree (drop assets on a folder to move them), thumbnail grid (real thumbnails for images, kind-coloured tiles otherwise), search box, kind filter, tile size, Rescan, New folder, Import..., Options (import sorting).
- Drag a tile into the scene: textures become sprites in 2D; models (with their import scale / collider) and sounds (bus / volume / loop) drop into the 3D scene.
- Details pane: GUID, import info, rename, Find references, Reimport, Delete, per-asset settings.

## Importers
| Kind | Extensions | Info recorded |
|---|---|---|
| Texture | .png .jpg .jpeg | size, alpha |
| Model | .gltf .glb .mdl (Source) .obj .fbx .dae .3ds .ply .stl .x .blend and more (see [model import](model-import.md)) | format, primitives, vertices, triangles, materials, bones, clips, LODs, skins, body groups, hulls, bounds, warnings |
| Audio | .wav .ogg .mp3 | channels, length |
| Font | .ttf .otf | family, glyph count |

## .meta files (the metadata folder)
Each asset gets a `.meta` (JSON): a stable 32-hex `guid`, `kind`, last known `path`, content `hash`, `settings` and `info`. The `.meta` files live in one hidden folder of the claim that mirrors the asset paths, so the Asset Browser and the folders on disk show only real assets:

```
<claim>/assets/Models/crate/crate.gltf
<claim>/assets/Textures/sign.png
<claim>/.tombstone/meta/assets/Models/crate/crate.gltf.meta
<claim>/.tombstone/meta/assets/Textures/sign.png.meta
```

`.tombstone/` is the claim's existing dot folder for engine data (the C++ / C# script builds live in `.tombstone/native` and `.tombstone/dotnet`). Dot folders are skipped by the asset scan, reference rewrites, exports and the Vault's pruning. What's in it:

| Path | What |
|---|---|
| `.tombstone/meta/<asset path>.meta` | the metadata. **Commit it** with the claim. |
| `.tombstone/meta.json` | layout stamp `{"meta_layout": 2, ...}`: this claim uses the metadata folder. Commit it. |
| `.tombstone/import.json` | per-claim import settings (see below). Commit it. |
| `.tombstone/meta.log` | migration, relink and orphan clean-up log. |
| `.tombstone/meta_backup/<time>/` | copies of the sidecar `.meta` files from before the migration. |
| `.tombstone/meta_orphans/` | orphaned `.meta` files you chose to keep. |

If you use git, ignore only the build folders (`.tombstone/native/`, `.tombstone/dotnet/`), not the whole `.tombstone/` folder. Otherwise the GUIDs are lost on a fresh clone and get regenerated, and Vault pulls can't recognise assets.

Exported paks leave the metadata out, except model `.meta` files: the game's model importer reads unit scale / up axis / LOD / skin from them. The exporter packs those as `<model>.meta` sidecars in `game.tspak` (and in a dedicated server's `content/`), which is where the runtime looks when there's no metadata folder. In the editor, the importer finds the `.meta` in `.tombstone/meta` by walking up from the model to the claim.

### Migration from sidecar `.meta` files
Claims from before the metadata folder kept a sidecar `foo.png.meta` next to every asset. When such a claim is opened (the Asset Browser's database opens it), the engine moves every sidecar into `.tombstone/meta/` once:
- GUIDs, settings and info are untouched. Scenes, prefabs and scripts don't change, because they name assets by path and the paths don't move.
- Every sidecar is first copied to `.tombstone/meta_backup/<yyyymmdd-hhmmss>/`, each move is written to `.tombstone/meta.log`, and `.tombstone/meta.json` is stamped `meta_layout: 2`. The editor log shows "Asset metadata moved into .tombstone/meta: N .meta file(s)".
- If the folder already has a `.meta` with the same GUID, the sidecar is just removed. If it has a different GUID, the folder's copy wins and the sidecar stays in the backup (logged as a CONFLICT).
- The migration is idempotent: opening the claim again does nothing. A sidecar that turns up later (an asset copied in from an older claim with its `.meta`) is adopted the same way on the next scan.
- project.json has no format version field, so the layout version is `.tombstone/meta.json`. Old builds don't read the folder. Opening a migrated claim in an older ts_admin gives its assets fresh GUIDs (restore `.tombstone/meta_backup/` if you need to go back).

Settings: texture `filter, mipmaps, srgb, max_size, pixels_per_unit`; model `scale, unit_scale, up_axis, material_path, lod, generate_collider, collider, hull_vertices, decompose_max, import_animations` (plus `skin` / `body_groups` for Source `.mdl` and `blender_path` for `.blend`; see [model import](model-import.md)); audio `bus, volume, loop, stream`; font `size, oversample`. A model with several meshes can be split into one asset per part (right-click, or the import option); each part is an ordinary asset with its own `.meta`.
Applied today: all model settings (at load time, in the editor and the game), model scale/collider and audio bus/volume/loop (on drop). Texture filter/mipmaps/srgb/max_size are stored and validated, but the renderer doesn't use them yet.

## References and moves
Identity lives in the GUID. Scenes and scripts still store project-relative paths, so the runtime and scene format don't change. When an asset moves, every reference in `.json` scenes/prefabs and quoted paths in `.lua` scripts is rewritten, and open scenes reload. A copied file gets a new GUID.
- **In the editor** (rename, drag onto a folder): the file moves, its `.meta` moves with it inside `.tombstone/meta`, and empty mirror folders are removed. Delete removes the file and its `.meta`.
- **Outside the editor** (Explorer, git mv): the `.meta` stays behind in the metadata folder. On the next scan (every 1.5 s, or Rescan) the moved file has no `.meta`, so the database looks for a left-behind one: first the same **content hash**, then (for a file that was moved *and* edited) the same **file name and type**, used only when exactly one left-behind `.meta` has that name. The GUID is kept, references are rewritten, and the relink shows in the editor log and in `.tombstone/meta.log` (`relink: old -> new (GUID ..., matched by content hash)`). You can also move the `.meta` yourself to the mirrored path (`git mv` both).
- **Orphans**: a `.meta` whose file is gone (deleted outside the editor, or moved and changed beyond recognition) is reported in the Asset Browser toolbar ("N orphaned .meta"). Nothing is removed until you confirm: **Move to .tombstone/meta_orphans** (kept, and a file that comes back with the same content gets its GUID back) or **Delete them**.

## Sorted imports
Files brought in from outside go into type folders by default:

| Folder | What |
|---|---|
| `assets/Models/<ModelName>/` | a model, its sibling files (`.bin`, `.mtl`, `.vvd` / `.vtx` / `.phy`) and its textures, keeping their layout relative to the model so its texture paths keep working |
| `assets/Textures/` | images (`.png .jpg .tga .bmp .gif .psd .dds .ktx .hdr .vtf`...) |
| `assets/Materials/` | `.mtl`, `.vmt`, `.mat`, `*.material.json` on their own |
| `assets/Audio/` | `.wav .ogg .mp3 .flac` |
| `assets/Fonts/` | `.ttf .otf .ttc .fnt` |
| `prefabs/` | `*.prefab.json` (the claim's existing prefab folder) |
| `assets/Scenes/` | `*.scene.json`, `*.scene3d.json`, a `scene.json` from elsewhere |
| `scripts/` | `.lua .cs .cpp .h`, `*.graph.json` (the claim's existing script folder, which the C++ / C# builds read) |
| `assets/Animations/` | `*.anim.json` |
| `assets/Other/` | anything else |

`prefabs/` and `scripts/` stay top-level and lowercase because the templates, the prefab library and the script builds already use them. Everything else is under `assets/`.

Ways to import:
- **Drop files or folders from Explorer** onto the editor window. Drop on a folder in the Asset Browser's tree to put them exactly there (a dropped folder keeps its structure). Drop on the grid to put them in the open folder. Drop on "All assets" or anywhere else to sort them.
- **Asset Browser > Import... > Import file... / Import folder (model pack)...** goes to the open folder, or sorted when "All assets" is selected.
- The 2D editor's Supply Wagon **Import Image** puts images in `assets/Textures/`.
- **Vault pulls** of new items: a model and the texture items it depends on go to `assets/Models/<Name>/` with their relative layout kept. A texture, sound or font goes to its type folder. Prefabs, which name their parts by path, and items this claim already has (matched by lock file or GUID) keep their paths. See [vault.md](vault.md).

Existing files are never overwritten: a second `hero.png` becomes `hero_2.png`, and a second `crate` model gets `assets/Models/crate_2/`. Textures a model brings: glTF `images[].uri`, OBJ `.mtl` maps, COLLADA `<init_from>`. For other formats (FBX, GLB, 3DS, `.blend`...) the images next to it and in `textures/` / `materials/` / `tex/` / `maps/` / `images/` subfolders come along (all of them when it's the only model in its folder, otherwise only the ones whose names start with the model's name). Source `.mdl` materials keep using `material_path` (see [model import](model-import.md)).

Per-claim settings (Asset Browser > **Options**, saved in `.tombstone/import.json`):
- **Sort imports into type folders** (default on). When it's off, imports go to `assets/` (a model still gets its own folder, `assets/<ModelName>/`).
- **Keep source folder structure for model packs** (default on). When it's on, a model's textures stay with it in `assets/Models/<ModelName>/`. When it's off, they go to `assets/Textures/<ModelName>/`, and the model's texture paths are rewritten (glTF uris, OBJ `.mtl` map lines). Formats whose paths can't be rewritten (FBX, DAE...) keep their textures with the model (the log says so), and Vault pulls always keep a pack together, because rewriting a pulled file would make it show as modified. On is the default because a model and its textures then move, copy, send to the Vault and delete as one folder, and no file has to be edited.

## Hot reload
The browser polls the project every 1.5 s. The scan skips the `.tombstone/` folder, so writing `.meta` files (imports, settings, migrations) never shows up as asset changes. Changed files are reimported (for models this includes their sibling files: `.mtl`, `.bin`, `.vvd` / `.vtx` / `.phy`) and textures/models are reloaded in the open editor.

## Find references / deletion
Find references lists every scene, prefab and script that uses an asset. Deleting an asset that's in use asks for confirmation and shows the users.

## The Vault
Right-click an asset > **Send to Vault...** puts it, with its `.meta` and dependencies, into the asset library shared by all your claims. Assets pulled from the Vault keep their GUIDs and show a **V** badge (**UPD** when a newer version is waiting). See [vault.md](vault.md).

## Smoke
`TS_AUDIO=null ts_admin --smoke-assets` and `--smoke-meta` (both also part of `--smoke`). `--smoke-meta` covers: new imports write their `.meta` in `.tombstone/meta`; sorted destinations, re-imports, folder drops, sorting off and split model packs with rewritten paths; migrating a sidecar claim (GUIDs and scene references kept, backup, log, idempotent); editor move and delete carrying the `.meta`; outside moves relinked by hash and by name; orphans reported, parked and recovered; Vault push / pull round trips with the new layout and with items uploaded from sidecar claims; the runtime importer and the exporter finding model `.meta` files. Model formats: `--smoke-models` and `--smoke-mdl`.

You are responsible for having the rights to any model you import.
