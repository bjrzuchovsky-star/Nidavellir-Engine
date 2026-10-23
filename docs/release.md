# Releasing the engine (Windows)

This page is about installing the TombStone Engine itself (the editor, the runtime and the tools). To ship a game made with it, see [distribution](distribution.md).

## Version

`VERSION` at the repo root holds the one version number, `MAJOR.MINOR.PATCH`. CMake reads it (`project(... VERSION)`), `ts_admin --version` prints it, and the installer and zip are named after it. [CHANGELOG.md](../CHANGELOG.md) lists what each version holds.

## Building a release

In a Developer PowerShell for VS 2022, run:

```powershell
tools\release\build_release.ps1 -Out D:\Dev\TombStoneReleases\0.2.0 [-BuildDir build] [-SkipBuild] [-SkipSmoke] [-TestInstall]
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

4. Writes `TombStone-<version>-win64.zip` (unzip and run `ts_admin.exe`), `TombStone-<version>-setup.exe` (the full installer) and `TombStone-<version>-update.exe` (the in-place upgrade, see below). Both installers come from `tools\release\tombstone.iss`, the update one with `/DUpdate=1`. The script installs Inno Setup 6 first if it's missing (`tools\installer\install_inno.ps1`). It also writes `SHA256SUMS.txt`, which lists all three files.
5. With `-TestInstall`, it runs these checks:
   - installs the setup silently, per-user, into a temp folder;
   - checks the files, the Start menu and desktop shortcuts, and the Apps & features entry;
   - runs the installed `ts_admin --version` and `--smoke`;
   - runs `-update.exe` silently over that install, then checks that the files are still there and that there's still exactly one Apps & features entry;
   - uninstalls, then checks that the files, shortcuts and entry are gone.

The script never tags or publishes anything.

## The installer

- Per-user by default, with no admin rights, into `%LOCALAPPDATA%\Programs\TombStone Engine`. "Install for all users" is offered on the first page, or with `/ALLUSERS`.
- Start menu folder *TombStone Engine*, with links to the Editor, the Client, Documentation, Samples and Uninstall, plus a desktop shortcut to the Editor (a task you can untick). The uninstaller is also listed in Apps & features.
- The shortcuts start the editor in `Documents\TombStone`, so its settings (`TombStoneConfig`) and projects (`TombStoneProjects`) live there. The uninstaller leaves that folder alone.
- GPU check: after install, `ts_game.exe --check-gpu --quiet` runs. Without OpenGL 3.3 it shows a friendly message about updating the graphics driver. Silent installs suppress the message.

Silent install and uninstall:

```
TombStone-0.2.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER [/DIR="C:\Tools\TombStone"] [/MERGETASKS="!desktopicon"]
"%LOCALAPPDATA%\Programs\TombStone Engine\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

## The update installer

`TombStone-<version>-update.exe` holds the same files as the setup and uses the same Inno `AppId`, so it upgrades an existing install in place. It keeps the previous folder, Start menu group, tasks (desktop icon), language and per-user / all-users mode. It has a minimal wizard: no welcome, licence, folder (unless there's no previous install), task or ready pages, just progress and Finish, which can start the editor again. Restart Manager closes any TombStone app that's still open. Claims, settings and the Vault live in `Documents\TombStone`, which neither installer touches.

## Publishing a release (assets the editor looks for)

The editor's update notifier ([updates.md](updates.md)) reads the GitHub release that `releases/latest` returns:

- **Tag:** `v<version>` (for example `v0.2.0`), the same as `VERSION`. Mark it as the latest release. Drafts and pre-releases aren't returned by `releases/latest`.
- **Assets:** upload `TombStone-<version>-update.exe`, `TombStone-<version>-setup.exe`, `TombStone-<version>-win64.zip` and `SHA256SUMS.txt` from the output folder, with the names unchanged.
- The editor's **Update** button fetches `TombStone-<version>-update.exe`, or `TombStone-<version>-setup.exe` if there's no update file. If `SHA256SUMS.txt` is attached, the installer must be listed in it with a matching SHA-256, or the editor refuses to run it. So always upload the `SHA256SUMS.txt` from the same build as the installers.

## Installed layout notes

An installed engine finds everything next to its executables:
- templates in `templates\`;
- the C++ script SDK in `sdk\include` (C++ scripts still need CMake and a compiler);
- the C# binding in `sdk\dotnet` (C# scripts still need the .NET 8 SDK);
- plugins in `plugins\`.

The Freight Office exports games from the installed `ts_game.exe` and its DLLs.

## Version info and icons

Every shipped exe gets a Windows version resource (Company **TombStone Team**, Product **TombStone Engine**, per-app description, version from `VERSION`), an application manifest (asInvoker, per-monitor DPI aware, Windows 10 / 11) and, except `ts_game`, the Dusk tombstone icon. All of this comes from `cmake/WinResources.cmake` and `resources/windows/`. `ts_game` has no icon because the exporter stamps each game's own. Run `python3 resources/windows/make_icons.py` (Pillow) to redraw the icons. Both installers use the same icon and version info. The builds aren't code-signed; see [security.md](security.md).
