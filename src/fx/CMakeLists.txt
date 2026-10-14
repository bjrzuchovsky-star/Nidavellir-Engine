# Effects (ts_fx): particle emitters + presets and the 2D lightmap. Pure
# C++ (no GL): hosts draw the sprites / lightmap.
add_library(ts_fx
  Particles.cpp
  Light2D.cpp
)

target_include_directories(ts_fx
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_fx
  PUBLIC
    ts_core
)

target_compile_features(ts_fx PUBLIC cxx_std_20)
