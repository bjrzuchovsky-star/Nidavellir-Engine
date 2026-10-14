add_executable(ts_admin
  main.cpp
  SmokeRuntime.cpp
  SmokeCollision.cpp
  SmokeAnimation.cpp
  SmokeAnimEditor.cpp
  SmokeScripting.cpp
  SmokeScriptRuntime.cpp
  SmokeScriptEditor.cpp
  SmokeNetwork.cpp
  SmokePlugins.cpp
  Smoke3D.cpp
  Smoke3DEditor.cpp
)

target_link_libraries(ts_admin
  PRIVATE
    ts_core
    ts_platform
    ts_render
    ts_input
    ts_net
    ts_mp
    ts_physics
    ts_scene
    ts_runtime
    ts_gfx
    ts_editor
    ts_plugin
    ts_plugins_builtin
    ts_render3d
    ts_world3d
    ts_xr
    ts_imgui
)

target_compile_features(ts_admin PRIVATE cxx_std_20)

# --smoke loads the sample dynamic plugin from the build tree.
target_compile_definitions(ts_admin PRIVATE
  TS_SAMPLE_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")
add_dependencies(ts_admin ts_plugin_lantern)

# MSVC: enable UTF-8 source and avoid min/max macros interfering with ImGui.
if(MSVC)
  target_compile_options(ts_admin PRIVATE /utf-8)
  target_compile_definitions(ts_admin PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
