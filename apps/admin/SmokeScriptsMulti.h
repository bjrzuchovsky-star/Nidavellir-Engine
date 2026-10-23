#pragma once

// --smoke-scripts-multi (also part of --smoke): Unity-style script
// components. Several scripts per object in the 2D and 3D editors (attach,
// order, enable / disable, per-script props, remove, undo), New Script for
// every language and template (graph templates check clean; C++ / C#
// templates compile when the toolchain / .NET SDK is there, C# is
// create-only without the SDK), drag-attach through the editor API, the
// VS Code / Visual Studio launch plans and IDE files, old single-script
// scenes loading unchanged, and Play running every script's hooks in
// Inspector order with errors naming the object and the script.
int run_scripts_multi_smoke();
