# ts_physics3d: the core 3D physics module (Jolt underneath, never in a
# public header). Used by ts_world3d for statics, rigid bodies, triggers,
# joints, grabbing and the rider's character controller.
add_library(ts_physics3d Physics3D.cpp)
target_include_directories(ts_physics3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_physics3d PRIVATE Jolt)
target_compile_features(ts_physics3d PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_physics3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
