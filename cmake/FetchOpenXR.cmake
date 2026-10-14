# OpenXR loader (Khronos OpenXR-SDK, Apache-2.0) for ts_xr: headsets through
# whatever OpenXR runtime is active (SteamVR, Oculus, WMR, Monado...).
# Pinned release tarball checked by hash; only the loader is built (no
# tests, layers or samples). Windows: static loader (nothing to ship next
# to the exes). Elsewhere: the SDK's default shared loader in the build
# tree. TS_WITH_OPENXR=OFF builds ts_xr with the desktop simulator only.
option(TS_WITH_OPENXR "Build ts_xr against the OpenXR loader (headsets)" ON)
if(NOT TS_WITH_OPENXR)
  return()
endif()

include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
if(WIN32)
  set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
  ts_openxr
  URL      https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.tar.gz
  URL_HASH SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(ts_openxr)

if(TARGET openxr_loader)
  # Third-party code: keep its warnings out of our build logs.
  if(MSVC)
    target_compile_options(openxr_loader PRIVATE /w)
  else()
    target_compile_options(openxr_loader PRIVATE -w)
  endif()
  set(TS_HAVE_OPENXR ON)
else()
  message(WARNING "OpenXR loader target missing; ts_xr builds with the simulator only")
  set(TS_HAVE_OPENXR OFF)
endif()
