add_library(ts_platform
  Window.cpp
)

target_include_directories(ts_platform
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_platform
  PUBLIC
    ts_core
)

target_compile_features(ts_platform PUBLIC cxx_std_20)
