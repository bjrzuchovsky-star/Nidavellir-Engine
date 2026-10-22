#pragma once

// Headless model import smokes (render3d/ModelImport.h). Every test file is
// generated here, in a temp folder - nothing third-party is shipped.
//
// --smoke-mdl: a synthetic Source model set (.mdl v44 / v48 / v49 + .vvd +
// .dx90.vtx + .phy, VMT + VTF materials: DXT1 with mips, BGRA8888 through
// VTF 7.4 resources) imported and checked: vertex / bone / material counts,
// Z-up inches -> Y-up metres, winding, skin weights, the clip posing a bone
// through the skinning palette, body groups, skins, LODs, the material
// search path, the magenta placeholder, .phy convex hulls, asset database
// info + hot reload when a sibling file changes, moves taking the siblings.
int run_mdl_smoke();

// --smoke-models: --smoke-mdl plus OBJ / MTL (+ TGA texture), Collada
// (Z up, skinned + animated), FBX (ASCII), PLY, STL and DirectX .x through
// Assimp, the .blend "Blender not found" message, and the shared import
// settings (unit scale, up axis) on every format.
int run_models_smoke();
