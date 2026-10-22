# Assimp (BSD-3-Clause) for the universal model importer in ts_render3d:
# OBJ / MTL, FBX, Collada, 3DS, PLY, STL, X, SMD, LWO... Pinned to the
# v5.4.3 release tarball and checked by hash; nothing is committed. Only
# importers are built (no exporters, tools, tests or samples). glTF stays
# on cgltf and .blend goes through Blender, so those two importers are off;
# so is IFC (large, BIM only). Turn the whole thing off with
# -DTS_WITH_ASSIMP=OFF (glTF and Source .mdl keep working).
include(FetchContent)

set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_VIEW OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DRACO OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_BLEND_IMPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_IFC_IMPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_USD_IMPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_M3D_IMPORTER OFF CACHE BOOL "" FORCE)
# Static, so the apps carry it (no DLL to ship). The rest of the tree
# names its library types, so this default does not change anything else.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  ts_assimp
  URL      https://github.com/assimp/assimp/archive/refs/tags/v5.4.3.tar.gz
  URL_HASH SHA256=66dfbaee288f2bc43172440a55d0235dfc7bf885dda6435c038e8000e79582cb
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(ts_assimp)

# Third-party warnings are not ours to fix.
if(MSVC)
  target_compile_options(assimp PRIVATE /W0)
else()
  target_compile_options(assimp PRIVATE -w)
endif()
