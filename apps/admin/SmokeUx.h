#pragma once

// --smoke-ux: the Field Guide (editor user-friendliness) checks, headless.
// Action registry coverage (every menu item registered, the palette finds
// and runs them), tooltips on toolbar + Inspector fields, walkthrough
// persistence + skip, layout save / load / reset, friendly error text for
// each failure kind, and the context-menu items. 0 = pass.
int run_ux_smoke();
