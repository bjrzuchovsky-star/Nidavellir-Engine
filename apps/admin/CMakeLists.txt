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
  SmokeSurvival.cpp
  SmokeShipyard.cpp
  SmokeAudio.cpp
  Smoke3D.cpp
  Smoke3DEditor.cpp
  SmokeAnim3D.cpp
  SmokeAI.cpp
  SmokeAIWorld.cpp
  SmokeAIMenu.cpp
  SmokeVR.cpp
  SmokeTools.cpp
  SmokeUi.cpp
  SmokeFx.cpp
  ExportCli.cpp
  SmokeGraph.cpp
  SmokeNative.cpp
  SmokeTemplates.cpp
  SmokeTradingPost.cpp
  SmokeCSharp.cpp
  SmokePhysics.cpp
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
    ts_store
    ts_plugin
    ts_plugins_builtin
    ts_ai
    ts_ai3d
    ts_lua
    ts_render3d
    ts_world3d
    ts_physics3d
    ts_xr
    ts_imgui
    ts_audio
    ts_audio3d
    ts_ui
    ts_fx
    ts_export
    ts_vfs
)

target_compile_features(ts_admin PRIVATE cxx_std_20)

# --smoke loads the sample dynamic plugin from the build tree.
target_compile_definitions(ts_admin PRIVATE
  TS_SAMPLE_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")
add_dependencies(ts_admin ts_plugin_lantern)

# Ship the engine templates next to ts_admin (<exe dir>/templates); runs
# every build so edited templates are picked up.
add_custom_target(ts_admin_templates ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${PROJECT_SOURCE_DIR}/templates" "$<TARGET_FILE_DIR:ts_admin>/templates"
  COMMENT "Copying engine templates next to ts_admin")
add_dependencies(ts_admin_templates ts_admin)

# MSVC: enable UTF-8 source and avoid min/max macros interfering with ImGui.
if(MSVC)
  target_compile_options(ts_admin PRIVATE /utf-8)
  target_compile_definitions(ts_admin PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
