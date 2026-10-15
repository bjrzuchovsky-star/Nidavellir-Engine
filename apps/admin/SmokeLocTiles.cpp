#include "SmokeLocTiles.h"

#include "core/Localization.h"
#include "core/PngWrite.h"
#include "editor/tools/AutoTilePanel.h"
#include "editor/tools/SpriteSlicerPanel.h"
#include "editor/tools/StringsPanel.h"
#include "runtime/GraphScript.h"
#include "runtime/ScriptAbi.h"
#include "runtime/World.h"
#include "scene/AutoTile.h"
#include "scene/SpriteSlice.h"
#include "ui/Font.h"
#include "ui/Ui.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ts::tombstone;
using namespace ts::tombstone::runtime;
namespace fs = std::filesystem;

int g_fail = 0;
const char* g_what = "loc";

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "[smoke] " << g_what << " FAILED: " << what << '\n';
    ++g_fail;
  }
}

void write_file(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

fs::path temp_project(const char* name) {
  const fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

Entity2D scripted(std::uint64_t id, const std::string& path) {
  Entity2D e;
  e.id = id;
  e.name = "Teller";
  e.script = ScriptData{};
  e.script->path = path;
  return e;
}

std::vector<std::string> info_lines(World& w) {
  std::vector<std::string> out;
  for (const LogEntry& e : w.take_logs()) {
    if (e.level == LogLevel::Error) out.push_back("ERROR " + e.text);
    else if (e.channel == "script") out.push_back(e.text);
  }
  return out;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

std::string dump(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& l : v) s += "\n  " + l;
  return s;
}

// ---------------------------------------------------------------- loc ----

void loc_tables(const fs::path& proj) {
  // JSON for English, CSV for French + Japanese (CSV quoting, BOM).
  write_file(proj / "strings/en.json",
             "{\"language.name\": \"English\", \"menu.start\": \"Start\", \"hud.gold\": \"Gold: {0}\","
             " \"ui.pause.resume\": \"Resume\", \"only.en\": \"Only English\", \"quote\": \"Say \\\"hi\\\"\\n\"}");
  write_file(proj / "strings/strings.csv",
             "\xEF\xBB\xBFkey,fr,ja\n"
             "language.name,Fran\xC3\xA7" "ais,\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n"
             "menu.start,Commencer,\xE3\x82\xB9\xE3\x82\xBF\xE3\x83\xBC\xE3\x83\x88\n"
             "hud.gold,\"Or : {0}, merci\",\xE9\x87\x91: {0}\n"
             "ui.pause.resume,Reprendre,\xE5\x86\x8D\xE9\x96\x8B\n");
  loc::StringTables t;
  std::string errors;
  check(t.load_project(proj.string(), &errors) == 3, "3 languages load (" + errors + ")");
  check(t.languages() == std::vector<std::string>({"en", "fr", "ja"}), "languages base first");
  const std::string* gold = t.find("fr", "hud.gold");
  check(gold && *gold == "Or : {0}, merci", "CSV quoted field");
  const std::string* q = t.find("en", "quote");
  check(q && *q == "Say \"hi\"\n", "JSON escapes");
  const std::vector<std::string> miss = t.missing("fr");
  check(std::find(miss.begin(), miss.end(), "only.en") != miss.end(), "missing() finds only.en for fr");

  // Round trip through save_project.
  t.set("fr", "only.en", "Seulement");
  std::string err;
  check(t.save_project(proj.string(), &err), "save_project: " + err);
  loc::StringTables back;
  back.load_project(proj.string(), nullptr);
  check(back.find("fr", "only.en") && *back.find("fr", "only.en") == "Seulement", "save/load round trip");
  check(fs::exists(proj / "strings/strings.csv") && fs::exists(proj / "strings/en.json"),
        "CSV stays CSV, JSON stays JSON");

  // Global state + fallback chain.
  loc::load_project(proj.string());
  check(loc::language() == "en" || std::getenv("TS_LANG"), "starts in base language");
  check(loc::set_language("fr"), "set_language fr");
  check(loc::tr("menu.start") == "Commencer", "tr fr");
  check(loc::tr("quote") == "Say \"hi\"\n", "fr falls back to en");
  check(loc::tr("nope.key", "Fallback") == "Fallback", "caller fallback");
  check(loc::tr("nope.key") == "nope.key", "key as last resort");
  check(loc::format(loc::tr("hud.gold"), {"12"}) == "Or : 12, merci", "format {0}");
  check(!loc::set_language("xx"), "unknown language refused");
  check(loc::display_name("ja") == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "display name");
}

void loc_scripts() {
  loc::set_language("fr");
  // Lua: tr(key, ...) and the lang table.
  {
    World w;
    w.script_library().put("scripts/tell.lua",
                           "function on_start()\n"
                           "  log(tr('menu.start'))\n"
                           "  log(tr('hud.gold', 7))\n"
                           "  log(tr('hud.gold', {'8'}))\n"
                           "  log(lang.current(), #lang.list(), lang.name('fr'))\n"
                           "  lang.set('ja')\n"
                           "  log(lang.current(), tr('ui.pause.resume'))\n"
                           "  lang.set('fr')\n"
                           "end\n");
    std::string err;
    check(w.build({scripted(2, "scripts/tell.lua")}, "", &err), "lua build: " + err);
    w.step(InputFrame{});
    const std::vector<std::string> l = info_lines(w);
    for (const char* want : {"Commencer", "Or : 7, merci", "Or : 8, merci",
                             "fr 3 Fran\xC3\xA7" "ais", "ja \xE5\x86\x8D\xE9\x96\x8B"}) {
      check(has(l, want), std::string("lua line '") + want + "'" + dump(l));
    }
  }
  // Graph: text.tr feeding log.
  {
    Graph g;
    GraphNode start;
    start.id = g.next_id();
    start.type = "event.start";
    g.nodes.push_back(start);
    GraphNode t;
    t.id = g.next_id();
    t.type = "text.tr";
    t.inputs = {{"key", ScriptValue::of_text("hud.gold")}, {"arg0", ScriptValue::of_text("9")}};
    g.nodes.push_back(t);
    GraphNode log;
    log.id = g.next_id();
    log.type = "log";
    g.nodes.push_back(log);
    g.links.push_back({start.id, "out", log.id, "in"});
    g.links.push_back({t.id, "value", log.id, "text"});
    World w;
    w.script_library().put("scripts/tell.graph.json", write_graph(g));
    std::string err;
    check(w.build({scripted(2, "scripts/tell.graph.json")}, "", &err), "graph build: " + err);
    w.step(InputFrame{});
    const std::vector<std::string> l = info_lines(w);
    check(has(l, "Or : 9, merci"), "graph text.tr" + dump(l));
  }
  // C ABI 3 (C++ and C# scripts go through these).
  {
    const TsScriptApi a = make_script_api(nullptr);
    check(a.translate && a.lang_set && a.lang_get, "ABI 3 has translate / lang_set / lang_get");
    char buf[64] = {};
    if (a.translate) {
      const int n = a.translate(nullptr, "hud.gold", "3\nx", buf, sizeof(buf));
      const std::string got(buf, static_cast<std::size_t>(std::max(0, n)));
      check(got == "Or : 3, merci", "abi translate: " + got);
    }
    if (a.lang_set && a.lang_get) {
      check(a.lang_set(nullptr, "ja") == 1 && a.lang_set(nullptr, "zz") == 0, "abi lang_set");
      const int n = a.lang_get(nullptr, buf, sizeof(buf));
      check(std::string(buf, static_cast<std::size_t>(std::max(0, n))) == "ja", "abi lang_get");
      a.lang_set(nullptr, "fr");
    }
  }
}

void loc_ui() {
  loc::set_language("en");
  ui::UiSystem u;
  u.set_atlas(&ui::default_atlas());
  ui::Document d;
  check(ui::parse_document(
            R"({"name": "title", "visible": true, "root": {"type": "panel", "id": "root", "children": [
                {"type": "button", "id": "go", "text": "Start!", "text_key": "menu.start"},
                {"type": "label", "id": "gone", "text": "Authored", "text_key": "missing.key"}]}})",
            &d, nullptr),
        "parse text_key document");
  u.add(d);
  ui::Widget* go = u.find("title.go");
  ui::Widget* gone = u.find("title.gone");
  check(go && go->text == "Start", "text_key binds on add");
  check(gone && gone->text == "Authored", "missing key keeps authored text");
  loc::set_language("fr");
  u.update(ui::UiInput{}, 1280, 720);
  check(go && go->text == "Commencer", "language change re-binds on update");
  u.set_text("title.go", "Custom");
  loc::set_language("en");
  u.update(ui::UiInput{}, 1280, 720);
  check(go && go->text == "Custom", "set_text unbinds the key");

  // Pause menu language button cycles en -> fr -> ja.
  ui::PauseMenu::install(u);
  ui::PauseMenu pm;
  pm.open(u, 1, 1, 1);
  ui::Widget* lb = u.find("pause.language");
  check(lb && lb->visible, "language button shows with 3 languages");
  ui::UiEvent ev;
  ev.document = "pause";
  ev.widget = "language";
  std::string bus;
  float v = 0;
  check(pm.handle(u, ev, &bus, &v) == ui::PauseMenu::Action::Language && loc::language() == "fr",
        "language button -> fr");
  check(lb && lb->text.find("Fran") != std::string::npos, "button shows the language name");
  ui::Widget* resume = u.find("pause.resume");
  check(resume && resume->text == "Reprendre", "pause menu is localised");
  pm.handle(u, ev, &bus, &v);
  check(loc::language() == "ja", "language button -> ja");
  pm.handle(u, ev, &bus, &v);
  check(loc::language() == "en", "language button wraps to en");
}

