# ts_game: the shipped runtime. Window + fixed-function OpenGL renderer over
# ts_runtime; shares GLFW / stb_image (via ts_gfx) with ts_admin, no ImGui.

# stb_easy_font (single header, public domain / MIT dual license): the
# block letters on the HUD toasts. Same pinned stb commit as stb_image
# (TS_STB_COMMIT), checked by hash; only the header is downloaded.
include(FetchContent)
FetchContent_Declare(
  ts_stb_easy_font
  URL      https://raw.githubusercontent.com/nothings/stb/${TS_STB_COMMIT}/stb_easy_font.h
  URL_HASH SHA256=7b48b52a316477766c0a7a096b33a29bcbd847b6fab4fabd32bb3fdb7cea1a60
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(ts_stb_easy_font)

add_executable(ts_game
  main.cpp
  GameRenderer.cpp
  HudText.cpp
  Game3D.cpp
)

target_include_directories(ts_game PRIVATE ${ts_stb_easy_font_SOURCE_DIR})

target_link_libraries(ts_game
  PRIVATE
    ts_core
    ts_scene
    ts_runtime
    ts_plugins_builtin
    ts_gfx
    ts_render3d
    ts_world3d
    glfw
    OpenGL::GL
)

target_compile_features(ts_game PRIVATE cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_game PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
