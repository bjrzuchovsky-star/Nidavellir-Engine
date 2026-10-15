# Jolt Physics (MIT): 3D rigid bodies, shapes, joints, character controller
# for ts_physics3d. Pinned to the v5.3.0 release tarball and checked by hash
# (nothing is committed). Built as a static library with the cross-platform
# deterministic option on (same inputs -> same simulation on Linux and MSVC)
# and no AVX / FMA, so one binary runs on any x64 machine.
include(FetchContent)
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
foreach(_jph_opt USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD)
  set(${_jph_opt} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(
  ts_jolt
  URL      https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.3.0.tar.gz
  URL_HASH SHA256=e7f9621e480646c434150e1fbe3a9410f4ec4b04ffe54791e0678326b741b918
  SOURCE_SUBDIR Build
)
FetchContent_MakeAvailable(ts_jolt)
