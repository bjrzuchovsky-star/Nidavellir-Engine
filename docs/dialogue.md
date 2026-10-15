# Dialogue: Parley (`ts.dialogue`)

Source: `src/story/Dialogue.*`, `src/story/Vars.*`, `src/story/StoryLua.cpp`, `src/editor/plugins/StoryPanels.cpp`.

Parley is an **optional** Trading Post plugin for branching dialogue. Turning it on also turns on Story Core (`ts.story`). See [quests.md](quests.md) for how to turn plugins on.

## Dialogue files

Dialogue files live at `<project>/dialogue/*.dlg.json`:

```json
{"id": "hank", "start": "hello",
 "speakers": {"hank": {"name": "Old Hank", "portrait": "assets/hank.png"}},
 "nodes": [
   {"id": "hello", "speaker": "hank", "text": "Evenin', {player}.", "x": 0, "y": 0,
    "actions": ["set met_hank true"],
    "choices": [
      {"text": "Need a hand?", "next": "offer", "if": "quest:bell == inactive"},
      {"text": "Bell's rung.", "next": "thanks", "if": "quest:bell == completed"},
      {"text": "Just passing.", "next": "", "once": true}]},
   {"id": "offer", "speaker": "hank", "text": "Ring the church bell for me.",
    "actions": ["quest start bell"], "next": "", "x": 320, "y": 0}]}
```

How a conversation runs:

1. **Entering a node** shows its text and runs its `actions`.
2. **Choices:** the node then offers the choices whose `if` condition holds. A choice marked `"once": true` disappears after it has been picked once.
3. **No choices:** if the node has no choices, it continues to `next`. An empty `next` ends the conversation.
4. **Variables in text:** `{var}` is replaced with the value of that story variable.
5. **Editor layout:** `x` and `y` only position the node in the editor graph.

## Conditions and actions

Quests and dialogue use the same small condition and action language.

Conditions:

- `met_hank`, `!met_hank`
- `gold >= 10`, `name == 'Rider'`
- `quest:bell == active`
- `obj:bell/ring >= 1`

Join conditions with `&&` and `||`. There are no parentheses, and `&&` binds tighter than `||`.

Actions:

- `set k v`
- `add k n`
- `quest start|complete|fail id`
- `quest progress q o [n]`
- `event name [arg]`, which fires `on_dialogue_event(name, arg)`
- `save [slot]`
- `end`

## Dialogue box

Story Core adds the built-in Dusk `dialogue` UI document. It shows the speaker's name, the line and the choices, and you can pick a choice with a click or the keyboard. Autosave waits until no dialogue is open.

## Lua (`dialogue`, `story`)

| Call | Does |
|---|---|
| `dialogue.start(id [, node])` | Start a conversation, optionally at a given node. |
| `dialogue.active()` | Whether a conversation is open. |
| `dialogue.choose(i)` | Pick visible choice `i`. Indexing is **1-based**. |
| `dialogue.advance()` / `dialogue.stop()` | Continue past a line that has no choices, or close the conversation. |
| `dialogue.line()` | The current line: speaker, text and choices. |
| `dialogue.last_event()` / `dialogue.list()` | The last `event` action, or the loaded dialogue ids. |
| `story.get(k)` / `story.set(k, v)` / `story.add(k [, n])` | Read and write story variables. |
| `story.format(text)` / `story.check(cond)` / `story.run(action)` | Fill in `{var}`, evaluate a condition, or run an action. |

Hooks:

- `on_dialogue_event(name, number)`
- `on_dialogue_end(dialogue_id)`

These are Lua only.

Example from Ghost Town's `scripts/hank.lua`:

```lua
function on_interact(who)
  if dialogue and not dialogue.active() then dialogue.start("hank") end
end
function on_dialogue_event(name, n)
  if name == "journal" and quests then quests.journal(true) end
end
```

## Editor: Parley panel

Open it from **View → Parley (Dialogue)** in the 2D or 3D editor, including during Play. The panel is in the Dusk theme and has:

- a node graph you can drag and pan;
- a node and choice inspector with fields for speakers, text, conditions and actions;
- lint for missing targets, unreachable nodes and bad conditions;
- a preview that runs the dialogue against scratch variables;
- a marker on the live node while the game is playing.
