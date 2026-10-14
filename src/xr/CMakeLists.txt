# ts_xr: VR session interface shaped after OpenXR, with a stub runtime.
# No OpenXR SDK dependency yet; VR projects open with a placeholder.
add_library(ts_xr
  XrSession.cpp
)
target_include_directories(ts_xr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_xr PUBLIC ts_render3d)
target_compile_features(ts_xr PUBLIC cxx_std_20)
