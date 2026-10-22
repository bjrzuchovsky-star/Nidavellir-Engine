# Updates: "New update available"

The editor tells you when a newer TombStone Engine release is out. The notice sits on the Project Manager (the start screen that lists your claims), right under the title:

> **New update available: v0.2.0**  You're riding v0.1.0. Fresh supplies are in from the trail.
> **Update** · **Release notes** · **Later** · **Skip this version**

## How the check works

- When the Project Manager opens, the editor asks GitHub for the latest release: `GET https://api.github.com/repos/bjrzuchovsky-star/TombStone_Engine/releases/latest`, with a `User-Agent` header. The request runs on a background thread, so the screen never waits on it.
- It strips the leading `v` from `tag_name` and compares it with the editor's own version using semver rules. A pre-release sorts before its release, and build metadata (`+...`) is ignored. The editor's own version is the `VERSION` file, compiled in by CMake (`TS_ENGINE_VERSION`), the same number `ts_admin --version` prints.
- There's at most one check every **6 hours**. The time of the last check and what it found (version and release page) are cached in `TombStoneConfig/settings.json` (`update_last_check`, `update_cached_version`, `update_cached_url`). Inside that window, the cached result is used and no request is made.
- Offline, rate limited (HTTP 403 / 429), GitHub down, or a reply that isn't valid JSON: nothing shows. The reason goes to the console log (`[Update] check failed (quietly): ...`), and the next check comes 6 hours later.
- The request goes through the Vault's HTTP client (`src/vault/Http.h`): WinHTTP for `https://` on Windows, so the system proxy and certificate store apply. That client only speaks plain `http://` on Linux and macOS, so update checks are switched off on those platforms (the log says so, and Settings shows a note). No other HTTP stack is pulled in.

## The buttons

| Button | What it does |
|---|---|
| **Update** | Downloads the installer from the release into `%TEMP%\TombStoneUpdate\` with a progress bar, verifies it, runs it and closes the editor. |
| **Release notes** | Opens the release page (`html_url`) in your browser. |
| **Later** | Hides the notice until the next time the editor starts. |
| **Skip this version** | Hides it for that version for good (`update_skip_version` in settings.json). A newer release shows again. Settings > Supply Run > **Stop skipping** undoes it. |

### Which file Update fetches

1. `TombStone-<version>-update.exe`, if the release has one: the small-wizard upgrade (see [release.md](release.md#the-update-installer)).
2. Otherwise `TombStone-<version>-setup.exe`, the full installer. It upgrades in place too (same AppId), with the usual pages.

The version in the file name has to match the release tag. If neither file is attached, Update says so in the notice and nothing is run.

### Verification

- If the release has a `SHA256SUMS.txt` asset (`build_release.ps1` writes one), the editor downloads it first. The installer **must** be listed there, and its SHA-256 must match. Otherwise the download is deleted and the notice shows the error.
- With no `SHA256SUMS.txt`, it uses GitHub's own asset digest (`"digest": "sha256:..."`) when the API gives one.
- A download that is cut short (the size doesn't match the asset's `size`) is thrown away too.

The installer is started with the shell (`ShellExecute`), so a UAC prompt works for all-users installs. Then the editor closes so its files can be replaced. Your claims, settings and the Vault live in `Documents\TombStone`, which no installer touches.

## Settings

Settings > **Supply Run** > **Check for updates on start** (on by default). With it off, the editor never contacts GitHub.

Environment overrides:

| Variable | Effect |
|---|---|
| `TS_FAKE_UPDATE=9.9.9` | Forces the notice for v9.9.9 with no network call, even with checks off or the version skipped. Use it for screenshots and UI testing. Update only says it's a test notice; Release notes opens the releases page. |
| `TS_NO_UPDATE_CHECK=1` | No check this run (kiosks, offline labs). |

Headless runs (`--smoke`, `--export`, the CLI) never check. Only the windowed editor turns the notifier on.

## Smoke test

`ts_admin --smoke-update-check` (also part of `ts_admin --smoke`, so CI runs it on Windows and Linux) needs no internet:

- semver cases, canned GitHub JSON, asset choice (`-update.exe`, then `-setup.exe`, and never another version's file), `SHA256SUMS.txt` parsing, and the 6-hour throttle;
- the background check and download against an in-process HTTP server on loopback (the Vault's server). It covers a verified download, a checksum mismatch, the digest fallback, no installer, a rate limit, up to date and offline;
- the Project Manager flow through `AppFlow`: the notice, Skip (saved), the cache (no second request), the setting turned off, Later, Update (download, verify, then a dry-run launch that closes the editor) and `TS_FAKE_UPDATE`.

## Code

| File | What |
|---|---|
| `src/editor/update/UpdateCheck.h/.cpp` | Pure logic: semver, release JSON, asset choice, sums, throttle |
| `src/editor/update/UpdateService.h/.cpp` | Background check and verified download (Vault HTTP client + SHA-256), launching the installer, opening the browser |
| `src/editor/update/UpdateBanner.h` | What the notice shows and its buttons |
| `src/editor/AppFlow.cpp` | When to check, the cache in settings, button handling |
| `src/editor/screens/ProjectManagerScreen.cpp` | Draws the notice |
| `apps/admin/SmokeUpdate.cpp` | `--smoke-update-check` |
