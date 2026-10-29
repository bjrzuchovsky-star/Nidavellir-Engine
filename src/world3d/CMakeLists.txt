# ts_world3d: the 3D play runtime (rider controller, capsule-vs-box
# collision, triggers, follow camera) with Lua scripts through the shared
# ScriptHost, and the VR mode (rig, locomotion, hands, grab / throw,
# comfort menu) fed by ts_xr. No GL; drawn by ts_render3d's SceneView.
add_library(ts_world3d
  World3D.cpp
  World3D_Lua.cpp
  World3D_ScriptApi.cpp
  World3D_VR.cpp
  World3D_Anim.cpp
  World3D_Physics.cpp
  World3D_PhysicsLua.cpp
  World3D_Camera.cpp
  World3D_Terrain.cpp
  VrOverlay.cpp
)
target_include_directories(ts_world3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_world3d
  PUBLIC
    ts_render3d
    ts_runtime
    ts_physics3d
  PRIVATE
    ts_lua
)
target_compile_features(ts_world3d PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_world3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Sound for World3D rides (lives with the audio code, built here because it
# needs both ts_audio and ts_world3d).
add_library(ts_audio3d ${CMAKE_CURRENT_SOURCE_DIR}/../audio/SoundStage3D.cpp)
target_link_libraries(ts_audio3d PUBLIC ts_audio ts_world3d)
target_compile_features(ts_audio3d PUBLIC cxx_std_20)
