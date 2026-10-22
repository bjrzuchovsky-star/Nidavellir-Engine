#pragma once

// --smoke-bounds: model bounds / hit box / fitted collider regressions
// (render3d/ModelImport.h, render3d/ColliderFit.h). Every model is built
// here (in memory or in a temp folder); nothing third-party is shipped.
//
//  - a skinned model whose armature is scaled 1/100 (FBX-style centimetre
//    rigs) and that has a far-away helper bone and a stray unused vertex:
//    bounds = the rest-posed (drawn) vertices, not the raw bind vertices
//  - Collada through Assimp with the same rig, plus the unit scale
//  - glTF + unit scale: bounds = vertex extents x scale
//  - OBJ with UCX_ collision and _LOD1 meshes: neither drawn nor counted
//  - a 200-joint skeleton: per-primitive palettes of <= kMaxBones joints
//    pose exactly like the full palette
//  - fitted colliders: tight box, capsule for skinned models
int run_bounds_smoke();
