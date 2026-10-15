# ts_story: the story pieces the optional ts.story / ts.quests / ts.dialogue /
# ts.saves plugins share -- variables + conditions + actions, quest log,
# dialogue runner, save slots, the runtime that ties them to a world's UI and
# scripts, and its Lua tables. World-agnostic (2D plugin + 3D extension).
add_library(ts_story
  Vars.cpp
  Quest.cpp
  Dialogue.cpp
  SaveGame.cpp
  StoryRuntime.cpp
  StoryLua.cpp
)
target_include_directories(ts_story PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_story PUBLIC ts_scene ts_runtime PRIVATE ts_lua)
target_compile_features(ts_story PUBLIC cxx_std_20)
