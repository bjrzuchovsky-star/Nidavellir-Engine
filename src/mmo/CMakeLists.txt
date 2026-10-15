# Town Hall (ts_mmo): the MMO backbone on the posse's Transport / ENet. A
# persistent world server (ts_server --mmo), accounts with Argon2i password
# hashes (Monocypher), SQLite persistence (characters, inventory, positions,
# story state, sessions), zones with interest management, server-side
# movement checks, chat channels, session resume, an admin console, a bot
# load test and the Strongbox bridge. Headless: no window, no GL.
add_library(ts_mmo
  MmoCrypto.cpp
  MmoStore.cpp
  MmoWorld.cpp
  MmoProtocol.cpp
  MmoServer.cpp
  MmoBridge.cpp
  MmoClient.cpp
  MmoLoadTest.cpp
  LocalHost.cpp
)

target_include_directories(ts_mmo
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_mmo
  PUBLIC
    ts_core
    ts_scene
    ts_runtime
    ts_net
    ts_story
    ts_profiler
  PRIVATE
    ts_monocypher
    ts_sqlite
)

target_compile_features(ts_mmo PUBLIC cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_mmo PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(ts_mmo PRIVATE bcrypt)
endif()