void loc_fonts(const fs::path& proj) {
  // Non-Latin glyphs come from the fallback fonts.
  const std::vector<std::string> fonts = loc::fallback_fonts(proj.string());
  ui::FontAtlas a;
  const std::vector<std::uint32_t> cps = loc::utf8_decode("\xC3\xA7\xE6\x97\xA5\xE6\x9C\xAC");  // ç日本
  check(a.build(cps, fonts), "atlas with extras builds: " + a.error());
  check(a.has_glyph(0xE7), "ç baked");
  bool cjk_font = false;
  for (const std::string& f : fonts) {
    for (const char* n : {"msyh", "YuGoth", "meiryo", "msgothic", "simsun", "CJK", "Droid", "wqy"}) {
      if (f.find(n) != std::string::npos) cjk_font = true;
    }
  }
  if (cjk_font) check(a.has_glyph(0x65E5) && a.missing_glyphs().empty(), "日 from a fallback font");
  else check(a.has_glyph(0x65E5) || !a.missing_glyphs().empty(), "missing glyphs are reported");
  check(a.measure("\xE6\x97\xA5\xE6\x9C\xAC", ui::FontFace::Body, 18) > 0 || !cjk_font, "measure UTF-8");
  std::cout << "[smoke] loc fonts: " << fonts.size() << " fallback font(s), " << a.extra_glyphs()
            << " extra glyphs, " << a.missing_glyphs().size() << " missing, atlas " << a.width() << "px\n";

  // The editor's Strings panel sees keys used in scripts / UI.
  write_file(proj / "scripts/a.lua", "log(tr('menu.start')) log(tr(\"new.key\"))");
  write_file(proj / "ui/t.ui.json", "{\"text_key\": \"ui.title\"}");
  const std::set<std::string> used = editor::StringsPanel::scan_used_keys(proj.string());
  check(used.count("menu.start") && used.count("new.key") && used.count("ui.title"), "Strings panel key scan");
}

