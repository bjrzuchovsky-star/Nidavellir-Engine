# ts_physics_jolt: the Jolt Physics backend of the ts.physics.jolt Trading
# Post plugin (physics3d::JoltWorld). The only target that links Jolt.
add_library(ts_physics_jolt JoltWorld.cpp)
target_include_directories(ts_physics_jolt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(ts_physics_jolt PUBLIC ts_physics3d PRIVATE Jolt)
target_compile_features(ts_physics_jolt PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_physics_jolt PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
