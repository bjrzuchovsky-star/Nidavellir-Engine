# ts_story3d: the story plugins in World3D (extension: runtime, Lua tables,
# UI tap, rider position for saves).
add_library(ts_story3d StoryWorld3D.cpp)
target_include_directories(ts_story3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_story3d PUBLIC ts_story ts_world3d PRIVATE ts_lua)
target_compile_features(ts_story3d PUBLIC cxx_std_20)
