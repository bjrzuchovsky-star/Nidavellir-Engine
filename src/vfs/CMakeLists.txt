# Virtual filesystem (ts_vfs): the .tspak archive (indexed, hashed) plus a
# mount table with loose-file fallback. Pure C++17 filesystem, no deps.
add_library(ts_vfs
  Pak.cpp
  Vfs.cpp
)

target_include_directories(ts_vfs
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(ts_vfs PUBLIC cxx_std_20)
