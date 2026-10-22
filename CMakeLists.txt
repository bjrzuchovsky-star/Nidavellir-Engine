cmake_minimum_required(VERSION 3.20)
# The one version number: VERSION at the repo root (CMake, ts_admin
# --version, the release installer and zip all read it).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TS_VERSION LIMIT_COUNT 1 REGEX "^[0-9]+\\.[0-9]+\\.[0-9]+")
if(NOT TS_VERSION)
  message(FATAL_ERROR "VERSION must hold a MAJOR.MINOR.PATCH version")
endif()
project(TombStoneEngine VERSION ${TS_VERSION} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TombStoneOptions)

add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/render)
add_subdirectory(src/input)
add_subdirectory(src/physics)
add_subdirectory(src/scene)
# Assay Office core (ts_profiler): frame profiler + console commands.
add_subdirectory(src/profiler)
add_subdirectory(src/fx)
# Virtual filesystem + .tspak archives (runtime mount, export cooking).
add_subdirectory(src/vfs)

# Sound (ts_audio): miniaudio + stb_vorbis fetched by hash, null backend
# when no device opens (headless smokes and tools run it that way).
if(TS_BUILD_ADMIN OR TS_BUILD_GAME OR TS_BUILD_CLIENT OR TS_BUILD_SERVER)
  include(FetchMiniaudio)
  add_subdirectory(src/audio)
endif()

# Windowed apps: GLFW + OpenGL + stb_image for ts_admin, ts_game and
# ts_client (ts_gfx: texture cache + GL helpers). Dear ImGui is fetched only
# for the Admin (see FetchImGuiDeps). ts_server needs none of it.
if(TS_BUILD_ADMIN OR TS_BUILD_GAME OR TS_BUILD_CLIENT)
  include(FetchImGuiDeps)
  add_subdirectory(src/gfx)
  # 3D: renderer (GL 3.3 core + null), glTF via cgltf (fetched by hash),
  # scene.json v8; VR: OpenXR loader (fetched by hash) + desktop simulator.
  include(FetchCgltf)
  # Universal model import: Assimp importers (BSD-3, fetched by hash).
  if(TS_WITH_ASSIMP)
    include(FetchAssimp)
  endif()
  include(FetchOpenXR)
  add_subdirectory(src/render3d)
  add_subdirectory(src/xr)
  # 3D physics core (ts_physics3d): Jolt Physics fetched by hash.
  include(FetchJolt)
  add_subdirectory(src/physics3d)
endif()

# Game runtime (World, fixed-step PlaySession, Lua gameplay scripts). Pure
# C++ with no window or GL; built for every app that runs the game (the
# headless server included). Lua 5.4 is fetched by hash.
if(TS_BUILD_ADMIN OR TS_BUILD_GAME OR TS_BUILD_CLIENT OR TS_BUILD_SERVER)
  include(FetchLua)
  # 2D physics core (ts_physics2d): Box2D v3 fetched by hash.
  enable_language(C)
  include(FetchBox2D)
  add_subdirectory(src/physics2d)
  add_subdirectory(src/runtime)
  # In-game UI + fonts (ts_ui): stb_truetype and two OFL fonts by hash.
  include(FetchFonts)
  add_subdirectory(src/ui)
  # AI toolkit (ts_ai): Recast / Detour fetched by hash.
  include(FetchRecast)
  add_subdirectory(src/ai)
  # Story pieces (ts_story) behind the optional quests / dialogue / saves plugins.
  add_subdirectory(src/story)
  # Weather (ts_weather core; ts_weather3d when the 3D runtime is built).
  add_subdirectory(src/weather)
  # Plugins: interface, catalog, manager (ts_plugin) and the built-ins.
  add_subdirectory(src/plugin)
  add_subdirectory(src/plugins)
  # 3D play runtime (needs ts_render3d, so windowed apps only).
  if(TS_BUILD_ADMIN OR TS_BUILD_GAME OR TS_BUILD_CLIENT)
    add_subdirectory(src/world3d)
    add_subdirectory(src/ai3d)
    add_subdirectory(src/story3d)
    # In-game debug overlay + console, profiler 3D extension.
    add_subdirectory(src/debugtools)
  endif()
endif()

# Networking (ts_net: Transport interface + ENet backend). ENet is fetched by
# hash, only for the apps that talk over the wire.
if(TS_BUILD_ADMIN OR TS_BUILD_CLIENT OR TS_BUILD_SERVER)
  include(FetchENet)
  add_subdirectory(src/net)
  # Multiplayer game layer (protocol, GameServer, GameClient, LocalPosse).
  add_subdirectory(src/mp)
  # MMO backbone (ts_mmo): persistent world server, accounts (Monocypher
  # Argon2i), SQLite persistence, interest management, chat, bots.
  include(FetchMonocypher)
  include(FetchSQLite)
  add_subdirectory(src/mmo)
endif()

# Editor / Admin UI module (screens + AppFlow). Admin-only.
if(TS_BUILD_ADMIN)
  # Ship It: export pipeline (ts_export) used by the Freight Office + CLI.
  add_subdirectory(src/export)
  # Project templates + Trading Post plumbing (JSON, hashes, signatures, fetch).
  # (Monocypher is fetched with the networking block above.)
  add_subdirectory(src/store)
  add_subdirectory(src/editor)
  add_subdirectory(apps/admin)
endif()

if(TS_BUILD_CLIENT)
  add_subdirectory(apps/client)
endif()

if(TS_BUILD_SERVER)
  add_subdirectory(apps/server)
endif()

if(TS_BUILD_GAME)
  add_subdirectory(apps/game)
endif()
