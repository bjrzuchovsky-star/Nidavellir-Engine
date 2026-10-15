# Game runtime: World (actors, fixed-step simulation, follow camera, draw
# list), PlaySession (play / pause / step / stop) and the sandboxed Lua script
# host. Depends only on the scene data and Lua -- no ImGui, no GL -- so the
# editor, ts_game and a future headless server can all drive it.
add_library(ts_runtime
  World.cpp
  World_Collision.cpp
  World_Physics2D.cpp
  World_PhysicsLua.cpp
  World_Anim.cpp
  World_Script.cpp
  World_Plugins.cpp
  World_Fx.cpp
  World_Net.cpp
  PlaySession.cpp
  Script.cpp
  ScriptApi.cpp
  ScriptHost.cpp
  ScriptLanguages.cpp
  GraphScript.cpp
  ScriptAbi.cpp
  PluginBridge.cpp
  CompiledScript.cpp
  CSharpScript.cpp
  NativeScript.cpp
  Sound.cpp
)

target_include_directories(ts_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/sdk/include
)

target_link_libraries(ts_runtime
  PUBLIC
    ts_scene
    ts_fx
    ts_physics2d
    ts_profiler
  PRIVATE
    ts_lua
    ${CMAKE_DL_LIBS}
)

# C++ scripts: the module build runs this cmake against the bundled SDK.
target_compile_definitions(ts_runtime PRIVATE
  TS_CMAKE_COMMAND="${CMAKE_COMMAND}"
  TS_SCRIPT_SDK_DIR="${PROJECT_SOURCE_DIR}/sdk/include"
  TS_DOTNET_SDK_DIR="${PROJECT_SOURCE_DIR}/sdk/dotnet")

target_compile_features(ts_runtime PUBLIC cxx_std_20)
