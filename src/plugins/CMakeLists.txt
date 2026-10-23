# ts_physics_box2d: the Box2D v3 backend of the ts.physics.box2d Trading
# Post plugin (physics2d::Box2DWorld). The only target that links box2d.
add_library(ts_physics_box2d physics_box2d/Box2DWorld.cpp)
target_include_directories(ts_physics_box2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_physics_box2d PUBLIC ts_physics2d PRIVATE box2d)
target_compile_features(ts_physics_box2d PUBLIC cxx_std_20)

# Built-in plugins (statically linked) + the dynamic sample.
add_library(ts_plugins_builtin
  Builtins.cpp
  TumbleweedPlugin.cpp
  RuntimePlugins.cpp
  survival/Tiles.cpp
  survival/WorldGen.cpp
  survival/ChunkMap.cpp
  survival/Data.cpp
  survival/Inventory.cpp
  survival/FrontierSim.cpp
  survival/SurvivalPlugin.cpp
  survival/Sample.cpp
  shipyard/Parts.cpp
  shipyard/Design.cpp
  shipyard/ShipSim.cpp
  shipyard/ShipyardPlugin.cpp
  shipyard/Sample.cpp
  ai/AiPlugin.cpp
  story/StoryPlugins.cpp
  profiler/ProfilerPlugin.cpp
  weather/WeatherPlugin.cpp
  split/SplitPlugins.cpp
  split/Needs.cpp
  movement2d/Riders2D.cpp
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
    ts_runtime
    ts_scene
    ts_ui
    ts_ai
    ts_physics2d
    ts_physics_box2d
  PUBLIC
    ts_weather
    ts_story
    ts_profiler_lua
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
