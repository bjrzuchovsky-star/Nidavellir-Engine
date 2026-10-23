# Security and trusting your download

## What TombStone is

TombStone Engine is a 2D / 3D / VR game engine and editor for Windows, with Dusk frontier styling. It's built and published by **TombStone Team** from the source at <https://github.com/bjrzuchovsky-star/TombStone_Engine>. Official downloads live on that repository's **Releases** page and nowhere else:

| File | What it is |
|---|---|
| `TombStone-<version>-setup.exe` | The full installer (per user, no admin needed) |
| `TombStone-<version>-update.exe` | Upgrades an existing install in place and keeps your claims, settings and the Vault |
| `TombStone-<version>-win64.zip` | The same files without an installer |
| `SHA256SUMS.txt` | SHA-256 hashes of the three files above |

Every program it installs (`ts_admin.exe`, `ts_client.exe`, `ts_server.exe`, `ts_vault.exe`, `ts_game.exe`) and both installers carry Windows version information: **Company: TombStone Team**, **Product: TombStone Engine**, the version number and a description. Right-click the file, open **Properties > Details** to see them. The programs run as the signed-in user (`asInvoker`) and never ask for administrator rights.

## Check a download with SHA256SUMS

Download `SHA256SUMS.txt` from the same release, then in PowerShell:

```powershell
Get-FileHash .\TombStone-0.2.0-setup.exe -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

The hash must match the line for that file exactly (case doesn't matter). On Linux or macOS, run `sha256sum -c SHA256SUMS.txt` in the download folder. If a hash doesn't match, don't run the file. Download it again from the Releases page, and report it if it still doesn't match.

The editor's update notice works the same way: it downloads the update installer from the latest GitHub release and checks it against that release's `SHA256SUMS.txt` before running it.

## Why Windows SmartScreen or antivirus may warn

The builds on GitHub are **not code-signed**. Windows SmartScreen and some antivirus products are cautious about any unsigned program that few people have downloaded yet, so you may see:

- **"Windows protected your PC"** (SmartScreen). Check the hash first, then choose **More info > Run anyway**.
- An antivirus warning or quarantine, usually a generic or "machine learning" detection rather than a named threat. Games and engines can look unusual to these tools because they write files, open network ports (multiplayer, team claims, the Vault server) and launch other programs (the game runtime, Blender for imports, the update installer).

A matching SHA-256 hash means the file is exactly what TombStone Team published. Warnings usually ease off as more people download a release, and the version information above helps.

## Reporting a false positive

1. Check the hash against `SHA256SUMS.txt`. If it doesn't match, stop and report that instead (see below).
2. Send the file to your antivirus vendor as a false positive. Most have a web form, for example:
   - Microsoft Defender: <https://www.microsoft.com/wdsi/filesubmission> (choose "Incorrectly detected as malware")
   - Other vendors: search for "<vendor> false positive submission".
3. Let us know by opening an issue at <https://github.com/bjrzuchovsky-star/TombStone_Engine/issues>. Include the file name, version, SHA-256 hash, the antivirus product and version, and the detection name. Never attach the file itself or anything private.

## Reporting a security problem

If you find a vulnerability (for example in the Vault server, team sessions or the update check), please don't post details in a public issue. Open an issue asking for a private contact, or use GitHub's **Report a vulnerability** button on the repository's Security tab if it's enabled. Please include the steps to reproduce.

## Good habits

- Download TombStone only from the GitHub Releases page.
- Run `ts_vault` and team sessions only on networks you trust. Keep Vault API keys private, and use read-only keys for viewers.
- Only import and ship content you have the rights to (see [model-import.md](model-import.md#source-engine-formats-mdlvtfvmtgma)).
