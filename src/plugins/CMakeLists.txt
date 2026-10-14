# Built-in plugins (statically linked) + the dynamic sample.
add_library(ts_plugins_builtin
  Builtins.cpp
  TumbleweedPlugin.cpp
  RuntimePlugins.cpp
)

target_include_directories(ts_plugins_builtin
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_plugins_builtin
  PUBLIC
    ts_plugin
  PRIVATE
    ts_core
    ts_lua
)

target_compile_features(ts_plugins_builtin PUBLIC cxx_std_20)

# Sample dynamic plugin, emitted to <build>/plugins (no per-config subfolder
# on multi-config generators thanks to the $<1:...> wrapper).
set(TS_SAMPLE_PLUGIN_DIR "${CMAKE_BINARY_DIR}/plugins")
add_library(ts_plugin_lantern MODULE lantern/LanternPlugin.cpp)
target_include_directories(ts_plugin_lantern PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(ts_plugin_lantern PRIVATE cxx_std_20)
set_target_properties(ts_plugin_lantern PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY "$<1:${TS_SAMPLE_PLUGIN_DIR}>"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${TS_SAMPLE_PLUGIN_DIR}>"
  CXX_VISIBILITY_PRESET hidden
)
# Built with the host so the scan finds it; the smoke loads it.
add_dependencies(ts_plugins_builtin ts_plugin_lantern)
target_compile_definitions(ts_plugins_builtin PRIVATE
  TS_SAMPLE_PLUGIN_DIR="${TS_SAMPLE_PLUGIN_DIR}")
