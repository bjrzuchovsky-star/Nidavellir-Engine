# ts_plugins_3d: the 3D halves of the split Trading Post plugins -- the
# Rider 3D controller (ts.move.rider3d) and the Jolt backend offer -- for
# the hosts that run World3D (ts_admin, ts_game). offer_3d_providers()
# makes them available; the plugins switch them on per claim.
add_library(ts_plugins_3d Rider3D.cpp Providers3D.cpp)
target_include_directories(ts_plugins_3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(ts_plugins_3d PUBLIC ts_world3d ts_physics_jolt)
target_compile_features(ts_plugins_3d PUBLIC cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_plugins_3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
