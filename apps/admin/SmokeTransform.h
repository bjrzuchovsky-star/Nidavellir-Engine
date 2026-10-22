#pragma once

// Headless --smoke-transform: Blender-style modal transforms and gizmos in
// the 3D and 2D editors (docs/transforms.md). G / R / S with axis locks
// (global, local, plane), typed values, snapping (and Ctrl flipping it),
// precision, cancel restoring, Alt+G / R / S, every pivot mode,
// multi-select with children, one undo step per confirm and gizmo handle
// picking. 0 = pass.
int run_transform_smoke();
