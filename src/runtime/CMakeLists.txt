# Game runtime: World (actors, fixed-step simulation, follow camera, draw
# list), PlaySession (play / pause / step / stop) and the sandboxed Lua script
# host. Depends only on the scene data and Lua -- no ImGui, no GL -- so the
# editor, ts_game and a future headless server can all drive it.
add_library(ts_runtime
  World.cpp
  World_Collision.cpp
  World_Anim.cpp
  World_Script.cpp
  World_Plugins.cpp
  World_Net.cpp
  PlaySession.cpp
  Script.cpp
  ScriptApi.cpp
  Sound.cpp
)

target_include_directories(ts_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_runtime
  PUBLIC
    ts_scene
  PRIVATE
    ts_lua
)

target_compile_features(ts_runtime PUBLIC cxx_std_20)
