add_library(ts_platform
  Window.cpp
  SteamService.cpp
)

target_include_directories(ts_platform
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_platform
  PUBLIC
    ts_core
)

target_compile_features(ts_platform PUBLIC cxx_std_20)

if(NIDAVELLIR_STEAM)
  target_compile_definitions(ts_platform PUBLIC NIDAVELLIR_STEAM=1)
  target_include_directories(ts_platform PUBLIC "${STEAMWORKS_SDK_DIR}/public")
  if(WIN32)
    target_link_libraries(ts_platform PUBLIC
      "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
  elseif(APPLE)
    target_link_libraries(ts_platform PUBLIC
      "${STEAMWORKS_SDK_DIR}/redistributable_bin/osx/libsteam_api.dylib")
  else()
    target_link_libraries(ts_platform PUBLIC
      "${STEAMWORKS_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so")
  endif()
endif()
