# Live Team Claims (docs/team-claims.md): the shared scene document and its
# ops, the session server (hosted by ts_vault or an editor) and the client.
# Headless: no window, no GL; the editor bindings live in src/editor/team.
add_library(ts_team
  TeamDoc.cpp
  TeamServer.cpp
  TeamClient.cpp
  TeamRpc.cpp
)

target_include_directories(ts_team
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_team
  PUBLIC
    ts_scene
    ts_net
)

target_compile_features(ts_team PUBLIC cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_team PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
