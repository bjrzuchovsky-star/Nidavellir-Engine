# Scene data shared by the editor, the runtime and ts_game: entities and
# their components, tile codec, scene.json reader/writer. Pure C++ (no UI,
# no graphics API), so headless tools and servers can link it too.
add_library(ts_scene
  Animation.cpp
  CameraMigrate.cpp
  AutoTile.cpp
  SpriteSlice.cpp
  JsonFlat.cpp
  JsonTree.cpp
  Prefab.cpp
  SampleRider.cpp
  SampleScripts.cpp
  Scene.cpp
  SceneData.cpp
  SceneJson.cpp
  TileMap.cpp
)

target_include_directories(ts_scene
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_scene
  PUBLIC
    ts_core
    ts_fx
)

target_compile_features(ts_scene PUBLIC cxx_std_20)
