# Transforms and gizmos

Since 0.2.0, both editors move, rotate and scale like Blender. You can press a key and steer with the mouse (a *modal* transform), or drag a gizmo handle. Both work the same way in the 2D and 3D editors. The 2D editor has the X and Y axes only.

Source:
- Shared logic: `src/editor/transform/`. `ModalTransform` holds the state, axis locks and typed values. `GizmoShapes` holds the handle shapes and picking. `ModalKeys` reads the keys while a transform runs. `Gizmo2D` lays out the 2D gizmo.
- 3D: `src/editor/editor3d/Editor3DScreen_Transform.cpp` and `Gizmo3D`.
- 2D: `src/editor/screens/Editor2DScreen_Transform.cpp` and `Workspace2D_Xform.cpp`.

## Modal G / R / S

Select something and keep the mouse over the viewport. In the 2D editor, the Select tool must be active.

| Key | What it does |
|---|---|
| **G** / **R** / **S** | Start a move / rotate / scale. The mouse drives it. G, R or S again switches to that operation (still one undo step). |
| **X** / **Y** / **Z** | Lock to that axis (global). Press it again for the local axis, and a third time to unlock. |
| **Shift+X / Y / Z** | Lock to the plane that leaves that axis out. For example, Shift+Z moves on X and Y. In 2D, Shift+X locks Y and Shift+Y locks X. |
| **0-9**, **.**, **-** | Type an exact value (metres or pixels, degrees, or a factor). `-` flips the sign. **Backspace** deletes a character. **Tab** goes to the next component when nothing is locked. |
| **Ctrl** (held) | Flip snap while held (grid steps, 15°, or 0.1 / 0.25 scale steps). |
| **Shift** (held) | Precision: the mouse moves things at a tenth of the speed. |
| **R R** (3D) | Trackball rotate. |
| **Enter**, **Space** or **LMB** | Confirm. This is one undo step ("Move 3 entities", "Rotate Wagon"...). |
| **Esc** or **RMB** | Cancel. Everything goes back to where it started, and no undo step is added. |
| **Alt+G** / **Alt+R** / **Alt+S** | Clear the location / rotation / scale of the selection (one undo step each). In 3D these are position 0, no rotation and scale 1. In 2D they are x = y = 0, rotation 0 and a 64 × 64 size (the new-entity default) around the same centre. |

While a transform runs, a header shows the operation, the typed value, the axis and space, and whether snap is on. For example: `Move  X [20|]  along X (Global)  snap`. The viewport also shows a line along the locked axis and a dashed line from the pivot to the mouse.

Locked entities don't move. Children follow their parents. Multi-select works, with the pivot modes below.

## Gizmos

The gizmos follow Blender: red / green / blue are X / Y / Z. They keep the same size on screen, light up under the mouse, and hold Ctrl to flip snap while dragging.

- **Move:** drag the white centre circle to move freely, an arrow to move along one axis, or a square to move on a plane.
- **Rotate:** drag an axis ring or the white view ring. In 3D, the inside of the ball is a trackball.
- **Scale:** drag the white outer circle to scale everything evenly, a cube-tipped handle to scale one axis, or a square to scale a plane.
- **Transform:** all three gizmos together.

Dragging a handle is one undo step. In the 2D editor, *Tweak* mode (the default) keeps the familiar box handles and the round rotate knob. Choosing a gizmo hides them.

Pick the gizmo with the buttons along the top of the viewport, the toolbar radio buttons (Tweak / Move / Rotate / Scale / All), or these keys (with the mouse over the viewport, or with the viewport focused):

| Key | 3D | 2D (Select tool) |
|---|---|---|
| Q / 0 | Tweak (no gizmo) | Tweak (box handles) |
| W / 1 | Move | Move |
| E / 2 | Rotate | 2 only (E stays Erase) |
| T / 3 | Scale | Scale |
| Y / 4 | Transform | Transform |
| `,` | Local / Global | Local / Global |
| `.` | Pivot menu | Pivot menu |

**Local / Global** (the toolbar checkbox, the dropdown, or `,`) sets the axes that gizmos and axis locks use. Scale always works along each object's own axes, so it never shears.

## Pivot point

Choose the pivot from the dropdown next to the gizmo buttons, or from the `.` menu.

| Pivot | Rotates and scales around |
|---|---|
| Median Point (default) | The average of the selected objects' centres |
| Individual Origins | Each object's own centre |
| Active Element | The primary (last clicked) selection |
| 3D / 2D Cursor | The cursor. **Shift+RMB** click places it and **Shift+C** puts it back at the origin |
| Bounding Box Centre | The centre of the selection's bounds |

## Shortcut changes in 0.2.0

These keys used to do something else:

| Editor | Old | New |
|---|---|---|
| 3D | G toggled the grid | G moves; the grid toggle is `'` |
| 3D | R picked the Scale gizmo | R rotates; the Scale gizmo is T |
| 3D | Hold Ctrl while moving to stick to a surface | Ctrl flips snap; pick **Surface** as the toolbar snap target to stick to surfaces |
| 3D | Single keys needed a focused viewport | They also work with the mouse over the viewport |
| 2D | G toggled the grid | G moves (Select tool, over the viewport, with a selection); the grid toggle is `'` (View > Show Grid) |
| 2D | R always picked the Rect tool | R rotates over the viewport with the Select tool and a selection; otherwise it is still Rect |
| 2D | S did nothing | S scales (same conditions) |
| 2D | Shift+right-click opened the context menu | Shift+right-click places the 2D cursor; a plain right-click still opens the menu |
| Both | Shift+G toggled snap | Still does; Shift+Tab does too |

Keys that are unchanged: Delete, Ctrl+Z / Ctrl+Y, Ctrl+D, Ctrl+G, F, H, End (3D), arrows, and the tile tools V / B / E / U / I. X is an axis key only while a transform runs; the rest of the time Ctrl+X still cuts. None of these keys fire while you type in a text field or rename something.

## Not yet

- Mirroring with a negative scale is clamped at a small positive size.
- Alt+S in 2D resets to 64 × 64, not to the sprite frame's size.
- TileMaps rotate and resize their position only; their grid isn't scaled or turned.

## Tests and screenshots

- `ts_admin --smoke-transform` is also part of `--smoke`. It drives G / R / S with typed values, axis and plane locks, local axes, snap, Ctrl and Shift, cancel, switching operations, Alt resets, every pivot mode, the cursor, gizmo picking and handle drags in both editors. It checks the undo steps too.
- `ts_admin --gizmo-shot 3d <out.png>` and `ts_admin --gizmo-shot 2d <out.png>` render the editors with the Transform gizmo on a selection. They use a throwaway claim and never touch `imgui.ini`. On Linux, run them under `xvfb-run`.
