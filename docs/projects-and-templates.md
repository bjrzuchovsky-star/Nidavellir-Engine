# Project types and templates

Source: `src/editor/projects/ProjectStore.*`, `src/editor/ProjectInfo.h`, `src/editor/templates/TemplateStore.*`, `templates/`.

## project.json

```json
{ "name": "Sample 2D Platformer", "dimension": "2d", "vr": false,
  "created": "...", "last_opened": "...", "plugins": "..." }
```

- **`dimension`** is `"2d"` or `"3d"`. 2D claims open the 2D editor and 3D claims open the 3D editor. A file with no `dimension` loads as 2D.
- **`vr`** turns a 3D project into a VR project. The legacy value `"dimension": "vr"` loads as 3D with VR on. You can flip VR in the create flow or in **Claim Settings**.
- **`plugins`** is the list of plugins enabled for this project (see below).

Each project is a folder under `projects_root` (default `./TombStoneProjects/`). It holds `project.json`, `scene.json`, `assets/`, `scripts/` and optionally `ui/`.

## Claim templates

**+ New Claim...** opens the template picker. You can filter by All, 2D, 3D, VR or Mine. Each template is `templates/<id>/template.json` plus a baked `project/` folder, which gets copied when you create a claim.

| id | Dimension | Contents (from `templates/`) |
|---|---|---|
| `blank-2d` | 2D | Empty claim |
| `blank-3d` | 3D | Empty claim |
| `dusty-town` | 2D | Rider sprite and `.anim.json`, `scripts/gate.lua`, `scripts/gold.lua` |
| `frontier-survival` | 2D | Enables `ts.frontier`. Has `survival/*.json` data and `scripts/supply_crate.lua`. |
| `sky-pirates` | 2D | Enables `ts.shipyard`. Has `shipyard/parts.json`, `sky.json`, the sloop and marauder designs, and `scripts/bounty_board.lua`. |
| `ghost-town-3d` | 3D | `drifter.gltf` (skinned), `lantern.gltf`, `boards.tga`, scripts (`bell`, `hank`, `rider`, `town`, `windmill`) and `ui/main_menu.ui.json` |
| `haunted-saloon-vr` | 3D + VR | `hand.gltf`, scripts (`bottle`, `ghost`, `piano`, `spittoon`) |

`template.json` fields: `id`, `name`, `dimension`, `vr`, `description`, `plugins`, `tags`, `order`, `thumbnail`, `project`, and an optional `generator`. If the `project/` folder is missing, the generator's built-in sample writer is used instead.

Templates are searched in this order:

1. `$TS_TEMPLATES_PATH`
2. `<exe>/templates`
3. The source tree

**Save as Template** writes the current claim into `<projects_root>/.templates/`, where it shows under **Mine**. `ts_admin --bake-templates <dir>` regenerates the baked folders.

## Plugins per project

Turn plugins on or off for a project in the **Outfitter's Wagon** (the Plugins panel, `src/editor/plugins/PluginsPanel.*`). Turning one on brings its dependencies along, and turning one off also turns off its dependents. The list is saved to `project.json` right away. `ts_game`, `ts_server` and `ts_client` load the project's plugins too.

Built-in plugins (`src/plugins/`):

| Plugin | Default | What it is |
|---|---|---|
| `ts.posse` | on | Multiplayer |
| `ts.tumbleweed` | off | Sample gameplay plugin |
| `ts.frontier` | off | Survival layer, `src/plugins/survival` |
| `ts.shipyard` | off | Airship Works ship builder, `src/plugins/shipyard` |
| `sample.lantern` | n/a | Sample dynamic plugin |

The root README documents Frontier Survival and Airship Works in depth.
