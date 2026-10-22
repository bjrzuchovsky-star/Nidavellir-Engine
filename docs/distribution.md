# Distributing an exported game

To install the engine itself, see [release.md](release.md).

The Freight Office (see [export.md](export.md)) turns a project into a game folder. This page covers getting that folder onto other people's Windows PCs. Store publishing isn't covered.

Source: `src/export/Installer.*` (settings, Inno Setup script, PE import check, `.ico`), `src/export/Exporter.cpp`, `src/editor/export/FreightOffice.*`, `apps/game/GpuCheck.*`, `tools/installer/`.

## What you get

| Output | Where | Notes |
|---|---|---|
| Game folder | `<out>/<slug>/` | Runs as-is from any folder |
| Portable zip | `<out>/<slug>-<version>.zip` | Unzip anywhere and run `<slug>.exe`. Needs no install |
| Setup installer | `<out>/<slug>-<version>-setup.exe` | Built with Inno Setup 6 |
| Installer script | `<out>/<slug>-installer.iss` | Always written. Rebuild with `ISCC.exe` |

The installer has:

- the game name, version and publisher, and the icon (as a `.ico` made from the project PNG);
- a license / EULA page, when you set a license file;
- a Start menu shortcut, an optional desktop shortcut (a task the player can untick), and an uninstaller. The uninstaller is also listed in **Apps & features**;
- per-user or all-users installs. **Ask** installs per-user without admin rights by default and offers "Install for all users" on the first page. **Per-user** never needs admin. **All users** needs admin and installs into Program Files;
- an OpenGL 3.3 check after the install (`<game>.exe --check-gpu --quiet`). On a PC without GL 3.3 it shows a friendly message about updating the graphics driver. Silent installs suppress the message.

## No files from the dev machine

- **VC++ runtime (app-local):** the ts_game build copies `vcruntime140*.dll` and `msvcp140*.dll` next to `ts_game.exe` (CMake `InstallRequiredSystemLibraries`). The exporter ships every DLL that sits beside the runtime, so players need no VC++ Redistributable. UCRT is part of Windows 10 and later.
- **Dependency check:** every export reads the PE import tables of the exe and of each shipped DLL, including delay-loads. Each import must be either a Windows system DLL or a DLL shipped in the folder. If one isn't, the export fails with `missing DLL on a clean PC: ...`. To check by hand: `dumpbin /dependents <slug>.exe`.
- **Content:** the content is all in `game.tspak`. The pak cache goes to `%LOCALAPPDATA%\TombStone\paks`, or `TS_PAK_CACHE`. The uninstaller leaves that cache in place.
- **Friendly GPU error:** if the game can't open its window (no OpenGL 3.3), it shows a message box rather than closing silently. Set `TS_NO_DIALOGS=1` to keep the message on stderr only.

## Freight Office settings

The **Distribution** section of the Freight Office has these settings:

- portable zip
- Windows setup installer
- license / EULA
- install for (ask / per-user / all users)
- Start menu and desktop shortcuts

The publisher, version and icon fields are in the Waybill section.

**Save settings** and **Ship It** both store them in `<project>/export_settings.json`. That file never goes into the pak. `ts_admin --export` reads it too, and command-line flags override it.

## Command line

```
ts_admin --export <project | sample:<id>> --out <dir> --zip --installer
         [--publisher P] [--license EULA.txt] [--install-mode ask|user|all]
         [--icon icon.png] [--version V] [--no-desktop-icon]
```

To do the whole thing on Windows, run `tools\installer\build_installer.ps1 -Project <dir> -Out <dir>`. It installs Inno Setup 6 when it's missing, using `tools\installer\install_inno.ps1`: winget per-user first, then the signed jrsoftware.org installer. Then it exports. ISCC is looked up in this order:

1. `TS_ISCC`
2. `PATH`
3. `Program Files (x86)\Inno Setup 6`
4. `Program Files\Inno Setup 6`
5. `%LOCALAPPDATA%\Programs\Inno Setup 6`

Without ISCC the export still succeeds. It writes the `.iss` script and adds a warning.

## Installing silently (IT / testing)

```
<slug>-<version>-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /DIR="C:\Games\X" [/MERGETASKS="desktopicon"]
"C:\Games\X\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

Use `/ALLUSERS` instead of `/CURRENTUSER` for an all-users install. It needs admin.

## Smoke

`ts_admin --smoke-installer` covers:

- the settings round trip;
- the installer script contents;
- a Ghost Town 3D export with a zip and an EULA;
- the DLL import check and the app-local CRT;
- a run from a clean copy of the folder, with `PATH` cut to the Windows folders.

When ISCC is present, it also installs silently per-user into a temp folder, checks the Start menu shortcut, runs the installed game with `--smoke` and `--check-gpu`, then uninstalls and checks that the files and shortcuts are gone. Without ISCC it prints `SKIP` for the installer part and still passes.

To screenshot the Freight Office: `ts_admin --freight-shot <project | sample:<id>> out.png`.
