# ts_client: the multiplayer client. Connects to a ts_server, predicts its
# own rider, interpolates the rest, and draws the world with the same
# fixed-function renderer and HUD as ts_game (shared sources, ts_gfx
# textures). GLFW + stb only; no ImGui.

include(FetchContent)
FetchContent_Declare(
  ts_stb_easy_font
  URL      https://raw.githubusercontent.com/nothings/stb/${TS_STB_COMMIT}/stb_easy_font.h
  URL_HASH SHA256=7b48b52a316477766c0a7a096b33a29bcbd847b6fab4fabd32bb3fdb7cea1a60
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(ts_stb_easy_font)

set(TS_GAME_APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../game)

add_executable(ts_client
  main.cpp
  ${TS_GAME_APP_DIR}/GameRenderer.cpp
  ${TS_GAME_APP_DIR}/HudText.cpp
  ${TS_GAME_APP_DIR}/GameUi.cpp
)
ts_win_resources(ts_client "TombStone multiplayer client")


target_include_directories(ts_client PRIVATE
  ${TS_GAME_APP_DIR}
  ${ts_stb_easy_font_SOURCE_DIR}
)

target_link_libraries(ts_client
  PRIVATE
    ts_core
    ts_scene
    ts_runtime
    ts_net
    ts_mp
    ts_debugtools
    ts_plugin
    ts_plugins_builtin
    ts_gfx
    ts_audio
    ts_ui
    glfw
    OpenGL::GL
)

target_compile_features(ts_client PRIVATE cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
