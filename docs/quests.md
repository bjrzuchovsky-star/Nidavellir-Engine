# Quests: Bounty Board (`ts.quests`)

Source: `src/story/Quest.*`, `src/story/StoryRuntime.*`, `src/story/StoryLua.cpp`, `src/plugins/story/`, `src/story3d/`, `src/editor/plugins/StoryPanels.cpp`.

Bounty Board is an **optional** Trading Post plugin, so it ships only in games that turn it on. Turning it on also turns on **Story Core** (`ts.story`), which provides the shared story variables and runtime. Parley (dialogue, see [dialogue.md](dialogue.md)) and Strongbox (saves, see [saves.md](saves.md)) build on the same core.

To turn it on, use the Trading Post or the Outfitter's Wagon, or edit `project.json` directly:

```json
{ "plugins": "ts.story,ts.quests,ts.dialogue,ts.saves" }
```

The 2D runtime is created once per world when Play begins. Posse client worlds don't get one. In 3D, the runtime is the `story3d::StoryWorld3D` world extension, and both editor Play and `ts_game` attach it.

## Quest files

Quest files live at `<project>/quests/*.quest.json`. A file holds either one quest or `{"quests": [...]}`.

```json
{"id": "bell", "title": "Ring the Bell", "giver": "Old Hank",
 "description": "The church bell has gone quiet.",
 "requires": ["intro"], "next": "lanterns",
 "objectives": [{"id": "ring", "text": "Ring the church bell", "count": 1},
                {"id": "polish", "text": "Polish it", "optional": true, "hidden": true}],
 "rewards": ["add gold 25", "event bounty_paid 25"], "auto_complete": true}
```

- **`requires`** lists quests that must be completed before this one can start.
- **`next`** names a quest that starts automatically when this one completes.
- **Auto-completion:** a quest completes on its own once every objective that isn't optional is done. This is the default; set `"auto_complete": false` to complete it from a script or a dialogue action instead.
- **`rewards`** are actions run on completion. The action syntax is shared with dialogue; see [dialogue.md](dialogue.md#conditions-and-actions).
- **Status** is one of `inactive`, `active`, `completed` or `failed`.

## Journal and tracker

Story Core adds these built-in Dusk UI documents unless the project already ships a `ui/<name>.ui.json` with the same name:

- **`quest_tracker`** is a small HUD that shows the tracked quest and its objectives.
- **`journal`** is a modal list of active and finished quests with their details. Open it with `quests.journal()`.

`story.json` settings: `"tracker": false` hides the tracker, and `"toasts": false` turns off the quest toasts.

## Lua (`quests`)

| Call | Does |
|---|---|
| `quests.start(id)` | Start a quest. Returns `ok, why`. |
| `quests.progress(q, o [, n])` | Add `n` (default 1) to objective `o` of quest `q`. |
| `quests.set(q, o, n)` | Set the objective's count to `n`. |
| `quests.complete(id)` / `quests.fail(id)` | End the quest by hand. |
| `quests.status(id)` | Returns `"inactive"`, `"active"`, `"completed"` or `"failed"`. |
| `quests.count(q, o)` / `quests.done(q [, o])` | Objective progress, or whether the quest or objective is done. |
| `quests.list([status])` / `quests.info(id)` | Quest ids, or one quest's definition and progress as a table. |
| `quests.track(id)` | Choose which quest the tracker shows. |
| `quests.journal([on])` | Open or close the journal. With no argument it toggles. |

Hooks are called on every running Lua script that defines them:

- `on_quest_started(id)`, `on_quest_completed(id)`, `on_quest_failed(id)`
- `on_objective("quest/objective", count)`

Hooks are **Lua only**. Graph, C++ and C# scripts don't receive them.

## Editor: Bounty Board panel

After you enable the plugin, open the panel from **View → Bounty Board (Quests)** in either the 2D or the 3D editor. It's also available during Play. The panel can:

- list, create and edit quest files;
- lint them, including `requires` and `next` ids that point at quests in other files;
- show live quest state while the game plays.

## Demo

The **Ghost Town 3D** template has a small demo. Walk up to Old Hank and press interact to hear his offer and take "Ring the Bell". Ringing the bell (its trigger calls `quests.progress("bell", "ring")`) completes the quest, pays $25 and autosaves.

The smoke `ts_admin --smoke-story` (also part of `--smoke`) plays the shipped demo through from start to finish.
