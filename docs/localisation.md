# Localisation

TombStone games can ship in several languages. Text lives in per-language
string tables in the project. Scripts and UI ask for it by key, and the
player picks a language in the pause menu.

## String tables

Put tables in `<project>/strings/`. JSON and CSV both work, and you can mix them:

```
strings/en.json        {"language.name": "English", "menu.start": "Ride out", "hud.gold": "Gold: {0}"}
strings/fr.json        {"language.name": "Français", "menu.start": "En selle"}
strings/strings.csv    key,fr,ja          (one column per language)
strings/config.json    {"base": "en"}     (optional; default base is "en")
```

- Each file is a flat object of `key: text`, UTF-8. `\n`, `\"` and `\uXXXX` escapes work.
- CSV follows RFC 4180: quote fields that contain commas, quotes or newlines, and double any quote inside them. The first row is `key,<lang>,<lang>...`. A UTF-8 BOM is fine.
- Files are read in name order, JSON first, then CSV. A later file adds keys or overrides earlier ones.
- `language.name` is the name the language picker shows. Without it, the picker shows the code (`fr`).

Lookup falls back in this order:

1. the current language;
2. the base language;
3. the caller's fallback text;
4. the key itself.

A missing translation is therefore visible but never blank. `{0}` to `{9}`
are replaced by arguments.

## tr() everywhere

| Where | Call |
|---|---|
| Lua | `tr("hud.gold", gold)` or `tr("hud.gold", {gold})`; `lang.current()`, `lang.list()`, `lang.name("fr")`, `lang.set("fr")` |
| Visual graphs (Brand Book) | **Values → Translate** (`text.tr`): `key`, `arg0`, `arg1` → `value` |
| C++ scripts | `tr("hud.gold", {std::to_string(gold)})`, `set_language("fr")`, `language()` (`script_sdk.h`) |
| C# scripts | `Tr("hud.gold", gold)`, `SetLanguage("fr")`, `Language` |
| Engine C++ | `ts::tombstone::loc::tr(key, fallback)`, `loc::format(text, args)` (`core/Localization.h`) |

The C ABI version is now **3**. It adds `translate`, `lang_set` and
`lang_get` to `TsScriptApi`, so existing C++ and C# script modules must be
rebuilt against the new SDK headers / assembly.

## UI text keys

Any widget in a `.ui.json` can carry a key:

```json
{"type": "button", "id": "play", "text": "Play", "text_key": "menu.start"}
```

- The widget shows `tr(text_key)`. Its authored `text` is the fallback.
- The widget is re-bound automatically when the language or the tables change.
- `ui.set_text(...)` from a script replaces the text and drops the key, so script text is never overwritten.

The built-in pause menu uses keys too:

| Widget | Key |
|---|---|
| Resume | `ui.pause.resume` |
| Settings | `ui.pause.settings` |
| Quit | `ui.pause.quit` |
| Title, Volume, Master, Music, Effects, Back | `ui.pause.title`, `ui.pause.volume`, `ui.pause.master`, `ui.pause.music`, `ui.pause.effects`, `ui.pause.back` |
| Language button | `ui.pause.language` |

## Language picker

When a project has two or more languages, the pause menu shows a
**Language: English** button that cycles through them.

The choice is saved to `TombStoneConfig/language.txt` in the game's working folder and
used on the next start. The `TS_LANG=fr` environment variable overrides it,
which is handy for testing.

## Fonts for non-Latin text

The bundled UI fonts cover Latin. When a language needs other characters
(accents beyond Latin-1, Cyrillic, Greek, CJK, Hangul, Devanagari, Thai...),
the UI atlas bakes them from fallback fonts. It tries these in order:

1. every `.ttf` / `.otf` / `.ttc` in `<project>/fonts/`. **Ship one here for a released game.** Players' PCs may not have the system font you tested with.
2. OS fonts:
   - Windows: Segoe UI, Arial, Microsoft YaHei, Yu Gothic, Meiryo, MS Gothic, Malgun Gothic, SimSun, Nirmala UI, Leelawadee UI, Segoe UI Symbol;
   - Linux: Noto / DejaVu under `/usr/share/fonts`.

The atlas grows from 1024 to 2048 to 4096 px as needed. Glyphs that no font
has are listed in the log as `[ui] missing glyphs`. Word wrap breaks between
characters for text without spaces (Chinese / Japanese).

## The Strings panel (editor)

Open it from **File → Strings... (localisation)** in the 2D editor or
**Claim → Strings...** in the 3D editor.

- A grid shows one row per key and one column per language. Empty cells are red (**missing**). A key that scripts or UI use but no table defines is also red, marked *undefined*. The panel finds these keys by scanning `scripts/` and `ui/` for `tr("...")`, `Tr("...")` and `"text_key"`.
- Column headers show how many keys each language is missing. **Missing only** filters to rows with gaps.
- Click a row to edit every language's text for that key, with multi-line editors underneath.
- You can add a key or a language, delete a key, and pick a preview language. The preview also sets the language used when you press Play in the editor.
- **Save** writes the tables back. CSV-sourced languages go to `strings/strings.csv` and the rest to `strings/<lang>.json`, with sorted keys so diffs stay small.
- The editor merges fallback fonts into its own UI font, so Japanese or Arabic text shows in the panel too.

## Smoke test

`ts_admin --smoke-loc` (also part of `--smoke`) checks:

- JSON and CSV parsing, the save round trip, missing keys and the fallback chain;
- Lua `tr` / `lang`, the graph `text.tr` node and ABI 3;
- UI `text_key` binding and re-binding, and the pause-menu language button;
- fallback-font glyph baking and the Strings panel key scan.
