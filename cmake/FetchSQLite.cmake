# SQLite 3.46.1 (public domain) amalgamation: the MMO persistence layer
# (ts_mmo: accounts, characters, inventory, sessions). Downloaded into the
# build tree and checked by hash; nothing from it is committed here.

include(FetchContent)

FetchContent_Declare(
  ts_sqlite
  URL      https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
  URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(ts_sqlite)
enable_language(C)

add_library(ts_sqlite STATIC ${ts_sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(ts_sqlite PUBLIC ${ts_sqlite_SOURCE_DIR})
# One connection per thread in ts_mmo; no extension loading, no deprecated APIs.
target_compile_definitions(ts_sqlite PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
if(MSVC)
  target_compile_options(ts_sqlite PRIVATE /w)
else()
  target_compile_options(ts_sqlite PRIVATE -w)
  find_package(Threads REQUIRED)
  target_link_libraries(ts_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
