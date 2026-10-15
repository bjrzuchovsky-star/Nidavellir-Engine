# Getting started: building on Windows with Visual Studio 2022 and CMake

## Requirements

- **CMake 3.20 or newer.** The root `CMakeLists.txt` sets `cmake_minimum_required(VERSION 3.20)`.
- **A C++20 compiler.** `CMAKE_CXX_STANDARD 20` is required. On Windows, use Visual Studio 2022 with the *Desktop development with C++* workload.
- **A desktop OpenGL driver** for the admin, game and client apps.
- **Internet access at configure time.** Third-party code is pulled with `FetchContent`, pinned by hash (see `cmake/Fetch*.cmake`):
  - GLFW 3.4, Dear ImGui (docking branch) and stb_image (`FetchImGuiDeps.cmake`)
  - Lua 5.4.8 (`FetchLua.cmake`)
  - ENet 1.3.18 (`FetchENet.cmake`). On Windows it links `ws2_32` and `winmm`.
  - miniaudio 0.11.21 and stb_vorbis (`FetchMiniaudio.cmake`)
  - cgltf v1.14 (`FetchCgltf.cmake`)
  - OpenXR-SDK loader 1.1.63 (`FetchOpenXR.cmake`)
  - Monocypher 4.0.2 (`FetchMonocypher.cmake`)
  - Fonts: Rye and Arvo, SIL OFL (`FetchFonts.cmake`)

  Downloads land under `build/_deps/` and are never committed.
- **Optional:** the .NET 8 SDK for C# scripts. Without it, C# is turned off cleanly. See [scripting](scripting.md).
- **Optional:** SteamVR, set as the OpenXR runtime, for headset VR.

## Build options

From `cmake/TombStoneOptions.cmake` and `cmake/FetchOpenXR.cmake`:

| Option | Default | Builds |
|---|---|---|
| `TS_BUILD_ADMIN` | ON | `ts_admin`, the editor and tools (only target with ImGui) |
| `TS_BUILD_CLIENT` | ON | `ts_client`, the multiplayer client |
| `TS_BUILD_GAME` | ON | `ts_game`, the standalone and shipped runtime |
| `TS_BUILD_SERVER` | ON | `ts_server`, the headless authoritative server (no GLFW / GL) |
| `TS_WITH_OPENXR` | ON | Links `ts_xr` against the OpenXR loader. When OFF, only the desktop VR simulator is built. |

## Configure and build (Developer PowerShell for VS 2022)

```powershell
git clone <repo-url> TombStone_Engine
cd TombStone_Engine
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DTS_BUILD_ADMIN=ON -DTS_BUILD_CLIENT=ON -DTS_BUILD_GAME=ON -DTS_BUILD_SERVER=ON
cmake --build build --config Release
```

You can also open `build/TombStoneEngine.sln` in VS2022, or use *File > Open > CMake...* on the root `CMakeLists.txt`. The MSVC-specific settings (UTF-8 source, `NOMINMAX`-style guards) are in each `apps/*/CMakeLists.txt`.

Build **Release** if you plan to export. The exporter copies the `ts_game` that sits next to the running `ts_admin`, and it warns when that copy is a Debug build. Exported games need the VC++ 2015-2022 x64 redistributable.

## Executables

With the Visual Studio generator, binaries go under `build/apps/<app>/<Config>/`.

| Exe | Purpose |
|---|---|
| `ts_admin` | Editor (Loading → Login → Project Manager → editors) and all the headless tooling (`--smoke*`, `--export`, `--bake-templates`, `--brand-book`, `--store-sign`) |
| `ts_game` | Plays a project (`--project <dir>`) or an exported game (reads `game.json` next to it) |
| `ts_server` / `ts_client` | Multiplayer. See [multiplayer](multiplayer.md). |

## First run

1. Run `ts_admin`. Settings live in `./TombStoneConfig/settings.json`, and projects default to `./TombStoneProjects/`. Both paths are relative to the working directory.
2. At Login, use **Dev login**.
3. In the Project Manager, use **+ New Claim...** and pick a template (see [projects and templates](projects-and-templates.md)).
4. Press **F5** in the editor to play. Use **Launch Game** to start `ts_game` on the project.

## Smoke tests

`ts_admin --smoke` runs the whole headless suite. Single areas, from the flags parsed in `apps/admin/main.cpp`:

`--smoke-3d`, `--smoke-anim3d`, `--smoke-vr`, `--smoke-audio`, `--smoke-ui`, `--smoke-fx`, `--smoke-tools`, `--smoke-prefab`, `--smoke-graph`, `--smoke-cpp`, `--smoke-cs`, `--smoke-export`, `--smoke-templates`, `--smoke-store`, `--smoke-survival`, `--smoke-shipyard`

Also: `ts_game --smoke --project <dir>`, `ts_server --smoke` and `ts_client --smoke`.

Environment switches used by the smokes:

- `TS_SMOKE_REQUIRE_GL=1`: GL checks fail instead of being skipped when there is no display.
- `TS_SMOKE_REQUIRE_CPP=1` / `TS_SMOKE_REQUIRE_CS=1`: a missing C++ or C# toolchain counts as a failure.
- `TS_SMOKE_CS_PREBUILT=<dir>`: runs a prebuilt C# assembly.
- `TS_AUDIO=null`: no sound device.

## Linux and CI

The same tree builds on Linux with GCC 13+ (or Clang) and Ninja. This is the groundwork for a Linux export target. On Debian / Ubuntu, install the GLFW build dependencies first:

```
sudo apt-get install ninja-build libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
  libxi-dev libgl1-mesa-dev libwayland-dev libxkbcommon-dev wayland-protocols
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build/apps/admin && TS_AUDIO=null ./ts_admin --smoke
```

Without a display, the editors and smokes fall back to the null renderer. `xvfb-run` gives the GLFW paths a virtual screen.

`.github/workflows/ci.yml` runs on every push to `main` and on pull requests:

- `windows-latest`: MSVC x64, Release, with the newest Visual Studio on the runner (no pinned generator, so image updates do not break configure).
- `ubuntu-latest`: GCC + Ninja, Release.

Each job builds every app, then runs `ts_server --smoke`, `ts_client --smoke`, `ts_admin --smoke-prefab` and the full `ts_admin --smoke` with `TS_AUDIO=null`. The runners have no GPU and `TS_SMOKE_REQUIRE_GL` is left unset, so the GL-only checks are skipped instead of failing.
