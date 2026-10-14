#include "SmokeUi.h"

#include "core/MemImages.h"
#include "ui/Font.h"
#include "runtime/World.h"
#include "ui/Ui.h"
#include "ui/UiScript.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace ui = ts::tombstone::ui;
namespace mem = ts::tombstone::mem_images;
namespace rt = ts::tombstone::runtime;
namespace tsb = ts::tombstone;

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "ui smoke FAILED: " << what << "\n";
    ++g_fail;
  }
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

const char* kDoc = R"({
  "name": "menu", "visible": true, "modal": true, "focus": "play",
  "root": {"type": "panel", "id": "root", "anchor": [0.5, 0.5], "pivot": [0.5, 0.5],
    "size": [400, 0], "layout": "stack", "padding": 20, "gap": 10, "children": [
      {"type": "label", "id": "title", "text": "Tombstone", "font": "display", "font_size": 40,
       "anchor": [0.5, 0]},
      {"type": "button", "id": "play", "text": "Play", "stretch": [true, false]},
      {"type": "slider", "id": "vol", "text": "Volume", "value": 0.5, "step": 0.1,
       "stretch": [true, false]},
      {"type": "input", "id": "name", "placeholder": "Your name", "stretch": [true, false],
       "max_length": 8},
      {"type": "list", "id": "maps", "items": ["Dry Gulch", "Red Mesa", "Boot Hill"],
       "selected": 0, "stretch": [true, false]},
      {"type": "panel", "id": "grid", "style": "clear", "layout": "grid", "columns": 2,
       "gap": 4, "stretch": [true, false], "children": [
        {"type": "button", "id": "g1", "text": "A"}, {"type": "button", "id": "g2", "text": "B"},
        {"type": "button", "id": "g3", "text": "C"}, {"type": "progress", "id": "hp", "value": 0.25}
      ]}
  ]}
})";

}  // namespace

