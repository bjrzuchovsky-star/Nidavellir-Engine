# ts_profiler: Assay Office core -- frame profiler (named CPU scopes, counters,
# GPU timings, captures) and the debug console command registry. No GL, no UI.
add_library(ts_profiler
  Profiler.cpp
  Console.cpp
)
target_include_directories(ts_profiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_profiler PRIVATE ts_scene)
if(WIN32)
  target_link_libraries(ts_profiler PRIVATE psapi)
endif()
target_compile_features(ts_profiler PUBLIC cxx_std_20)


# ts_profiler_lua: the `profiler` / `console` Lua tables (ts.profiler plugin,
# 3D extension). Separate so ts_profiler itself needs no Lua.
add_library(ts_profiler_lua ProfilerLua.cpp)
target_link_libraries(ts_profiler_lua PUBLIC ts_profiler PRIVATE ts_lua)
target_compile_features(ts_profiler_lua PUBLIC cxx_std_20)
