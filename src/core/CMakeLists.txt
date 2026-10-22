add_library(ts_core
  AppPaths.cpp
  Engine.cpp
  MemImages.cpp
  Localization.cpp
  PngWrite.cpp
)

target_include_directories(ts_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(ts_core PUBLIC cxx_std_20)
