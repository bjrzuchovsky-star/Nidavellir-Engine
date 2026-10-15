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
| Model | .gltf .glb | primitives, triangles, materials, skin, clips, bounds |
| Audio | .wav .ogg .mp3 | channels, length |
| Font | .ttf .otf | family, glyph count |

## .meta files
Each asset `foo.png` gets a sidecar `foo.png.meta` (JSON): a stable 32-hex `guid`, `kind`, last known `path`, content `hash`, `settings` and `info`. Commit .meta files with your project. They are left out of exported paks.

Settings: texture `filter, mipmaps, srgb, max_size, pixels_per_unit`; model `scale, generate_collider, import_animations`; audio `bus, volume, loop, stream`; font `size, oversample`.
Applied today: model scale/collider and audio bus/volume/loop (on drop). Texture filter/mipmaps/srgb/max_size are stored and validated, but the renderer doesn't use them yet.

## References and moves
Identity lives in the GUID. Scenes and scripts still store project-relative paths, so the runtime and scene format don't change. When an asset moves (in the browser or outside the editor, with or without its .meta, matched by GUID or content hash), every reference in `.json` scenes/prefabs and quoted paths in `.lua` scripts is rewritten, and open scenes reload. A copied file gets a new GUID.

## Hot reload
The browser polls the project every 1.5 s. Changed files are reimported and textures/models are reloaded in the open editor.

## Find references / deletion
Find references lists every scene, prefab and script that uses an asset. Deleting an asset that's in use asks for confirmation and shows the users.

## Smoke
`TS_AUDIO=null ts_admin --smoke-assets` (also part of `--smoke`).