// -------------------------------------------------------------- tiles ----

void tiles_core() {
  const auto& masks = blob47_masks();
  std::set<int> uniq(masks.begin(), masks.end());
  check(uniq.size() == 47, "47 unique blob masks");
  int reduced = 0;
  for (int m = 0; m < 256; ++m) reduced += blob_reduce(static_cast<std::uint8_t>(m)) == m;
  check(reduced == 47, "exactly 47 of 256 masks are already reduced");
  check(blob47_slot(0) == 0 && blob47_slot(0xFF) == 46, "blob slots: isolated 0, full 46");
  check(blob47_slot(kNbN | kNbNE) == blob47_slot(kNbN), "lone corner ignored");
  check(wang16_slot(kNbN | kNbE | kNbNE) == 3 && wang16_slot(0xFF) == 15, "wang slots");

  AutoTileSet blob;
  blob.edges_connect = false;
  TileMapData m(7, 7, 32);
  // A plus sign.
  for (int i = 1; i <= 5; ++i) {
    paint_autotile(m, blob, 3, i, 1, false);
    paint_autotile(m, blob, i, 3, 1, false);
  }
  check(m.at(3, 3) == blob.slot_tile(blob47_slot(kNbN | kNbE | kNbS | kNbW)), "plus centre");
  check(m.at(3, 1) == blob.slot_tile(blob47_slot(kNbS)), "plus top arm end");
  check(m.at(0, 0) == 0, "untouched cells stay empty");
  paint_autotile(m, blob, 3, 2, 1, true);  // erase above the centre
  check(m.at(3, 1) == blob.slot_tile(0) && m.at(3, 3) == blob.slot_tile(blob47_slot(kNbE | kNbS | kNbW)),
        "erase re-tiles neighbours");
  m.set(5, 5, blob.full_tile());
  check(retile_all(m, blob) >= 1 && m.at(5, 5) == blob.slot_tile(0), "retile_all fixes a raw tile");

  AutoTileSet w;
  w.mode = AutoTileMode::Wang16;
  w.base = 100;
  w.edges_connect = false;
  TileMapData m2(4, 4, 16);
  paint_autotile(m2, w, 1, 1, 2, false);  // 2x2 brush
  check(m2.at(1, 1) == 100 + 2 + 4 && m2.at(2, 2) == 100 + 1 + 8, "wang 2x2 block");
  check(w.member(115) && !w.member(99), "wang membership");

  // JSON round trip.
  AutoTileSet r;
  r.mode = AutoTileMode::Rules;
  r.name = "Fence";
  r.rules = {{5, ".x./.-./.x."}, {6, ".../x-x/..."}, {7, ".../.-./..."}};
  r.members = {9};
  AutoTileSet back;
  std::string err;
  check(autotile_json::parse(autotile_json::write(r), &back, &err) && back == r, "autotile json round trip " + err);
  check(r.resolve(kNbN | kNbS) == 5 && r.resolve(kNbE | kNbW) == 6 && r.resolve(0) == 7, "rule resolve");
}

