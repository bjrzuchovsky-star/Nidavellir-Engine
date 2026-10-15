# ts_weather: the ts.weather simulation (kinds, blending, wind gusts,
# lightning, time of day; weather.json). ts_weather3d: its World3D
# extension (environment, rain / snow / dust billboards, Lua). The 2D side
# lives in the built-in plugin (plugins/weather).
add_library(ts_weather Weather.cpp WeatherLua.cpp)
target_include_directories(ts_weather PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ts_weather PUBLIC ts_scene PRIVATE ts_lua)
target_compile_features(ts_weather PUBLIC cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_weather PRIVATE NOMINMAX)
endif()

# Same condition as src/world3d in the top-level CMakeLists.txt.
if(TS_BUILD_ADMIN OR TS_BUILD_GAME OR TS_BUILD_CLIENT)
  add_library(ts_weather3d Weather3D.cpp)
  target_include_directories(ts_weather3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(ts_weather3d PUBLIC ts_weather ts_world3d PRIVATE ts_lua)
  target_compile_features(ts_weather3d PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_definitions(ts_weather3d PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
endif()
