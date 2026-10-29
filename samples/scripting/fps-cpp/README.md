# Scripted first person (C++)

`scripts/character_movement.cpp` is the C++ twin of the C# sample. It drives the Wanderer itself and does not use Rider 3D. This claim does not list that plugin.

You need CMake and the Visual Studio 2022 C++ toolchain (MSVC) on Windows, or GCC/Clang on Linux. The editor builds `scripts/*.cpp` into one module and hot-reloads it on save. If the toolchain is missing, the Telegraph says so.

Play, props and the Rider warning are the same as the C# claim. See docs/scripting.md.
