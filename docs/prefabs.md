# Prefabs

A prefab is a reusable group of entities saved as a `.prefab.json` asset. You place **instances** of it in a scene. When you change the prefab, every instance updates. Each instance can still **override** any field, and you can push an override back into the prefab (**Apply**) or drop it (**Revert**). Prefabs work the same way in the 2D and 3D editors.

Source: `src/scene/Prefab.h` (format and algorithms, no UI), `src/editor/assets/PrefabLibrary.*` (cache, on-disk change detection, asset undo), `src/editor/screens/Editor2DScreen_Prefabs.cpp` and `src/editor/editor3d/Editor3DScreen_Prefabs.cpp` (editor commands and panels), `apps/admin/SmokePrefab.cpp` (`ts_admin --smoke-prefab`).

## In the editor

**Prefabs panel** (View > Prefabs, docked with the Supply Wagon / Assets):

- **Save Prefab**: select one or more entities, type a name and press the button. The selection and all of its children are written to `prefabs/<name>.prefab.json` (`Lamp Post` becomes `prefabs/lamp_post.prefab.json`; `_2`, `_3`, ... are added if the file already exists). The selected entities become the first instance.
- **Place**: adds a new instance. In 2D it goes next to the selected entity. In 3D it goes at the camera target on the ground. In 3D you can also drag a prefab row into the viewport or onto a Hierarchy row.
- Each row shows how many entities the prefab has and how many instances are placed in the open scene.

**Inspector**: a selected instance entity shows a blue *Prefab: name #instance* header with:

- **Select instance**: selects every entity of that instance.
- **Overrides**: the fields this entity no longer takes from the prefab, each with **Apply** (write the value into the prefab, then every other instance updates) and **Revert** (take the prefab's value again).
- **Apply all** / **Revert all**: the same operations for every override on every entity of the instance.
- **Unpack**: turns the instance back into plain entities with no link.

**Editing a prefab**: edit any instance, then press **Apply all**. You can also edit the `.prefab.json` file directly (text editor, git pull, another editor window). The editors check about once a second for prefab files that changed on disk and update every instance. Instances are also brought up to date when a scene is opened.

Every prefab command is one undo step (Ctrl+Z / Ctrl+Y) and autosaves `scene.json`. For **Save Prefab**, **Apply**, and asset edits, undo also restores the `.prefab.json` file to how it was before, and redo writes it again. A file created by Save Prefab stays on disk after undo; the scene just no longer links to it. A change made on disk outside the editor shows up as one undoable "Prefab changed on disk" step.

## What counts as an override

An override is a **top-level entity field** that differs from the prefab. In 2D these are `name`, `w`, `h`, `r` / `g` / `b` / `a`, `layer`, `rotation`, `sprite`, `collider`, `script`, and so on. In 3D they are `name`, `rotation`, `scale`, `visible`, `mesh`, `material`, `light`, `collider`, `script`, `rigid_body`, and so on. A component object counts as one field: change one material value and the whole `material` is overridden. Adding or removing a component is an override too.

The editors recompute overrides every time they save, so you never mark them by hand. The rules:

- **Position.** The entity linked to the prefab's first root is the instance's *origin*. Moving it moves the instance and is never an override. Every other entity sits at origin + its offset in the prefab. Moving one of those records a `position` override. The offset is a translation only: rotating or scaling the instance root does not move the other entities, and the children follow it only through the normal parenting tools while you edit.
- **Never synced**: `id`, `parent` (rebuilt from the prefab's hierarchy; the instance root keeps its own parent, so an instance can sit under any entity), `locked`, and `hidden` / `editor_hidden`.
- **Structure.** When an entity is added to the prefab, every instance gets it. When an entity is removed from the prefab, it is removed from every instance. If you delete part of an instance, it comes back the next time that prefab changes. Use **Unpack** first if you want to change an instance's structure.
- **Duplicates.** Duplicating or copy-pasting an instance creates a new instance; it never shares one with the original.
- Prefabs do not nest. Saving a selection that already contains instances unpacks them into the new prefab.

## File formats

`prefabs/<name>.prefab.json`:

```json
{
  "prefab": 1,
  "dimension": "2d",
  "name": "Lamp Post",
  "entities": [
    {"id": 1, "name": "Post", "x": 0, "y": 0, "w": 20, "h": 80, "r": 0.35, "g": 0.65, "b": 0.95, "a": 1, "layer": 0},
    {"id": 2, "name": "Lamp", "x": -5, "y": -20, "w": 30, "h": 20, "r": 0.35, "g": 0.65, "b": 0.95, "a": 1, "layer": 0, "parent": 1}
  ]
}
```

The entities are the same objects that `scene.json` writes: 2D entity fields for `"2d"` prefabs, 3D (`position` / `rotation` / `scale`, components) for `"3d"`. Ids are local to the file, starting at 1. The first entity is the anchor at `0, 0[, 0]`, and the other positions are offsets from it. A 2D prefab cannot be placed in a 3D scene, and the reverse is also refused.

Instance link on each placed entity in `scene.json`:

```json
"prefab": {"asset": "prefabs/lamp_post.prefab.json", "instance": 2, "source": 1, "overrides": ["g", "r"]}
```

- `asset`: path relative to the project.
- `instance`: groups the entities of one placement.
- `source`: the entity's id inside the asset.
- `overrides`: sorted field names (`"position"` for a moved non-origin entity).

This link is why the scene formats moved up: **2D `scene.json` v11** and **3D `scene.json` v15**. Older files load unchanged. If an instance's asset is missing, its entities keep their last values; the Inspector shows "asset missing", and the scene still loads and plays. The runtime (`ts_game`, Play mode, export) ignores the link and plays the entities as saved.

## Smoke test

`ts_admin --smoke-prefab` (also part of `ts_admin --smoke`) runs both editors headless. It covers: Save Prefab with a parent and child; placing instances (child offsets and parents); a tint or material override; an asset edit through the API and one on disk, with overrides surviving both; Revert on one key and on all; Apply on one key and on all (including a moved child); undo and redo of Apply, checking both the scene and the asset file; Ctrl+D creating a new instance; save and reload; Unpack plus undo; and the v11 / v15 version stamps.
