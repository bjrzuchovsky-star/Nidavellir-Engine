# Releasing the engine (Windows)

This page is about installing the TombStone Engine itself (the editor, the runtime and the tools). To ship a game made with it, see [distribution](distribution.md).

## Version

`VERSION` at the repo root holds the one version number, `MAJOR.MINOR.PATCH`. CMake reads it (`project(... VERSION)`), `ts_admin --version` prints it, and the installer and zip are named after it. [CHANGELOG.md](../CHANGELOG.md) lists what each version holds.

## Building a release

In a Developer PowerShell for VS 2022, run:

```powershell
tools\release\build_release.ps1 -Out D:\Dev\TombStoneReleases\0.1.0 [-BuildDir build] [-SkipBuild] [-SkipSmoke] [-TestInstall]
```

The script does the following, in order:

1. Configures (if needed) and builds Release with VS 2022 x64, then checks that `ts_admin --version` matches `VERSION`.
2. Runs `ts_admin --smoke` with `TS_AUDIO=null`. The log goes to `<out>\smoke.log`. A failure stops the release.
3. Stages `<out>\stage\TombStone-<version>-win64\`:

   | Path | Contents |
   |---|---|
   | `ts_admin.exe`, `ts_game.exe`, `ts_server.exe`, `ts_client.exe` | the apps, side by side, so the editor finds the runtime and the server |
   | `vcruntime140*.dll`, `msvcp140*.dll`, `concrt140.dll` | app-local C++ runtime (no VC++ Redistributable needed) |
   | `plugins\` | dynamic plugin DLLs (the sample `ts_plugin_lantern`) |
   | `templates\` | project templates for the project picker |
   | `samples\<id>\` | every template's project, ready to open |
   | `fonts\` | Rye and Arvo with their SIL OFL licences (the apps also embed them) |
   | `docs\` | this documentation |
   | `sdk\include`, `sdk\dotnet` | C++ and C# script SDKs |
   | `licenses\` | third-party licences of the fetched dependencies |
   | `LICENSE`, `README.md`, `CHANGELOG.md`, `VERSION` | |

4. Writes `TombStone-<version>-win64.zip` (unzip and run `ts_admin.exe`), and `TombStone-<version>-setup.exe` from `tools\release\tombstone.iss`. The script installs Inno Setup 6 first if it's missing (`tools\installer\install_inno.ps1`). It also writes `SHA256SUMS.txt`.
5. With `-TestInstall`, it runs these checks:
   - installs the setup silently, per-user, into a temp folder;
   - checks the files, the Start menu and desktop shortcuts, and the Apps & features entry;
   - runs the installed `ts_admin --version` and `--smoke`;
   - uninstalls, then checks that the files, shortcuts and entry are gone.

The script never tags or publishes anything.

## The installer

- Per-user by default, with no admin rights, into `%LOCALAPPDATA%\Programs\TombStone Engine`. "Install for all users" is offered on the first page, or with `/ALLUSERS`.
- Start menu folder *TombStone Engine*, with links to the Editor, the Client, Documentation, Samples and Uninstall, plus a desktop shortcut to the Editor (a task you can untick). The uninstaller is also listed in Apps & features.
- The shortcuts start the editor in `Documents\TombStone`, so its settings (`TombStoneConfig`) and projects (`TombStoneProjects`) live there. The uninstaller leaves that folder alone.
- GPU check: after install, `ts_game.exe --check-gpu --quiet` runs. Without OpenGL 3.3 it shows a friendly message about updating the graphics driver. Silent installs suppress the message.

Silent install and uninstall:

```
TombStone-0.1.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER [/DIR="C:\Tools\TombStone"] [/MERGETASKS="!desktopicon"]
"%LOCALAPPDATA%\Programs\TombStone Engine\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

## Installed layout notes

An installed engine finds everything next to its executables:
- templates in `templates\`;
- the C++ script SDK in `sdk\include` (C++ scripts still need CMake and a compiler);
- the C# binding in `sdk\dotnet` (C# scripts still need the .NET 8 SDK);
- plugins in `plugins\`.

The Freight Office exports games from the installed `ts_game.exe` and its DLLs.
