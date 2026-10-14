# cgltf (single header, MIT): glTF 2.0 parsing for ts_render3d. Pinned to
# the v1.14 tag and checked by hash; only the header is downloaded (nothing
# is committed). The implementation is compiled once in
# src/render3d/Gltf.cpp.
include(FetchContent)
FetchContent_Declare(
  ts_cgltf
  URL      https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.14/cgltf.h
  URL_HASH SHA256=19db3bdf85f6aa991a3b9d056641bcb39a2ef11cd5211e7a9f6b6c307d9ebc64
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(ts_cgltf)

add_library(ts_cgltf INTERFACE)
target_include_directories(ts_cgltf INTERFACE ${ts_cgltf_SOURCE_DIR})
