cmake_minimum_required(VERSION 3.20)
project(TombStoneEngine VERSION 0.1.0 LANGUAGES CXX)

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

# Windowed apps: GLFW + OpenGL + stb_image for ts_admin, ts_game and
# ts_client (ts_gfx: texture cache + GL helpers). Dear ImGui is fetched only
# for the Admin (see FetchImGuiDeps). ts_server needs none of it.
if(TS_BUILD_ADMIN OR TS_BUILD_GAME OR TS_BUILD_CLIENT)
  include(FetchImGuiDeps)
  add_subdirectory(src/gfx)
  # 3D: renderer (GL 3.3 core + null), glTF via cgltf (fetched by hash),
  # scene.json v7; VR: OpenXR-shaped session stub.
  include(FetchCgltf)
  add_subdirectory(src/render3d)
  add_subdirectory(src/xr)
endif()

# Game runtime (World, fixed-step PlaySession, Lua gameplay scripts). Pure
# C++ with no window or GL; built for every app that runs the game (the
# headless server included). Lua 5.4 is fetched by hash.
if(TS_BUILD_ADMIN OR TS_BUILD_GAME OR TS_BUILD_CLIENT OR TS_BUILD_SERVER)
  include(FetchLua)
  add_subdirectory(src/runtime)
  # Plugins: interface, catalog, manager (ts_plugin) and the built-ins.
  add_subdirectory(src/plugin)
  add_subdirectory(src/plugins)
endif()

# Networking (ts_net: Transport interface + ENet backend). ENet is fetched by
# hash, only for the apps that talk over the wire.
if(TS_BUILD_ADMIN OR TS_BUILD_CLIENT OR TS_BUILD_SERVER)
  include(FetchENet)
  add_subdirectory(src/net)
  # Multiplayer game layer (protocol, GameServer, GameClient, LocalPosse).
  add_subdirectory(src/mp)
endif()

# Editor / Admin UI module (screens + AppFlow). Admin-only.
if(TS_BUILD_ADMIN)
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
