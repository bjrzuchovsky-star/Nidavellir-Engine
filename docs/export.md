# Export and the Freight Office

Source: `src/export/` (`Exporter.*`, `Validate.cpp`, `Zip.*`, `WinIcon.*`), `src/editor/export/FreightOffice.*`, `apps/admin/ExportCli.cpp`, `src/vfs/`, `apps/game/GameManifest.*`.

## Exporting from the editor

Open the Freight Office with **File > Export... (Freight Office)** in 2D, or **Claim > Export... (Freight Office)** in 3D / VR.

## Exporting from the command line

```
ts_admin --export <project dir | sample:<id>> --out <dir>
         [--profile shipping|dev] [--server] [--zip] [--runtime <ts_game>]
         [--name N] [--version V]
```

On Windows, build Release first with `cmake --build build --config Release`. The exporter copies the `ts_game` next to `ts_admin`, unless you pass `--runtime`.

## Output: `<out>/<slug>/`

| File | Contents |
|---|---|
| `<slug>.exe` | `ts_game`, renamed. On Windows, `icon.png` is stamped in as the exe icon. |
| `game.json` | Manifest: name, version, icon, window size, start scene, dimension, `vr`, profile, pak, `plugins` (what the game loads) |
| `game.tspak` | Cooked content |
| `plugins/` | Dynamic plugin DLLs the project enables. Only the plugins the claim uses ship; physics and movement are built-in plugins that load only when the claim uses them ([physics.md](physics.md)). |
| `server/` | Only with `--server`: `ts_server`, the content and a run script |
| `README.txt`, `export_report.txt` | How to run the game, and the validation report |

## Profiles

- **shipping:** any validation error stops the export, and editor state is stripped.
- **dev:** problems are only warnings, `.pdb` files are kept, and the game writes `telegraph.log`.

`--zip` writes `<slug>-<version>.zip` (portable). `--installer` also builds a Windows setup installer; see [distribution.md](distribution.md) for the installer, the app-local VC++ runtime, the dependency check and the distribution settings.

## Validation

The exporter checks that:

- the scene exists
- every `.lua` file compiles
- every referenced asset exists
- every enabled plugin resolves

## .tspak and ts_vfs

- **Format** (`src/vfs/Pak.h`): the header is `TSPAK\0\0\1`, followed by an index of entries with FNV-1a 64 hashes. Entries are stored uncompressed, and reads are hash-checked.
- **Mounting:** `Vfs` mounts the pak over loose files.
- **Cache:** the runtime lays the pak out in a cache: `$TS_PAK_CACHE`, or `%LOCALAPPDATA%\TombStone\paks` on Windows.

## Running an export

`ts_game` with no `--project` reads `game.json` next to it.

Smoke test: `ts_admin --smoke-export`. It exports every sample and plays each one from its pak.
