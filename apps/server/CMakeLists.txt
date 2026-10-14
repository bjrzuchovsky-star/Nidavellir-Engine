# ts_server: headless authoritative multiplayer server. World at 60 Hz, up
# to four riders over ENet. No window, no GL, no ImGui.
add_executable(ts_server
  main.cpp
)

target_link_libraries(ts_server
  PRIVATE
    ts_core
    ts_scene
    ts_runtime
    ts_net
    ts_mp
    ts_plugin
    ts_plugins_builtin
)

target_compile_features(ts_server PRIVATE cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
