# Steam upload (App 3326740)

Nidavellir Engine is a **Software** app on Steam. Depot **3326741** is the Windows build. The Steamworks SDK is licensed and is **not** in this repo. On the build PC it lives at `D:\Dev\SteamworksSDK` (SDK 1.65) and `STEAMWORKS_SDK_DIR` is that path.

## Build with Steam

```bat
cmake -S . -B build -DNIDAVELLIR_STEAM=ON
cmake --build build --config Release
```

`STEAMWORKS_SDK_DIR` is read from the environment when you configure. The option is **OFF** by default, so a normal build does not need the SDK and the existing smokes stay the same.

When the option is on, the editor calls `SteamAPI_RestartAppIfNecessary(3326740)` and `SteamAPI_Init`, links `steam_api64`, and copies `redistributable_bin\win64\steam_api64.dll` next to `ts_admin.exe`. The Steam persona is added to the window title. If `steam_appid.txt` is missing beside the exe, the editor writes `3326740` there so a dev run outside the Steam client can still boot. Do not ship that file: the depot excludes it.

## Upload to the beta branch

1. Package a release (`tools\release\build_release.ps1`) and copy the staged folder's contents into `tools\steam\content\` (the folder that contains `ts_admin.exe`). `content/` and `output/` are gitignored.
2. From `tools\steam`, run ContentBuilder's steamcmd. It asks for the password and a Steam Guard code. Nothing is stored in the repo.

```bat
"%STEAMWORKS_SDK_DIR%\tools\ContentBuilder\builder\steamcmd.exe" +login <steam-user> +run_app_build "%CD%\app_build_3326740.vdf" +quit
```

`app_build_3326740.vdf` sets `setlive` to `beta`, so the build goes to the **beta** branch and not the public default. Change that string if the branch has another name. Leave it blank and set the build live on the partner site if you would rather choose there.

## Clans over Steam (not this change)

Friends, invites and lobbies should replace the IP "Join clan" box. `SteamService` is where the Steam API is already up. The menu hook is `editor/team/TeamPanel.cpp` ("Join clan...").
