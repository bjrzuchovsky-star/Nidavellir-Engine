#pragma once

#include <string>
#include <vector>

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

// The synthetic Source model set used above (bytes; written by the
// caller as <base>.mdl / .vvd / .dx90.vtx / .phy): a box (material 0)
// and a hat (material 1), $cdmaterials `cd_paths`, material names tex0 /
// tex1. Plus the generated VTFs: DXT1 8x8 (mip 0 green, mip 1 red) and
// BGRA8888 2x2 (orange) through VTF 7.4 resources.
struct SmokeSourceModel {
  std::vector<unsigned char> mdl, vvd, vtx, phy;
};
SmokeSourceModel smoke_source_model(const std::vector<std::string>& cd_paths, const std::string& tex0,
                                    const std::string& tex1);
std::vector<unsigned char> smoke_vtf_dxt1_8x8_green();
std::vector<unsigned char> smoke_vtf_bgra_2x2();
