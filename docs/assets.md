# Assets: browser, import pipeline, GUIDs

Both the 2D and 3D editors have an **Asset Browser** panel (docked at the bottom; View → Asset Browser).

## Browser
- Folder tree (drop assets on a folder to move them), thumbnail grid (real thumbnails for images, kind-coloured tiles otherwise), search box, kind filter, tile size, Rescan, New folder.
- Drag a tile into the scene: textures become sprites in 2D; models (with their import scale / collider) and sounds (bus / volume / loop) drop into the 3D scene.
- Details pane: GUID, import info, rename, Find references, Reimport, Delete, per-asset settings.

## Importers
| Kind | Extensions | Info recorded |
|---|---|---|
| Texture | .png .jpg .jpeg | size, alpha |
| Model | .gltf .glb .mdl (Source) .obj .fbx .dae .3ds .ply .stl .x .blend and more (see [model import](model-import.md)) | format, primitives, vertices, triangles, materials, bones, clips, LODs, skins, body groups, hulls, bounds, warnings |
| Audio | .wav .ogg .mp3 | channels, length |
| Font | .ttf .otf | family, glyph count |

## .meta files
Each asset `foo.png` gets a sidecar `foo.png.meta` (JSON): a stable 32-hex `guid`, `kind`, last known `path`, content `hash`, `settings` and `info`. Commit .meta files with your project. They are left out of exported paks, except model `.meta` files, which carry the import settings the game needs.

Settings: texture `filter, mipmaps, srgb, max_size, pixels_per_unit`; model `scale, unit_scale, up_axis, material_path, lod, generate_collider, import_animations` (plus `skin` / `body_groups` for Source `.mdl` and `blender_path` for `.blend`; see [model import](model-import.md)); audio `bus, volume, loop, stream`; font `size, oversample`.
Applied today: all model settings (at load time, in the editor and the game), model scale/collider and audio bus/volume/loop (on drop). Texture filter/mipmaps/srgb/max_size are stored and validated, but the renderer doesn't use them yet.

## References and moves
Identity lives in the GUID. Scenes and scripts still store project-relative paths, so the runtime and scene format don't change. When an asset moves (in the browser or outside the editor, with or without its .meta, matched by GUID or content hash), every reference in `.json` scenes/prefabs and quoted paths in `.lua` scripts is rewritten, and open scenes reload. A copied file gets a new GUID.

## Hot reload
The browser polls the project every 1.5 s. Changed files are reimported (for models this includes their sibling files: `.mtl`, `.bin`, `.vvd` / `.vtx` / `.phy`) and textures/models are reloaded in the open editor.

## Find references / deletion
Find references lists every scene, prefab and script that uses an asset. Deleting an asset that's in use asks for confirmation and shows the users.

## Smoke
`TS_AUDIO=null ts_admin --smoke-assets` (also part of `--smoke`). Model formats: `--smoke-models` and `--smoke-mdl`.

You are responsible for having the rights to any model you import.
