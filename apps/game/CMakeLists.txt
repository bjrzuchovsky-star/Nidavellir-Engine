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
  GameUi.cpp
  Game3D.cpp
  GameVR.cpp
  GameManifest.cpp
  GpuCheck.cpp
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
    ts_ai3d
    ts_story3d
    ts_audio
    ts_audio3d
    ts_xr
    ts_ui
    ts_vfs
    ts_stb_image
    glfw
    OpenGL::GL
)

target_compile_features(ts_game PRIVATE cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_game PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # Version resource (the exporter stamps each game's icon into its copy).
  configure_file(ts_game.rc.in ${CMAKE_CURRENT_BINARY_DIR}/ts_game.rc @ONLY)
  target_sources(ts_game PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ts_game.rc)
endif()

# App-local VC++ runtime: copy vcruntime140*.dll / msvcp140*.dll next to
# ts_game so exports (which take every DLL beside the runtime) run on PCs
# without the VC++ Redistributable. UCRT is part of Windows 10+.
if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
  set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
  include(InstallRequiredSystemLibraries)
  set(TS_APP_LOCAL_CRT "")
  foreach(lib IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    get_filename_component(lib_name "${lib}" NAME)
    if(lib_name MATCHES "^(vcruntime140|vcruntime140_1|msvcp140|msvcp140_1|msvcp140_2|msvcp140_atomic_wait|concrt140)\\.dll$")
      list(APPEND TS_APP_LOCAL_CRT "${lib}")
    endif()
  endforeach()
  if(TS_APP_LOCAL_CRT)
    add_custom_command(TARGET ts_game POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TS_APP_LOCAL_CRT} $<TARGET_FILE_DIR:ts_game>
      COMMENT "App-local VC++ runtime next to ts_game"
      VERBATIM)
  else()
    message(WARNING "VC++ runtime DLLs not found; exported games will need the VC++ Redistributable")
  endif()
endif()