int run_ui_smoke() {
  g_fail = 0;
  const ui::FontAtlas& atlas = ui::default_atlas();
  check(atlas.ok(), "font atlas bakes (" + atlas.error() + ")");
  check(atlas.width() == 1024 && atlas.rgba().size() == 1024u * 1024u * 4u, "atlas size");
  const mem::MemImage* m = mem::get(ui::kAtlasKey);
  check(m && m->width == 1024 && m->version > 0, "atlas published as mem:ui/atlas");
  {
    // White texel for solid fills.
    const int wx = static_cast<int>(atlas.white_u() * atlas.width());
    const int wy = static_cast<int>(atlas.white_v() * atlas.height());
    check(atlas.rgba()[(static_cast<std::size_t>(wy) * atlas.width() + wx) * 4 + 3] == 255,
          "white texel is opaque");
  }

  // Text measure.
  const float w1 = atlas.measure("Howdy", ui::FontFace::Body, 18);
  const float w2 = atlas.measure("Howdy partner", ui::FontFace::Body, 18);
  const float w36 = atlas.measure("Howdy", ui::FontFace::Body, 36);
  check(w1 > 20 && w1 < 90, "measure Howdy@18 = " + std::to_string(w1));
  check(w2 > w1, "longer text measures wider");
  check(w36 > w1 * 1.7f && w36 < w1 * 2.3f,
        "measure scales with size");
  check(atlas.measure("", ui::FontFace::Body, 18) == 0, "empty text has no width");
  check(atlas.measure("ab\ncd", ui::FontFace::Body, 18) ==
            atlas.measure("ab", ui::FontFace::Body, 18),
        "measure stops at newline");
  check(atlas.line_height(ui::FontFace::Display, 40) > 30, "display line height");
  const std::vector<std::string> lines =
      atlas.wrap("the quick brown fox jumps over the lazy dog", ui::FontFace::Body, 18, 120);
  check(lines.size() >= 3, "wrap breaks long text");
  for (const std::string& l : lines) {
    check(atlas.measure(l, ui::FontFace::Body, 18) <= 120 || l.find(' ') == std::string::npos,
          "wrapped line fits: " + l);
  }
  std::vector<ui::UiQuad> q;
  const float white[4] = {1, 1, 1, 1};
  atlas.text_quads("A b", ui::FontFace::Body, 18, 10, 10, white, &q);
  check(q.size() == 2, "space has no quad");

  // Layout math.
  ui::Document doc;
  std::string err;
  check(ui::parse_document(kDoc, &doc, &err), "parse document: " + err);
  ui::layout(doc, 1280, 720, atlas);
  const ui::Widget& root = doc.root;
  check(near(root.rect.w, 400) && near(root.rect.x, 440), "root centred at 1280x720");
  check(near(root.rect.y + root.rect.h * 0.5f, 360, 0.6f), "root centred vertically");
  const ui::Widget* play = ui::find_widget(doc.root, "play");
  const ui::Widget* title = ui::find_widget(doc.root, "title");
  check(play && near(play->rect.x, 460) && near(play->rect.w, 360), "stretch fills content box");
  check(title && near(title->rect.x + title->rect.w * 0.5f, 640, 0.6f), "anchor 0.5 centres");
  check(title && play && near(play->rect.y, title->rect.y + title->rect.h + 10), "stack gap");
  const ui::Widget* g1 = ui::find_widget(doc.root, "g1");
  const ui::Widget* g2 = ui::find_widget(doc.root, "g2");
  const ui::Widget* g3 = ui::find_widget(doc.root, "g3");
  check(g1 && g2 && g3 && near(g1->rect.w, 178) && near(g2->rect.x, g1->rect.x + 182),
        "grid columns");
  check(g1 && g3 && near(g3->rect.x, g1->rect.x) && g3->rect.y > g1->rect.y, "grid wraps");
  ui::layout(doc, 2560, 1440, atlas);
  check(near(doc.root.rect.w, 800) && near(doc.root.rect.x, 880), "layout scales 2x");
  ui::layout(doc, 1280, 720, atlas);

  // Data round trip.
  const std::string text = ui::write_document(doc);
  ui::Document back;
  check(ui::parse_document(text, &back, &err), "re-parse written document: " + err);
  check(ui::write_document(back) == text, "write -> parse -> write is stable");
  const ui::Widget* bm = ui::find_widget(back.root, "maps");
  check(bm && bm->items.size() == 3 && bm->items[1] == "Red Mesa" && back.modal &&
            back.focus == "play" && bm->stretch[0],
        "round trip keeps fields");
  check(!ui::parse_document(R"({"root": {"type": "wheel"}})", &back, &err) &&
            err.find("wheel") != std::string::npos,
        "unknown widget type rejected");

  // Navigation + events.
  ui::UiSystem sys;
  sys.add(doc);
  sys.hide("menu");
  sys.show("menu");
  check(sys.modal_open() && sys.focus() == "play", "modal show focuses its focus widget");
  auto step = [&](ui::UiInput in) { return sys.update(in, 1280, 720); };
  ui::UiInput none;
  step(none);
  ui::UiInput down;
  down.down = true;
  step(down);
  check(sys.focus() == "vol", "down -> slider");
  ui::UiInput right;
  right.right = true;
  std::vector<ui::UiEvent> ev = step(right);
  check(ev.size() == 1 && ev[0].kind == ui::UiEventKind::Change && near(ev[0].value, 0.6f),
        "right nudges slider by step");
  step(down);
  check(sys.focus() == "name" && sys.wants_text(), "down -> text input");
  ui::UiInput typing;
  typing.text = "Calamity Jane";
  ev = step(typing);
  check(sys.find("name") && sys.find("name")->text == "Calamity", "typing respects max_length");
  ui::UiInput bs;
  bs.backspace = true;
  step(bs);
  check(sys.find("name")->text == "Calamit", "backspace");
  step(down);
  check(sys.focus() == "maps", "down -> list");
  ev = step(down);
  check(!ev.empty() && ev[0].text == "Red Mesa", "down moves list selection");
  step(down);
  step(down);
  check(sys.focus() == "g1", "down past list end leaves it");
  step(right);
  check(sys.focus() == "g2", "right in grid");
  step(down);
  check(sys.focus() == "g3" || sys.focus() == "play", "down from grid row");
  sys.set_focus("menu", "play");
  ui::UiInput accept;
  accept.accept = true;
  ev = step(accept);
  check(ev.size() == 1 && ev[0].kind == ui::UiEventKind::Click && ev[0].widget == "play",
        "accept clicks the focused button");
  ui::UiInput back_in;
  back_in.back = true;
  ev = step(back_in);
  check(ev.size() == 1 && ev[0].kind == ui::UiEventKind::Back && ev[0].document == "menu",
        "back event");

  // Mouse: click a button, drag a slider.
  const ui::Widget* pw = sys.find("menu.play");
  ui::UiInput mouse;
  mouse.mouse_x = pw->rect.x + 5;
  mouse.mouse_y = pw->rect.y + 5;
  mouse.mouse_down = true;
  step(mouse);
  mouse.mouse_down = false;
  ev = step(mouse);
  check(ev.size() == 1 && ev[0].widget == "play" && ev[0].kind == ui::UiEventKind::Click,
        "mouse click");
  const ui::Widget* vs = sys.find("vol");
  const ui::Rect tr = ui::slider_track(*vs, 1.0f);
  mouse.mouse_x = tr.x + tr.w;  // far right
  mouse.mouse_y = vs->rect.y + vs->rect.h * 0.5f;
  mouse.mouse_down = true;
  ev = step(mouse);
  check(!ev.empty() && near(sys.find("vol")->value, 1.0f), "slider drag to the end");
  mouse.mouse_down = false;
  step(mouse);

  // Draw + raster.
  std::vector<ui::UiDraw> draws;
  sys.draw(&draws);
  check(draws.size() > 40, "menu draws quads (" + std::to_string(draws.size()) + ")");
  std::vector<unsigned char> px;
  ui::raster(draws, atlas, 1280, 720, &px);
  const auto alpha = [&](int x, int y) { return px[(static_cast<std::size_t>(y) * 1280 + x) * 4 + 3]; };
  check(alpha(5, 5) == 0, "raster: outside is transparent");
  check(alpha(static_cast<int>(pw->rect.x + 3), static_cast<int>(pw->rect.y + 3)) > 200,
        "raster: button is opaque");

  // Pause menu.
  ui::UiSystem game;
  ui::PauseMenu pause;
  pause.open(game, 0.8f, 0.5f, 1.0f);
  check(game.visible("pause") && game.modal_open() && game.focus() == "resume", "pause opens");
  game.update(none, 1280, 720);
  check(game.find("pause.vol_music") && near(game.find("pause.vol_music")->value, 0.5f),
        "pause sliders take current volumes");
  std::string bus;
  float vol = 0;
  ui::UiEvent click;
  click.kind = ui::UiEventKind::Click;
  click.document = "pause";
  click.widget = "settings";
  check(pause.handle(game, click, &bus, &vol) == ui::PauseMenu::Action::None &&
            game.find("pause.pause_settings")->visible && game.focus() == "vol_master",
        "settings view");
  ev = game.update(right, 1280, 720);
  check(ev.size() == 1 && pause.handle(game, ev[0], &bus, &vol) == ui::PauseMenu::Action::Volume &&
            bus == "master" && near(vol, 0.85f),
        "volume slider -> Volume action");
  ev = game.update(back_in, 1280, 720);
  check(ev.size() == 1 && pause.handle(game, ev[0], &bus, &vol) == ui::PauseMenu::Action::None &&
            game.find("pause.pause_main")->visible,
        "back leaves settings");
  ev = game.update(back_in, 1280, 720);
  check(ev.size() == 1 && pause.handle(game, ev[0], &bus, &vol) == ui::PauseMenu::Action::Resume &&
            !game.visible("pause"),
        "back again resumes");
  pause.open(game, 1, 1, 1);
  click.widget = "quit";
  check(pause.handle(game, click, &bus, &vol) == ui::PauseMenu::Action::Quit, "quit");

  // Memory images.
  const std::uint64_t v1 = mem::put("mem:smoke/x", 2, 2, std::vector<unsigned char>(16, 7));
  const std::uint64_t v2 = mem::put("mem:smoke/x", 2, 2, std::vector<unsigned char>(16, 9));
  check(v2 > v1 && mem::get("mem:smoke/x")->rgba[0] == 9 && mem::is_key("mem:smoke/x") &&
            !mem::is_key("assets/x.png"),
        "mem images versioned");
  mem::remove("mem:smoke/x");
  check(mem::get("mem:smoke/x") == nullptr, "mem image removed");

  // Lua ui.*: commands reach the host's UiSystem, events reach handlers.
  {
    rt::World w;
    w.script_library().put("scripts/menu.lua",
                           "function on_start()\n"
                           "  ui.show('menu')\n"
                           "  ui.set_text('menu.title', 'Dry Gulch')\n"
                           "  ui.set_value('hp', 0.75)\n"
                           "  ui.set_visible('g3', false)\n"
                           "  ui.hide('nope')\n"
                           "  ui.on_click('play', function(id) self:set('clicked', id) end)\n"
                           "  ui.on_change('vol', function(id, v) self:set('vol', v) end)\n"
                           "  ui.on_click('name', function(id, v, t) self:set('name', t) end)\n"
                           "  local ok = pcall(ui.on_click, 'x', 5)\n"
                           "  self:set('bad_refused', not ok)\n"
                           "end\n");
    tsb::Entity2D e;
    e.id = 7;
    e.name = "Menu";
    e.w = e.h = 16;
    e.script = tsb::ScriptData{};
    e.script->path = "scripts/menu.lua";
    std::string berr;
    check(w.build({e}, "", &berr), "lua ui world builds: " + berr);
    w.step(rt::InputFrame{});
    std::vector<rt::UiCommand> cmds = w.take_ui_commands();
    check(cmds.size() == 5, "lua queued 5 ui commands, got " + std::to_string(cmds.size()));
    ui::UiSystem host;
    ui::Document d2;
    ui::parse_document(kDoc, &d2, &err);
    d2.visible = false;
    host.add(d2);
    ui::apply_ui_commands(host, cmds);
    check(host.visible("menu") && host.find("title")->text == "Dry Gulch" &&
              near(host.find("hp")->value, 0.75f) && !host.find("g3")->visible,
          "ui commands applied");
    ui::UiEvent c1;
    c1.kind = ui::UiEventKind::Click;
    c1.widget = "play";
    ui::UiEvent c2;
    c2.kind = ui::UiEventKind::Change;
    c2.widget = "vol";
    c2.value = 0.3f;
    ui::UiEvent c3;
    c3.kind = ui::UiEventKind::Submit;
    c3.widget = "name";
    c3.text = "Jane";
    ui::forward_ui_events(w, {c1, c2, c3});
    const tsb::ScriptValue* clicked = w.script_get(7, "clicked");
    const tsb::ScriptValue* vol = w.script_get(7, "vol");
    const tsb::ScriptValue* name = w.script_get(7, "name");
    const tsb::ScriptValue* bad = w.script_get(7, "bad_refused");
    check(clicked && clicked->text == "play", "ui.on_click handler ran");
    check(vol && near(static_cast<float>(vol->number), 0.3f), "ui.on_change gets the value");
    check(name && name->text == "Jane", "submit reaches on_click with the text");
    check(bad && bad->flag, "ui.on_click needs a function");
  }

  if (g_fail == 0) {
    std::cout << "[smoke] ui OK (atlas " << atlas.width() << "x" << atlas.height()
              << ", measure, layout, nav, round trip, pause menu, raster, lua ui.*)\n";
  }
  return g_fail == 0 ? 0 : 1;
}
