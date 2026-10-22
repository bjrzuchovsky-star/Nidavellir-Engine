# The Vault

The Vault is a universal asset library that sits outside your claims (projects). You send textures, models, sounds, fonts and prefabs to it from any claim and pull them into any other. A pulled asset keeps its `.meta` file and its GUID, so references to it (scenes, prefabs, materials) work the same in every claim that uses it.

> **Rights:** you are responsible for having the rights to anything you upload to a Vault, the same as for anything you import (see [model-import.md](model-import.md)). The engine ships no third-party assets, and the Vault smoke builds its own tiny files in a temp folder.

The Vault builds on the Asset Browser and the import pipeline ([assets.md](assets.md), [model-import.md](model-import.md)): `.meta` files with stable GUIDs, content hashes, per-asset import settings, hot reload and Find references.

## Libraries

Both kinds of library sit behind the same interface (`vault::Backend` in `src/vault/VaultBackend.h`), so the panel and the CLI work the same with either.

| Library | Where | Use |
|---|---|---|
| **Local library** | A folder shared by every claim on the PC. Default `Documents\TombStone\Vault` (Linux: `~/Documents/TombStone/Vault`; override with `TS_VAULT_DIR`). | One person, one machine. |
| **Vault server** | `ts_vault`, a small headless HTTP server with a JSON API that you host yourself. | Several PCs, a team, a home server. |

Both store the same way: blobs named by their SHA-256 (`blobs/ab/abcdef…`) plus a SQLite index (`vault.db`). A local library folder can later be served by `ts_vault --root <that folder>`.

Pick the library under **Vault > Settings** (password field for the API key, **Test connection**). The settings are saved with the editor settings (`vault_backend`, `vault_local_dir`, `vault_url`, `vault_api_key`). The key is never written to a log, the console, or an error message.

## What an item holds

An item is one asset with everything it needs. Its id is the main asset's GUID.

