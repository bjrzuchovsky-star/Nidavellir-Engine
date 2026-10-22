# The Vault (ts_vault library): the universal asset library shared by every
# claim. Content-addressed storage (SHA-256 blobs + SQLite index), the HTTP
# JSON API and its server, the local-folder and remote-server backends, and
# a project's lock file (+ export pruning). Headless: no window, no GL.
add_library(ts_vault_lib
  Sha256.cpp
  Http.cpp
  VaultTypes.cpp
  VaultStore.cpp
  VaultServer.cpp
  VaultBackend.cpp
  VaultLock.cpp
)

target_include_directories(ts_vault_lib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(ts_vault_lib
  PUBLIC
    ts_scene
  PRIVATE
    ts_sqlite
)

target_compile_definitions(ts_vault_lib PRIVATE TS_VAULT_VERSION="${PROJECT_VERSION}")
target_compile_features(ts_vault_lib PUBLIC cxx_std_20)

if(WIN32)
  target_link_libraries(ts_vault_lib PRIVATE ws2_32 winhttp shell32 ole32)
  target_compile_definitions(ts_vault_lib PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(ts_vault_lib PUBLIC Threads::Threads)
endif()
