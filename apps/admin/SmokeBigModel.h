#pragma once

// --smoke-bigmodel: a procedural high-poly skinned model (millions of
// triangles, several 4K textures, a long skeleton) imported the way the
// editor does. Asserts the import returns, memory stays bounded, the
// collider is a hull rather than the raw mesh, and the calling thread is
// not blocked for the whole import.
int run_big_model_smoke();
