# ts_debugtools: the in-game overlay (F3) + console (backtick) over a
# ui::UiSystem, fed by ts_profiler; plus the ts.profiler World3D extension.
add_library(ts_debugtools
  DebugTools.cpp
  ProfilerWorld3D.cpp
)
target_include_directories(ts_debugtools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_debugtools PUBLIC ts_profiler ts_profiler_lua ts_ui ts_world3d PRIVATE ts_lua)
target_compile_features(ts_debugtools PUBLIC cxx_std_20)
