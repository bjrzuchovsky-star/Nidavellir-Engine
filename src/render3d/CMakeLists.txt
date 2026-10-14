# ts_render3d: 3D renderer interfaces (camera, mesh, material) with a null
# backend. No GPU dependencies yet -- 3D projects open with a placeholder.
add_library(ts_render3d
  Renderer3D.cpp
)
target_include_directories(ts_render3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(ts_render3d PUBLIC cxx_std_20)
