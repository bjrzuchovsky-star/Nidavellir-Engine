# Box2D v3 (MIT, C17): 2D rigid bodies, rotated shapes and joints for
# ts_physics2d. Pinned to the v3.1.1 release tarball and checked by hash.
# Box2D v3 is cross-platform deterministic; AVX2 stays off so one build
# runs on any x64 machine. Samples / tests are not built.
include(FetchContent)
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
set(BOX2D_PROFILE OFF CACHE BOOL "" FORCE)
set(BOX2D_AVX2 OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ts_box2d
  URL      https://github.com/erincatto/box2d/archive/refs/tags/v3.1.1.tar.gz
  URL_HASH SHA256=fb6ef914b50f4312d7d921a600eabc12318bb3c55a0b8c0b90608fa4488ef2e4
)
FetchContent_MakeAvailable(ts_box2d)
