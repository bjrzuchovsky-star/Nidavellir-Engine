# ts_store: data-driven bits shared by the project templates and the
# Trading Post (plugin store): a nested JSON value, SHA-256, Ed25519
# signature checks (Monocypher, fetched by hash), URL fetching (file://,
# http:// and, on Windows, https:// through WinHTTP) and the plugin catalog
# with its install cache.
add_library(ts_store
  Json.cpp
  Crypto.cpp
  Fetch.cpp
  TradingPost.cpp
)

target_include_directories(ts_store
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_store
  PUBLIC
    ts_plugin
  PRIVATE
    ts_monocypher
)
if(WIN32)
  target_link_libraries(ts_store PRIVATE winhttp)
endif()

target_compile_features(ts_store PUBLIC cxx_std_20)

if(MSVC)
  target_compile_definitions(ts_store PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
