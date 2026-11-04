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

## Clans

With `-DNIDAVELLIR_STEAM=ON` and Steam running, Team > Join clan... can host a friends-only hall (tick **Invite only** for a private one). The session still uses the same locks, presence and asset sync; the wire is Steam's relay (`ISteamNetworkingMessages`), not a forwarded UDP port. If Steam is not running, that button is absent and the address fields work as before.

The in-editor friend list is friends currently in app 3326740 (Steam does not tell the game which offline friends own it). **Invite overlay** is Steam's own dialog, which only offers friends who own the app.

`+connect_lobby <id>` on the exe command line, or accepting an overlay invite, joins that clan once a realm is open.

## Test with two Steam accounts

1. Install the Steam client twice is not required: one account on this PC, the second on another PC or a second Windows user. Both must own app 3326740 and have `STEAMWORKS_SDK_DIR` pointing at `D:\Dev\SteamworksSDK`.
2. Configure and build: `cmake -S . -B build -DNIDAVELLIR_STEAM=ON` then build `ts_admin` Release.
3. Start account A normally. Start account B with Steam's "start another account" or a second machine. Launch `ts_admin.exe` from each (the editor writes `steam_appid.txt` beside itself if it is missing).
4. On A, open a realm, Team > Join clan..., leave **Invite only** unticked, **Host clan**.
5. On B, open a realm with the same editor version. Team > Join clan... lists A's clan if A is in the app. **Join**. B should see A's persona. Realm edits, locks and presence match an address session.
6. On A, **Invite** next to B, or **Invite overlay**, and accept on B if you did not join from the list.
7. Close A. B should become host (the lobby is handed on). If A was alone, the clan closes.
8. Kick: A clicks **Kick** on B. B should see "The host asked you to leave this clan."
9. Version: join from an editor whose `ts_admin --version` differs. The panel should say which version forged the clan and which one this editor is.

`ts_admin --smoke-clan` checks the same rules with an in-memory stand-in and does not need the SDK.
