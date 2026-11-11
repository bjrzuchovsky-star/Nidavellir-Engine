# TombStone Engine build options

option(TS_BUILD_ADMIN  "Build the Admin (editor/tools) application" ON)
option(TS_BUILD_CLIENT "Build the Client (developer runtime) application" ON)
option(TS_BUILD_GAME   "Build the Game (shipped runtime) application" ON)
option(TS_BUILD_SERVER "Build the headless multiplayer Server (ts_server)" ON)
option(TS_BUILD_VAULT  "Build the Vault asset library server (ts_vault)" ON)
option(TS_WITH_ASSIMP  "Universal model import (OBJ / FBX / DAE / 3DS / PLY / STL...) via Assimp" ON)

# Steamworks SDK 1.65 is licensed and is not in this repo. OFF by default,
# so a normal build does not need the SDK. STEAMWORKS_SDK_DIR defaults to
# the environment variable of the same name (D:\Dev\SteamworksSDK on the
# Windows build PC).
option(NIDAVELLIR_STEAM "Link Steamworks and boot app 3326740" OFF)
set(STEAMWORKS_SDK_DIR "$ENV{STEAMWORKS_SDK_DIR}" CACHE PATH
    "Steamworks SDK 1.65 root (public/steam/steam_api.h). Not committed.")
if(NIDAVELLIR_STEAM AND NOT EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
  message(FATAL_ERROR
    "NIDAVELLIR_STEAM is ON but STEAMWORKS_SDK_DIR (${STEAMWORKS_SDK_DIR}) "
    "has no public/steam/steam_api.h. Point it at Steamworks SDK 1.65.")
endif()

message(STATUS "TombStoneEngine options:")
message(STATUS "  TS_BUILD_ADMIN  = ${TS_BUILD_ADMIN}")
message(STATUS "  TS_BUILD_CLIENT = ${TS_BUILD_CLIENT}")
message(STATUS "  TS_BUILD_GAME   = ${TS_BUILD_GAME}")
message(STATUS "  TS_BUILD_SERVER = ${TS_BUILD_SERVER}")
message(STATUS "  TS_BUILD_VAULT  = ${TS_BUILD_VAULT}")
message(STATUS "  TS_WITH_ASSIMP  = ${TS_WITH_ASSIMP}")
message(STATUS "  NIDAVELLIR_STEAM = ${NIDAVELLIR_STEAM}")

# Copy steam_api64.dll next to an executable. No-op unless NIDAVELLIR_STEAM.
function(nidavellir_attach_steam target)
  if(NOT NIDAVELLIR_STEAM OR NOT WIN32)
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll"
            "$<TARGET_FILE_DIR:${target}>/steam_api64.dll"
    COMMENT "Copy steam_api64.dll next to ${target}")
endfunction()
if(TS_BUILD_ADMIN)
  message(STATUS "  Admin will FetchContent GLFW + Dear ImGui (docking) + stb_image")
endif()
if(TS_BUILD_GAME)
  message(STATUS "  Game will FetchContent GLFW + stb_image (no ImGui)")
endif()
if(TS_BUILD_CLIENT)
  message(STATUS "  Client will FetchContent GLFW + stb_image + ENet (no ImGui)")
endif()
if(TS_BUILD_SERVER)
  message(STATUS "  Server is headless: Lua + ENet only (no window, no GL)")
endif()
