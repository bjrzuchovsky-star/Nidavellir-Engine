# Recast & Detour 1.6.0 (zlib licence): navmesh generation, path queries and
# crowd steering for the AI toolkit (ts_ai). The release tarball is fetched
# by hash into the build tree; only Recast, Detour and DetourCrowd are
# compiled (no demo, no tests). Nothing from it is committed here.

include(FetchContent)

FetchContent_Declare(
  ts_recast
  URL      https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz
  URL_HASH SHA256=d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  # Populate only: the tarball's own CMakeLists builds demos and tests.
  SOURCE_SUBDIR _ts_no_cmake
)
FetchContent_MakeAvailable(ts_recast)

set(TS_RECAST_DIR ${ts_recast_SOURCE_DIR})
file(GLOB TS_RECAST_SOURCES
  ${TS_RECAST_DIR}/Recast/Source/*.cpp
  ${TS_RECAST_DIR}/Detour/Source/*.cpp
  ${TS_RECAST_DIR}/DetourCrowd/Source/*.cpp
)
add_library(ts_recast STATIC ${TS_RECAST_SOURCES})
target_include_directories(ts_recast PUBLIC
  ${TS_RECAST_DIR}/Recast/Include
  ${TS_RECAST_DIR}/Detour/Include
  ${TS_RECAST_DIR}/DetourCrowd/Include
)
set_target_properties(ts_recast PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(ts_recast PRIVATE /w)
else()
  target_compile_options(ts_recast PRIVATE -w)
endif()
