# Fetch the windowed-app dependencies:
#   GLFW + OpenGL + stb_image   -> ts_admin and ts_game (TS_BUILD_ADMIN or
#                                  TS_BUILD_GAME)
#   Dear ImGui (docking)        -> ts_admin only (TS_BUILD_ADMIN)
# Sources are downloaded into the CMake build tree (_deps); they are NOT
# committed to this repository. Client builds never include this file.

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# Prefer X11 on Linux; Wayland needs wayland-scanner at configure time.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(glfw)

find_package(OpenGL REQUIRED)

# Windows: glfw links the right system libs via its own CMake.
# Linux: needs X11 + OpenGL development packages at configure time.

if(TS_BUILD_ADMIN)
  # Docking branch tip kept lean via shallow clone. Pin a v*-docking tag later
  # if you need bit-identical dependency trees across machines.
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
  )

  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()

  # imgui has no library target of its own; build core + GLFW/OpenGL3 backends.
  # imgui_impl_opengl3 embeds a small GL loader (no separate glad required).
  add_library(ts_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    # Item hooks for the editor's Field Guide tooltips + --smoke-ux menu audit.
    ${PROJECT_SOURCE_DIR}/src/editor/ux/ImGuiItemHook.cpp
  )

  add_library(ts::imgui ALIAS ts_imgui)

  target_include_directories(ts_imgui
    PUBLIC
      ${imgui_SOURCE_DIR}
      ${imgui_SOURCE_DIR}/backends
    PRIVATE
      ${PROJECT_SOURCE_DIR}/src
  )

  target_link_libraries(ts_imgui
    PUBLIC
      glfw
      OpenGL::GL
  )

  target_compile_features(ts_imgui PUBLIC cxx_std_20)
  target_compile_definitions(ts_imgui
    PUBLIC
      TS_HAS_IMGUI=1
      # Item hooks only (no test engine is linked): see src/editor/ux/ImGuiItemHook.h.
      IMGUI_ENABLE_TEST_ENGINE
  )
endif()  # TS_BUILD_ADMIN

# stb_image (single header, public domain / MIT dual license) for sprite +
# tileset loading in the editor and ts_game. Pinned to one commit and
# checked by hash; only the header is downloaded (nothing is committed).
set(TS_STB_COMMIT 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_Declare(
  ts_stb_image
  URL      https://raw.githubusercontent.com/nothings/stb/${TS_STB_COMMIT}/stb_image.h
  URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
  DOWNLOAD_NO_EXTRACT TRUE
)
# No CMakeLists.txt in the download, so this only populates the source dir.
FetchContent_MakeAvailable(ts_stb_image)

# Header-only interface target; the implementation is compiled once in
# src/gfx/StbImageImpl.cpp.
add_library(ts_stb_image INTERFACE)
target_include_directories(ts_stb_image INTERFACE ${ts_stb_image_SOURCE_DIR})
