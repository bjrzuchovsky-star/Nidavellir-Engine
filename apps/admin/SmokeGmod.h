#pragma once

// --smoke-gmod: Source engine formats (.gma / .vtf / .vmt) end to end. Every
// file is generated in code (our own synthetic addon, model, materials and
// textures) - nothing third-party is shipped. Checks: CRC32, .gma
// read / write / per-file CRC / unsafe paths / LZMA-compressed downloads,
// VTF 7.1-7.5 decode (DXT1/3/5, 8888 / 888 orders, I8, IA88, A8, UV88,
// BGRA4444, mips, frames, cube faces), VMT KeyValues + patch include /
// insert / replace, the $cdmaterials x content-root search order (model
// folder, claim, Source content search folders), missing-material logs
// and relinks, extraction into a claim + provenance ledger, the Ship It
// rights warning and its acknowledgement, the asset database info.
namespace ts {
namespace tombstone {
int run_gmod_smoke();
}  // namespace tombstone
}  // namespace ts