void tiles_project(const fs::path& proj) {
  const unsigned char fill[4] = {100, 160, 60, 255}, rim[4] = {60, 100, 30, 255};
  std::string err;
  const std::string a =
      editor::AutoTilePanel::create_terrain(proj.string(), "Grass Hill", AutoTileMode::Blob47, 16, fill, rim, &err);
  const std::string b =
      editor::AutoTilePanel::create_terrain(proj.string(), "Road", AutoTileMode::Wang16, 16, fill, rim, &err);
  const std::string c =
      editor::AutoTilePanel::create_terrain(proj.string(), "Fence", AutoTileMode::Rules, 16, fill, rim, &err);
  check(a == "tiles/grass_hill.autotile.json" && !b.empty() && !c.empty(), "create terrains: " + err);
  std::vector<unsigned char> px;
  int iw = 0, ih = 0;
  check(editor::TextureCache::decode_file((proj / "assets/tiles/grass_hill_blob47.png").string(), &px, &iw, &ih, &err),
        "blob template decodes");
  check(iw > 0 && tileset_tile_count(16, iw, ih) >= 47, "blob template has 47 tiles");
  check(autotile_json::list(proj.string()).size() == 3, "list finds 3 terrains");

  editor::AutoTilePanel& p = editor::AutoTilePanel::get();
  p.open(proj.string(), "tiles");
  p.close();
  check(p.select_brush(a) && p.brush(proj.string()) && p.brush(proj.string())->mode == AutoTileMode::Blob47,
        "panel brush selection");
  check(!p.brush("/elsewhere"), "brush is per project");
  const AutoTileSet* rules = p.set(c);
  check(rules && rules->rules.size() == 16, "rules terrain has 16 edge rules");
  if (rules) {
    AutoTileSet rs = *rules;
    rs.edges_connect = false;
    TileMapData m(5, 3, 16);
    for (int i = 0; i < 5; ++i) paint_autotile(m, rs, i, 1, 1, false);
    check(m.at(0, 1) == 1 + 2 && m.at(2, 1) == 1 + 2 + 8 && m.at(4, 1) == 1 + 8, "rule tiles pick by edges");
  }
  p.paint_demo();
  p.clear_brush();
}

