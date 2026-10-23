# ts_vault: the self-hosted Vault server (headless; see docs/vault.md).
add_executable(ts_vault main.cpp)
ts_win_resources(ts_vault "TombStone Vault asset library server" ICON tombstone_vault.ico)

target_link_libraries(ts_vault PRIVATE ts_vault_lib ts_team)
target_compile_definitions(ts_vault PRIVATE TS_ENGINE_VERSION="${PROJECT_VERSION}")
target_compile_features(ts_vault PRIVATE cxx_std_20)
if(MSVC)
  target_compile_definitions(ts_vault PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
