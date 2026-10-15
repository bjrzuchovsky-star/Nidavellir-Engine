# Autotiles and the sprite slicer

## Autotiles (terrain brushes)

An autotile set (a *terrain*) lets you paint a TileMap with "grass" or
"water" and have the right edge and corner tile picked automatically from
each cell's neighbours. Terrains live in `<project>/tiles/<name>.autotile.json`.

| Kind | Neighbours | Tiles | Use for |
|---|---|---|---|
| `blob47` | 8 (edges + corners) | 47 | Ground, cliffs, anything with inner corners |
| `wang16` | 4 edges | 16 | Paths, rivers, simple walls |
| `rules` | any 3×3 pattern | one per rule | Fences, pipes, special cases |

### Making one

1. Open **File → Autotiles...** in the 2D editor, or the **...** button next to *Terrain* in the Tile Palette.
2. Under **New terrain**, give it a name, a kind, a tile size and two colours, then click **Create**. This writes:
   - `tiles/<name>.autotile.json`;
   - a template tileset `assets/tiles/<name>_blob47.png` (or `_wang16.png`). The template has every layout slot drawn as a flat colour with rims on the open sides.
3. Paint over the template in your image editor, keeping the layout. The panel reloads it when the file changes.

The panel lists each slot with the neighbour mask it stands for. Hover a slot
to see its mask (or its pattern, for rules). The **Try it** area is a scratch
map: left-click paints and right-click erases, so you can check the art
without touching the scene.

### Painting

- Pick the terrain in the **Terrain** combo of the Tile Palette, or with its radio button in the Autotiles panel.
- The Paint tool (any brush size) then paints terrain, and the Erase tool removes it. Painted cells and their neighbours re-pick their tiles as you go.
- *Plain tiles* switches back to normal tile painting.
- If the TileMap is empty, the first stroke switches it to the terrain's tileset and tile size.
- **Re-tile selected TileMap** re-picks every terrain cell in one undo step. Use it after changing the layout or `map`.

### File format

```json
{
  "name": "Prairie", "mode": "blob47",
  "tileset": "assets/tiles/prairie_blob47.png", "tile_size": 32,
  "base": 1,
  "map": [],
  "members": [],
  "edges_connect": true
}
```

- `base`: tile id of layout slot 0. Slot *n* is tile `base + n`.
- `map`: optional explicit tile id per slot. Use it when your sheet has its own order.
- `members`: extra tile ids that count as this terrain, such as decorated variants.
- `edges_connect`: whether the map border counts as terrain, so ground runs off-screen without a rim.

**Layouts.** Masks use these bits:

| N | NE | E | SE | S | SW | W | NW |
|---|---|---|---|---|---|---|---|
| 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 |

- **47-blob:** a corner only counts when both of its edges are terrain, which leaves 47 distinct masks. Slot *n* is the *n*-th of those masks in ascending order. Slot 0 is an isolated cell and slot 46 is fully surrounded.
- **Wang 16:** slot = N·1 + E·2 + S·4 + W·8.

**Rule tiles** list patterns, first match wins:

```json
"mode": "rules",
"rules": [
  {"tile": 5, "pattern": ".x./.-./.x."},
  {"tile": 6, "pattern": ".../x-x/..."},
  {"tile": 7, "pattern": ".../.-./..."}
],
"fill": 7
```

- Rows run top to bottom, separated by `/`.
- `x` means the neighbour is this terrain, `o` means it is not, and `.` means don't care. The centre `-` is the cell itself.
- `fill` is used when no rule matches. When it is 0, the first rule's tile is used.
- A new *Rule tiles* terrain starts with 16 edge rules over a Wang template, as an example to edit.

Engine API (`scene/AutoTile.h`):

- `paint_autotile(map, set, col, row, brush, erase)`
- `retile(map, set, c0, r0, c1, r1)`, `retile_all`
- `neighbour_mask`, `blob47_slot`, `wang16_slot`
- `autotile_json::load_file` / `save_file`

## Sprite slicer

Open **File → Sprite Slicer...** (2D) or **Claim → Sprite Slicer...** (3D), then pick a sheet under `assets/`.

- **Grid** mode cuts by frame size, offset and spacing. *Skip empty* drops fully transparent cells. For horizontal strips, the first guess is square frames as tall as the image.
- **Auto-detect** finds each sprite as an 8-connected island of pixels with alpha above the threshold:
  - islands within *Merge gap* px of each other are joined, so a hat or a muzzle flash stays with its sprite;
  - islands smaller than *Min px* are dropped as specks;
  - frames are ordered row by row, left to right.

The sheet view numbers every frame and outlines the selected clip in amber.
The slicer starts with **one clip per row** of frames. You can rename them;
change the first frame, frame count, FPS and mode (Loop / Once / PingPong);
and add or remove clips. A live preview plays the selected clip.

**Save animations** writes `<sheet>.anim.json` next to the image:

- In grid mode with no offset or spacing, clips whose frames are consecutive cells are stored as compact grid clips.
- All other clips (auto-detect, uneven sheets) are stored as explicit frame rectangles.

`AnimatedSprite` loads the file like any other `.anim.json`.

Engine API (`scene/SpriteSlice.h`):

- `slicer::grid_rects`, `detect_rects`, `clips_per_row`, `build_set`

## Smoke test

`ts_admin --smoke-tiles` (also part of `--smoke`) checks:

- the 47 blob masks and slot numbering, Wang slots and rule matching;
- painting, erasing and re-tiling;
- the terrain templates and `.autotile.json` round trip, and the per-project brush;
- the slicer: grid with empty cells skipped, auto-detect with hat merging, clips per row, both `build_set` forms, and saving and loading `.anim.json` from the panel.

`ts_admin --tiles-shot demo out.png` renders the Autotiles, Sprite Slicer
and Strings panels on a generated demo project. The docs screenshot is made
this way.
