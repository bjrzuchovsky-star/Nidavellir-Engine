# In-game UI and fonts (`ts_ui`)

Source: `src/ui/` (`Font.*`, `UiSystem.cpp`, `UiLayout.cpp`, `UiData.cpp`, `Ui.h`), `cmake/FetchFonts.cmake`, `assets/fonts/`.

## Fonts

TTF fonts are rendered with stb_truetype and baked into one 1024×1024 atlas. The engine ships two fonts, both under SIL OFL 1.1 (license texts in `assets/fonts/OFL-*.txt`):

- **Rye**: display font
- **Arvo**: body font

The 2D `ts_game` HUD toasts use stb_easy_font.

## Widgets and layout

- **Widgets:** label, button, image, panel, progress, slider, text input, list.
- **Layout:** anchors, pivots and offsets on a 1280×720 reference that scales with window height. Panels lay out their children as a `stack` or a `grid`.
- **Styles:** `accent`, `muted`, `title`, `clear` (Dusk palette).
- **Navigation:** mouse, keyboard (arrows, Tab, Enter, Esc) and gamepad (d-pad, A, B).

## Data files

Each file in `<project>/ui/*.ui.json` is one document with the fields `name`, `visible`, `modal`, `layer`, `focus` and `root`. Only non-default fields are written. For an example, see `templates/ghost-town-3d/project/ui/main_menu.ui.json`, which `scripts/town.lua` wires up.

## Lua

```lua
ui.show("main_menu"); ui.hide("main_menu")
ui.set_text("title", "Howdy"); ui.set_value("volume", 0.5); ui.set_visible("hint", false)
ui.on_click("play", function(id, value, text) ui.hide("main_menu") end)
ui.on_change("volume", function(id, value, text) audio.set_volume("master", value) end)
```

## Built-in UI, VR and the editor

- **Pause menu:** Esc, or Start on a gamepad, opens Resume, Settings (volume sliders) and Quit.
- **VR:** the UI is drawn on a 0.64 × 0.36 m world-space panel that you click with the laser.
- **Editor:** Play draws the project UI. **View > UI Preview** renders a document without entering Play.

Smoke test: `ts_admin --smoke-ui`.
