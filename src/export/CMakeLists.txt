# Ship It (ts_export): packages a project into a standalone distributable:
# Release runtime (ts_game, optional ts_server), plugin DLLs, game.json,
# cooked game.tspak (ts_vfs), launcher / icon, validation report, zip.
add_library(ts_export
  Exporter.cpp
  Validate.cpp
  Zip.cpp
  WinIcon.cpp
)

target_include_directories(ts_export
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_export
  PUBLIC
    ts_core
    ts_vfs
  PRIVATE
    ts_scene
    ts_plugin
    ts_plugins_builtin
    ts_lua
)

target_compile_features(ts_export PUBLIC cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_export PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
