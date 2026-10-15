#pragma once

// ts_admin --export (headless Ship It) and --smoke-export.
#include <string>
namespace ts {
namespace tombstone {

// ts_admin --export <project|sample:<id>> --out <dir> [--profile shipping|dev]
//          [--server] [--zip] [--runtime <ts_game>] [--name N] [--version V]
// Returns the process exit code (0 shipped, 1 failed, 2 bad arguments).
int run_export_cli(int argc, char** argv);

// Exports every sample (2D, 3D, VR, survival, sky-pirates) into a temp
// folder and runs each exported game with --smoke from the pak only.
int run_export_smoke();

// Distribution: exports Ghost Town 3D with a portable zip + installer,
// checks the DLL imports, runs the exe from a clean folder with a system-
// only PATH, then (when Inno Setup's ISCC is present) installs silently per
// user into a temp folder, runs the installed game --smoke and uninstalls.
// Without ISCC the installer half reports SKIP and the smoke still passes.
int run_installer_smoke();

// A seeded sample's folder (temp root; the 2D one primed like a first open).
std::string export_sample_dir(const std::string& id, std::string* err);

}  // namespace tombstone
}  // namespace ts
