# ts_plugin: plugin interface, catalog (static + dynamic .dll/.so), and the
# manager that resolves dependencies and collects contributions. Depends on
# ts_runtime only for the WorldExtensions / log types it hands out.
add_library(ts_plugin
  Catalog.cpp
  DynamicLibrary.cpp
  PluginManager.cpp
)

target_include_directories(ts_plugin
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_plugin
  PUBLIC
    ts_runtime
  PRIVATE
    ${CMAKE_DL_LIBS}
)

target_compile_features(ts_plugin PUBLIC cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_plugin PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
