# The Trading Post (plugin store)

Source: `src/store/TradingPost.*`, `Fetch.*`, `Crypto.*`, `src/editor/store/TradingPostPanel.*`, `src/plugin/`.

## Opening it

Use **Trading Post** in the Project Manager, or **Visit the Trading Post...** in the editor's Plugins panel. There you can:

- browse, search, and filter by category
- see a plugin's details, versions and dependencies
- install and uninstall plugins
- enable a plugin for the current claim (the same list the Outfitter's Wagon edits)

## Sources

Entries come from four kinds of source (`TradingPost::refresh`):

- **Built-ins** from the plugin catalog, including `ts.ai` (Pathfinder, the AI Toolkit; see the root README) and the optional story plugins: `ts.story` (Story Core), `ts.quests` (Bounty Board, [quests.md](quests.md)), `ts.dialogue` (Parley, [dialogue.md](dialogue.md)) and `ts.saves` (Strongbox, [saves.md](saves.md)). Enabling any feature plugin also enables `ts.story`.
- **Local folders** that hold plugin libraries.
- **Remote indexes** over `file://` or `http(s)://`. The README notes that https is supported on Windows.
- **Coming soon** roadmap entries, hard-coded in `TradingPost.cpp`:
  - `ts.steam`: Steam Hitch

  Assay Office (`ts.profiler`) used to be listed here. It is now an optional built-in ([profiler.md](profiler.md)).

## Plugin cache and configuration

The cache is `$TS_PLUGIN_CACHE`, or `<cwd>/TombStonePlugins` if that is not set. Each plugin gets `<id>/installed.json` plus its library. Uninstalling a library that is still loaded leaves a `.remove` marker instead.

Sources are configured in `<cache>/trading_post.json`:

```json
{ "local_folders": ["D:/plugins"], "index_urls": ["https://example.com/ts/index.json"],
  "trusted_keys": ["<64 hex Ed25519 public key>"], "allow_unsigned": false }
```

## Remote index format

```json
{ "name": "My Exchange", "plugins": [ { "id": "acme.lanterns", "name": "Lanterns", "version": "0.3.0",
  "category": "Sample", "description": "...", "types": ["2d"],
  "deps": [ { "id": "ts.tumbleweed", "min": "1.0.0" } ],
  "downloads": { "windows-x64": { "url": "pkgs/lanterns.dll", "sha256": "<hex>", "size": 12345 } } } ] }
```

How an index and its downloads are checked:

- **Signature:** `index.json.sig` is a hex Ed25519 signature over the exact bytes of `index.json`, checked against `trusted_keys` (Monocypher).
- **Signing:** `ts_admin --store-sign <index.json> <64-hex seed>` writes the `.sig` file and prints the public key to trust.
- **Downloads:** each download must match its `sha256` and `size`. The library is then loaded to confirm its plugin id.
- **URLs:** relative URLs resolve against the index URL.

## Writing plugins

Source: `src/plugin/`.

A plugin derives from `ts::plugin::Plugin` and has an id, a semver version and dependencies. `on_register(Registrar&)` can add:

- World systems and components
- editor panels and menu items
- Lua tables
- asset types and tick hooks
- lights, occluders, ambient overrides and HUD layers
- net channels

`PluginManager` orders plugins by dependency, checks versions (same major version, at least the minimum), and unloads them in reverse order.

- **Static plugins:** use `TS_PLUGIN_STATIC(sym, Class)` in the plugin, plus `TS_PLUGIN_DECLARE` / `TS_PLUGIN_LINK` in the host.
- **Dynamic plugins:** use `TS_DYNAMIC_PLUGIN(Class)`, which exports `ts_plugin_entry`. The module must use the same compiler and runtime as the host (ABI tags are checked).
- **Search path:** `$TS_PLUGIN_PATH`, then `<project>/plugins`, then the build's `plugins/` folder.
- **Sample:** `src/plugins/lantern`.

Smoke test: `ts_admin --smoke-store`.