void tiles_slicer(const fs::path& proj) {
  // 4 x 2 sheet of 16 px frames; cell (3, 1) empty; each sprite a 10x11 blob
  // plus a detached 2x2 "hat" with a 1 px gutter above it.
  const int W = 64, H = 32;
  std::vector<unsigned char> rgba(static_cast<std::size_t>(W) * H * 4, 0);
  auto dot = [&](int x, int y) {
    unsigned char* q = &rgba[(static_cast<std::size_t>(y) * W + x) * 4];
    q[0] = 200; q[1] = 120; q[2] = 40; q[3] = 255;
  };
  for (int cell = 0; cell < 7; ++cell) {
    const int ox = (cell % 4) * 16, oy = (cell / 4) * 16;
    for (int y = 3; y < 14; ++y)
      for (int x = 3; x < 13; ++x) dot(ox + x, oy + y);
    for (int y = 0; y < 2; ++y) {
      dot(ox + 7, oy + y);
      dot(ox + 8, oy + y);
    }
  }
  slicer::GridSpec g;
  g.frame_w = g.frame_h = 16;
  const std::vector<AnimRect> grid = slicer::grid_rects(rgba.data(), W, H, g);
  check(grid.size() == 7, "grid skips the empty cell (" + std::to_string(grid.size()) + ")");
  slicer::DetectSpec d;
  d.merge_gap = 1;
  const std::vector<AnimRect> det = slicer::detect_rects(rgba.data(), W, H, d);
  check(det.size() == 7, "auto-detect finds 7 sprites (" + std::to_string(det.size()) + ")");
  if (det.size() == 7) {
    check(det[0] == AnimRect{3, 0, 10, 14}, "detected rect merges the hat");
    check(slicer::row_of(det)[4] == 1, "row grouping");
  }
  d.merge_gap = 0;
  check(slicer::detect_rects(rgba.data(), W, H, d).size() == 14, "no merge: hats separate");
  const std::vector<slicer::ClipPick> clips = slicer::clips_per_row(det, {"walk", "jump"});
  check(clips.size() == 2 && clips[0].name == "walk" && clips[0].count == 4 && clips[1].count == 3, "clips per row");
  const AnimSet gs = slicer::build_set("hero.png", grid, slicer::clips_per_row(grid), &g, W, H);
  check(gs.grid.frame_w == 16 && gs.grid.cols == 4 && gs.clips.size() == 2 && gs.clips[0].grid,
        "grid build_set uses grid clips");
  const AnimSet ds = slicer::build_set("hero.png", det, clips, nullptr, W, H);
  check(ds.clips.size() == 2 && !ds.clips[1].grid && ds.clips[1].rects.size() == 3 &&
            ds.frame_rect(ds.clips[1], 0) == det[4],
        "detect build_set uses explicit rects");

  // Through the panel: load, auto-detect, save .anim.json, load it back.
  check(write_png_rgba((proj / "assets/hero.png").string(), W, H, rgba), "write sheet");
  editor::SpriteSlicerPanel& sp = editor::SpriteSlicerPanel::get();
  sp.open(proj.string(), "assets/hero.png");
  sp.grid().frame_w = sp.grid().frame_h = 16;
  sp.reslice();
  check(sp.rects().size() == 7, "panel grid slice");
  sp.set_auto_detect(true);
  check(sp.rects().size() == 7 && sp.clips().size() == 2, "panel auto-detect");
  std::string err;
  check(sp.save(&err), "panel save: " + err);
  AnimSet loaded;
  check(anim_json::load_file((proj / sp.anim_path()).string(), &loaded, &err) && loaded.clips.size() == 2 &&
            loaded.image == "hero.png",
        "saved .anim.json loads (" + sp.anim_path() + ") " + err);
  sp.set_auto_detect(false);
}

}  // namespace

int run_loc_smoke() {
  g_fail = 0;
  g_what = "loc";
  const fs::path proj = temp_project("ts_smoke_loc");
  const loc::StringTables saved_tables = loc::tables();
  loc_tables(proj);
  loc_scripts();
  loc_ui();
  loc_fonts(proj);
  loc::set_tables(saved_tables);
  loc::set_tables(loc::StringTables{});
  std::error_code ec;
  fs::remove_all(proj, ec);
  if (g_fail) return 1;
  std::cout << "[smoke] loc OK (JSON + CSV string tables, save round trip, missing keys, fallback "
               "chain, {0} args, Lua tr/lang, graph text.tr, ABI 3 translate/lang, UI text_key "
               "binding + re-bind, pause-menu language button, fallback-font glyphs, Strings key scan)\n";
  return 0;
}

int run_tiles_smoke() {
  g_fail = 0;
  g_what = "tiles";
  const fs::path proj = temp_project("ts_smoke_tiles");
  tiles_core();
  tiles_project(proj);
  tiles_slicer(proj);
  std::error_code ec;
  fs::remove_all(proj, ec);
  if (g_fail) return 1;
  std::cout << "[smoke] tiles OK (47-blob masks + slots, Wang 16, rule tiles, paint / erase / "
               "retile, terrain templates + .autotile.json, brush per project, sprite slicer grid + "
               "auto-detect + hat merge + clips per row + .anim.json)\n";
  return 0;
}
