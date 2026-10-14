# Multiplayer game layer: wire protocol, the authoritative GameServer, the
# predicting / interpolating GameClient and the in-process LocalPosse. Built
# on ts_runtime (the World) and ts_net (Transport + ENet); no window, no GL,
# so ts_server stays headless.
add_library(ts_mp
  Protocol.cpp
  GameServer.cpp
  GameClient.cpp
  LocalPosse.cpp
  PosseSmoke.cpp
)

target_include_directories(ts_mp
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_mp
  PUBLIC
    ts_core
    ts_scene
    ts_runtime
    ts_net
)

target_compile_features(ts_mp PUBLIC cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_mp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
