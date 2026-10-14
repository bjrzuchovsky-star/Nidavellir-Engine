#pragma once

// Headless --smoke checks for the editor tools pass (2D + 3D): resize /
// rotate handles (Shift aspect, snap), Alt+drag copies (Esc takes them
// back), object snapping with guides, parenting (children follow moves,
// turns, deletes, the runtime), hierarchy drops, group / ungroup, lock /
// hide, clipboard copy / paste across scenes, align / distribute, frame,
// filter, add component, drop to ground, surface picks, scene.json v7 (2D)
// and v9 (3D) round trips with older files still loading. Every tool is
// checked to be one undo step. 0 = pass.
int run_tools_smoke();
