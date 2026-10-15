#pragma once

// ts_admin --export (headless Ship It) and --smoke-export.
namespace ts {
namespace tombstone {

// ts_admin --export <project|sample:<id>> --out <dir> [--profile shipping|dev]
//          [--server] [--zip] [--runtime <ts_game>] [--name N] [--version V]
// Returns the process exit code (0 shipped, 1 failed, 2 bad arguments).
int run_export_cli(int argc, char** argv);

// Exports every sample (2D, 3D, VR, survival, sky-pirates) into a temp
// folder and runs each exported game with --smoke from the pak only.
int run_export_smoke();

}  // namespace tombstone
}  // namespace ts