- The files: the main file, its `.meta` (GUID and import settings, read from `.tombstone/meta`), and the files it needs that are not assets of their own (a glTF's `.bin`, an `.mtl`).
- Its **dependencies**: other items it uses. A model depends on its textures (glTF `images[].uri`, OBJ `map_*`), and a prefab depends on the models, textures and prefabs it names. Pulling an item pulls its dependencies too.
- A name, kind (texture, model, audio, font, prefab), tags, **2D / 3D / VR** tags, an author, a licence, a description, and a thumbnail (made from the texture, or a model's first texture; PNG, at most 128 px).
- **Versions**. Sending an asset again whose files changed makes a new version (v2, v3, …) with an optional note. Sending unchanged files makes no new version but updates the metadata. When a dependency gets a new version, the item that uses it gets one too.

## The Vault panel

**View > Vault** in both the 2D and the 3D editor (docked under the viewport).

- **Browse, search and filter**: a tile grid with thumbnails. Search matches names, tags, authors and descriptions. Filter by kind, by 2D / 3D / VR and by tag. Each tile shows its latest version, an **UPDATE** badge when the claim has an older version, **PINNED**, and a dot when the item is already in this claim.
- **Send to Vault**: right-click an asset in the Asset Browser > **Send to Vault...**, use the button in the Asset Browser's details pane, or click **Send to Vault...** in the panel (an asset or a prefab). The dialog asks for name, author, licence (presets: CC0, CC-BY, CC-BY-SA, MIT, proprietary, or your own text), tags, 2D / 3D / VR, description, a version note and an optional thumbnail, and it lists the files and dependencies that will go up.
- **Pull**: select a tile and press **Pull** (or pick an older version first), or drag a tile into the Asset Browser or the scene. A drop on the scene pulls the item and then places it (sprites and prefabs in 2D, models and prefabs in 3D).
- Pulled assets show where they came from in the Asset Browser (a **V** badge, an origin line in the details pane), and **UPD** when a newer version is waiting. **Update all (N)** brings every pulled item that isn't pinned to its latest version. Tick **Pin** to keep an item at its version.

Network and disk work runs off the main thread. While a job is running, the Asset Browser holds its rescan, so the GUIDs that a pull writes are read only once.

## How a pull keeps references stable

A pull copies the files and the `.meta` with the **same GUID**. The `.meta` goes into the claim's metadata folder (`.tombstone/meta/<path>.meta`, see [assets.md](assets.md)). In the library it's still named by its sidecar path (`assets/models/crate.gltf.meta`), so items sent from claims that kept sidecar `.meta` files pull the same way and are converted on arrival. When the claim sorts imports (the default), a **new** item goes to a type folder: a model and the texture items it depends on go to `assets/Models/<Name>/` with their relative layout kept, so its texture paths still work, and a texture, sound or font goes to `assets/Textures/`, `assets/Audio/` or `assets/Fonts/`. Prefabs, which name their parts by path, keep the paths from the claim they were sent from. So does everything when sorting is off (`assets/models/crate.gltf`). If the GUID already lives elsewhere in this claim (you moved the file), or the claim's lock file already lists the item, the pull writes to that location instead. The `.meta`'s `path` field is rewritten to the destination, so the next Asset Browser scan sees a known asset, not a move or a copy.

A pull refuses to overwrite:

- a file at the same path that belongs to a different asset (different GUID);
- a pulled file you changed locally (its hash differs from the one in the lock).

**Force** (`--force` on the CLI) overwrites anyway.

Each claim records what it pulled and sent in `.vault/lock.json`: item, version, library, pinned, the main asset, GUIDs and file hashes. The folder starts with a dot, so the Asset Browser, Find references and exports skip it. Commit it with the claim if you want teammates to see the same versions.

## Exports

Ship It (and `ts_admin --export`) leaves out pulled items the game never uses. An item counts as used when a file that isn't itself an unused pulled file names one of its files: a scene, a prefab, a script, a material, a `.ui.json` and so on. The items a used item depends on count as used too, and the files of used pulled items are followed in turn. The export report lists how many pulled files were left out. Assets you made yourself are always exported, as before.

## Command line

```
ts_admin --vault push   --project DIR --asset REL [--name N] [--tags a,b] [--dims 2d,3d,vr]
                        [--author A] [--licence L] [--note TEXT] [--description TEXT] [--thumbnail PNG]
ts_admin --vault pull   --project DIR --item ID|NAME [--version N] [--pin] [--force]
ts_admin --vault list   [--query Q] [--kind K] [--tag T] [--dim D]
ts_admin --vault status --project DIR
ts_admin --vault update --project DIR
```

The library comes from the editor settings, or from `--library DIR` (a local folder), or from `--server URL --key KEY`. Instead of `--key` you can set `TS_VAULT_KEY` in the environment, which keeps the key out of your shell history. The key is never printed.

```
ts_admin --vault push --project "D:\Claims\Dusty Town" --asset assets/models/crate.gltf --tags props,western --dims 3d,vr --licence CC0-1.0
ts_admin --vault list --server http://vault.local:8740 --dim vr
ts_admin --vault pull --project "D:\Claims\Ghost Town" --item crate --pin
```

## ts_vault server

```
ts_vault --root DIR [--host ADDR] [--port N]   serve (default 127.0.0.1:8740)
ts_vault --root DIR --add-key NAME [--read-only]   create an API key; it is printed once
ts_vault --root DIR --list-keys | --revoke-key NAME
ts_vault --smoke | --version | --help
```

`--root` defaults to `$TS_VAULT_ROOT`, or `./vault-data`. The server won't start until there is at least one key. Keys look like `tsv_` followed by 48 hex digits and are stored only as SHA-256 hashes. A read-only key can browse and pull but not send. The access log has one line per request (peer, method, path, status) and never includes headers, keys or bodies.

### API (JSON, under `/api/v1`)

Every route except `health` needs `Authorization: Bearer <key>` (or `X-API-Key: <key>`). A missing or unknown key gets **401**; a read-only key that tries to write gets **403**.

| Method and path | What it does |
|---|---|
| `GET /health` | `{ok, server, version}`, no key needed |
| `GET /whoami` | `{name, can_write}` for the key |
| `GET /items?q=&kind=&tag=&dim=` | list items (versions without file lists) |
| `GET /items/<id>` | one item with every version, its files and dependencies |
| `POST /items/<id>/versions` | publish `{item, version}`; every blob must already be there |
| `POST /blobs/missing` | `{sha256: [...]}` → the ones the server doesn't have |
| `GET /blobs/<sha256>` | download; supports `Range: bytes=N-` for resuming |
| `POST /uploads` | `{sha256, size}` → `{id, offset}`; reuses an open upload of the same blob |
| `GET /uploads/<id>` | `{offset, size}`: how much has arrived |
| `PUT /uploads/<id>?offset=N` | append a chunk; a wrong offset gets **409** with the right one |
| `POST /uploads/<id>/complete` | verify the SHA-256 and store the blob (**422** if it doesn't match) |

Uploads are chunked (1 MiB by default) and **resumable**: if the connection drops, the next push asks for the upload's offset and sends only the rest. Downloads resume from a `.vault-part` file in the same way. Content addressing means a blob that is already on the server (the same texture in two items) is never sent twice.

### Hosting on Linux

```sh
sudo useradd --system --home /srv/ts_vault --create-home ts_vault
sudo install -m 755 ts_vault /usr/local/bin/ts_vault
sudo -u ts_vault ts_vault --root /srv/ts_vault --add-key brenden      # copy the key it prints
```

`/etc/systemd/system/ts_vault.service`:

```ini
[Unit]
Description=TombStone Vault
After=network-online.target

[Service]
User=ts_vault
ExecStart=/usr/local/bin/ts_vault --root /srv/ts_vault --host 127.0.0.1 --port 8740
Restart=on-failure
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/srv/ts_vault

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload && sudo systemctl enable --now ts_vault
curl http://127.0.0.1:8740/api/v1/health
```

`ts_vault` speaks plain HTTP. On a LAN you can use `--host 0.0.0.0` and open port 8740 (`sudo ufw allow 8740/tcp`). Over the internet, put it behind a TLS reverse proxy and keep it bound to 127.0.0.1. For example, with Caddy:

```
vault.example.com {
    reverse_proxy 127.0.0.1:8740
}
```

With nginx, add `client_max_body_size 32m;` (chunks are 1 MiB, and the server accepts bodies up to 16 MiB) and `proxy_request_buffering off;`. In the editor, set the server URL to `https://vault.example.com`. The Windows editor speaks HTTPS through WinHTTP. On Linux the editor and CLI speak plain HTTP only, so reach the server over a LAN, an SSH tunnel (`ssh -L 8740:127.0.0.1:8740 host`), or a VPN.

### Hosting on Windows

```powershell
New-Item -ItemType Directory C:\TombStoneVault | Out-Null
Copy-Item ts_vault.exe C:\TombStoneVault\
C:\TombStoneVault\ts_vault.exe --root C:\TombStoneVault\data --add-key brenden   # copy the key it prints
```

To run it at startup without anyone signed in, use a scheduled task (or wrap it with a service manager such as NSSM):

```powershell
$a = New-ScheduledTaskAction -Execute 'C:\TombStoneVault\ts_vault.exe' -Argument '--root C:\TombStoneVault\data --host 0.0.0.0 --port 8740'
$t = New-ScheduledTaskTrigger -AtStartup
Register-ScheduledTask -TaskName 'TombStone Vault' -Action $a -Trigger $t -User 'SYSTEM' -RunLevel Highest
New-NetFirewallRule -DisplayName 'TombStone Vault' -Direction Inbound -Protocol TCP -LocalPort 8740 -Action Allow -Profile Private
Start-ScheduledTask -TaskName 'TombStone Vault'
```

Use `--host 127.0.0.1` (and no firewall rule) to keep it on this PC only. For access from outside your network, put IIS, Caddy or nginx in front with TLS, as on Linux.

**Backups:** stop the server and copy the whole `--root` folder (`vault.db`, `blobs/`). Blobs never change after they are written.

## Smoke

`ts_admin --smoke-vault` (also part of `ts_admin --smoke`; run with `TS_AUDIO=null`) checks:

- **Local library**:
  - a model with a texture dependency goes up as two items, with metadata, search and filters;
  - it is pulled into two separate temp claims, with the SHA-256 of every file and the GUID of every `.meta` checked, and no fresh GUIDs or moves on the next scan;
  - unchanged content makes no new version;
  - a prefab is pushed and pulled with its dependencies;
  - a conflicting file is refused;
  - an unused pulled sound is left out of exports while the used crate is kept.
- **Remote**, against an in-process `ts_vault` on 127.0.0.1:
  - bad, missing and read-only keys are refused, and the key never appears in errors or the log;
  - chunked upload of the model and texture, then a pull into two separate claims with GUIDs and hashes checked;
  - v2 and the **update** badge in the panel, then update and pin;
  - an upload cut after three chunks resumes without sending anything twice;
  - a wrong chunk offset gets 409 and a bad hash gets 422.

`ts_vault --smoke` starts the server on a free port and checks `health`, key auth and a blob round trip. `ts_admin --vault-shot out.png` renders the panel over a demo library.
