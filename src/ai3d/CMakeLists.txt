# ts_ai3d: ts.ai in 3D worlds (World3D extension: Recast navmesh from the
# scene's static colliders, Detour crowd agents, behaviour trees, perception,
# the Lua `ai` table, debug lines).
add_library(ts_ai3d AiWorld3D.cpp)
target_include_directories(ts_ai3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_ai3d PUBLIC ts_ai ts_world3d PRIVATE ts_lua)
target_compile_features(ts_ai3d PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_ai3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
