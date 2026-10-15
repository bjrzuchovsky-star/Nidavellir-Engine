# Saves: Strongbox (`ts.saves`)

Source: `src/story/SaveGame.*`, `src/story/StoryRuntime.*`, `src/story/StoryLua.cpp`, `src/editor/plugins/StoryPanels.cpp`.

Strongbox is an **optional** Trading Post plugin that provides named save slots and a rotating autosave. Turning it on also turns on Story Core (`ts.story`).

## Slot files

Each slot is saved to `<project>/saves/slots/<slot>.save.json`:

```json
{"format": 1, "slot": "1", "label": "Before the bell", "time": "2026-10-15T02:00:00Z",
 "playtime": 412.5, "scene": "Ghost Town 3D", "auto": false,
 "sections": {"vars": {}, "quests": {}, "dialogue": {}, "data": {}, "player": {}}}
```

- **What's saved:** story variables, quest state, dialogue memory (which `once` choices have been taken) and the rider's position. That position is the 2D player, or the 3D rider through `World3D::rider_position`.
- **Slot names** may contain letters, digits, `-` and `_`, up to 32 characters.
- **Safe writes:** a save goes to a temp file first and is then renamed into place, so a crash during a save leaves the old slot intact.
- **Load** restores the saved state into the running scene. It doesn't reload the scene file.

## Autosave

Autosaves rotate through `auto-1` … `auto-N`: the newest overwrites the oldest. An autosave runs:

- every `autosave_seconds` of play;
- when a quest completes, if `autosave_on_quest` is on;
- on demand.

It is skipped while a dialogue is open.

Settings go in the optional `<project>/story.json`; these are the defaults:

```json
{"autosave_seconds": 300, "autosave_on_quest": true, "autosave_slots": 3,
 "tracker": true, "toasts": true}
```

## Lua (`saves`)

| Call | Does |
|---|---|
| `saves.save(slot [, label])` / `saves.load(slot)` | Write or read a slot. Returns `ok, error`. |
| `saves.autosave()` | Run an autosave now. |
| `saves.exists(slot)` / `saves.delete(slot)` | Check whether a slot exists, or delete it. |
| `saves.latest()` / `saves.list()` | The newest slot, or every slot with its label, time and playtime. |
| `saves.menu([on])` | Open, close or toggle the built-in Dusk `save_menu`. |
| `saves.autosave_every(seconds)` | Change the autosave interval. `0` turns the timer off. |
| `saves.playtime()` | Seconds of play, carried across saves. |

Hooks: `on_game_saved(slot)` and `on_game_loaded(slot)`. Lua, graphs, C++ and C# all receive them (see [scripting.md](scripting.md#plugin-events-story-hooks)).

## Editor: Strongbox panel

Open it from **View → Strongbox (Saves)**. It lists the project's slots and lets you inspect, load (during Play) or delete them. Saves made during editor Play go to the project's own `saves/` folder.

## Notes

- On a posse server, the story UI is not sent to clients yet.
- VR rides don't attach the story extension yet.
