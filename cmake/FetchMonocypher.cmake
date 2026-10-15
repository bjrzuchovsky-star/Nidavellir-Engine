# Monocypher 4.0.2 (CC0 / BSD-2): Ed25519 signature checks for Trading Post
# remote indexes (the optional monocypher-ed25519.c: RFC 8032 Ed25519 with
# SHA-512). The release tarball is downloaded into the build tree and
# checked by hash; nothing from it is committed here.

include(FetchContent)

FetchContent_Declare(
  ts_monocypher
  URL      https://monocypher.org/download/monocypher-4.0.2.tar.gz
  URL_HASH SHA256=38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# No CMakeLists.txt in the tarball: this only populates the source dir.
FetchContent_MakeAvailable(ts_monocypher)
enable_language(C)

set(TS_MONOCYPHER_DIR ${ts_monocypher_SOURCE_DIR}/src)
add_library(ts_monocypher STATIC
  ${TS_MONOCYPHER_DIR}/monocypher.c
  ${TS_MONOCYPHER_DIR}/optional/monocypher-ed25519.c
)
target_include_directories(ts_monocypher PUBLIC ${TS_MONOCYPHER_DIR} ${TS_MONOCYPHER_DIR}/optional)
if(MSVC)
  target_compile_options(ts_monocypher PRIVATE /w)
else()
  target_compile_options(ts_monocypher PRIVATE -w)
endif()
