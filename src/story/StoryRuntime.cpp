#include "story/StoryRuntime.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {
namespace story {

namespace {

constexpr int kMaxChoices = 4;
std::atomic<StoryRuntime*> g_latest{nullptr};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string clock_text(double seconds) {
  const long long s = static_cast<long long>(seconds);
  char buf[32];
  if (s >= 3600) {
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
  } else {
    std::snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
  }
  return buf;
}

}  // namespace

StorySettings load_story_settings(const std::string& project_dir) {
  StorySettings s;
  const fs::path p = fs::path(project_dir) / "story.json";
  std::error_code ec;
  if (project_dir.empty() || !fs::exists(p, ec)) return s;
  json::Value v;
  if (!json::parse(slurp(p), &v, nullptr)) return s;
  s.autosave_seconds = std::max(0.0, v.num("autosave_seconds", s.autosave_seconds));
  s.autosave_on_quest = v.flag("autosave_on_quest", s.autosave_on_quest);
  s.autosave_slots = std::clamp(static_cast<int>(v.num("autosave_slots", s.autosave_slots)), 1, 10);
  s.tracker = v.flag("tracker", s.tracker);
  s.toasts = v.flag("toasts", s.toasts);
  return s;
}

std::string wrap_text(const std::string& text, std::size_t width) {
  std::string out, line, word;
  auto flush_word = [&] {
    if (word.empty()) return;
    if (!line.empty() && line.size() + 1 + word.size() > width) {
      out += line + "\n";
      line.clear();
    }
    line += (line.empty() ? "" : " ") + word;
    word.clear();
  };
  for (char c : text) {
    if (c == '\n') {
      flush_word();
      out += line + "\n";
      line.clear();
    } else if (c == ' ') {
      flush_word();
    } else {
      word += c;
    }
  }
  flush_word();
  out += line;
  return out;
}

std::string builtin_dialogue_ui() {
  return R"({
  "name": "dialogue", "visible": false, "modal": true, "layer": 60, "focus": "dlg_next",
  "root": {
    "type": "panel", "id": "dlg_root", "anchor": [0.5, 1], "pivot": [0.5, 1], "offset": [0, -28],
    "size": [940, 0], "layout": "stack", "padding": 22, "gap": 10,
    "children": [
      {"type": "label", "id": "dlg_speaker", "text": "", "font": "display", "font_size": 30, "style": "title"},
      {"type": "label", "id": "dlg_text", "text": "", "font_size": 20},
      {"type": "button", "id": "dlg_choice_1", "text": "", "font_size": 19, "stretch": [true, false], "visible": false},
      {"type": "button", "id": "dlg_choice_2", "text": "", "font_size": 19, "stretch": [true, false], "visible": false},
      {"type": "button", "id": "dlg_choice_3", "text": "", "font_size": 19, "stretch": [true, false], "visible": false},
      {"type": "button", "id": "dlg_choice_4", "text": "", "font_size": 19, "stretch": [true, false], "visible": false},
      {"type": "button", "id": "dlg_next", "text": "Continue", "font_size": 19, "style": "accent", "stretch": [true, false]}
    ]
  }
})";
}

std::string builtin_journal_ui() {
  return R"({
  "name": "journal", "visible": false, "modal": true, "layer": 55, "focus": "jr_list",
  "root": {
    "type": "panel", "id": "jr_root", "anchor": [0.5, 0.5], "pivot": [0.5, 0.5],
    "size": [980, 560], "layout": "stack", "padding": 24, "gap": 12,
    "children": [
      {"type": "label", "text": "Bounty Board", "font": "display", "font_size": 40, "style": "title"},
      {"type": "panel", "id": "jr_body", "layout": "row", "gap": 18, "style": "clear", "stretch": [true, false],
       "children": [
        {"type": "list", "id": "jr_list", "size": [330, 380], "font_size": 18},
        {"type": "panel", "id": "jr_detail", "layout": "stack", "gap": 8, "style": "clear", "size": [560, 380],
         "children": [
          {"type": "label", "id": "jr_title", "text": "", "font": "display", "font_size": 28},
          {"type": "label", "id": "jr_meta", "text": "", "style": "muted", "font_size": 16},
          {"type": "label", "id": "jr_desc", "text": "", "font_size": 18},
          {"type": "label", "id": "jr_objectives", "text": "", "font_size": 18}
         ]}
       ]},
      {"type": "panel", "layout": "row", "gap": 12, "style": "clear", "children": [
        {"type": "button", "id": "jr_track", "text": "Track", "font_size": 18, "style": "accent"},
        {"type": "button", "id": "jr_close", "text": "Close", "font_size": 18}
      ]}
    ]
  }
})";
}

std::string builtin_tracker_ui() {
  return R"({
  "name": "quest_tracker", "visible": false, "modal": false, "layer": 10,
  "root": {
    "type": "panel", "id": "qt_root", "anchor": [1, 0], "pivot": [1, 0], "offset": [-18, 18],
    "size": [380, 0], "layout": "stack", "padding": 14, "gap": 4, "style": "dark",
    "children": [
      {"type": "label", "id": "qt_title", "text": "", "font": "display", "font_size": 22, "style": "title"},
      {"type": "label", "id": "qt_text", "text": "", "font_size": 16}
    ]
  }
})";
}

std::string builtin_save_menu_ui() {
  return R"({
  "name": "save_menu", "visible": false, "modal": true, "layer": 58, "focus": "sv_list",
  "root": {
    "type": "panel", "id": "sv_root", "anchor": [0.5, 0.5], "pivot": [0.5, 0.5],
    "size": [760, 520], "layout": "stack", "padding": 24, "gap": 12,
    "children": [
      {"type": "label", "text": "Strongbox", "font": "display", "font_size": 40, "style": "title"},
      {"type": "label", "id": "sv_note", "text": "Pick a slot.", "style": "muted", "font_size": 16},
      {"type": "list", "id": "sv_list", "size": [700, 300], "font_size": 18},
      {"type": "panel", "layout": "row", "gap": 12, "style": "clear", "children": [
        {"type": "button", "id": "sv_save", "text": "Save", "font_size": 18, "style": "accent"},
        {"type": "button", "id": "sv_load", "text": "Load", "font_size": 18},
        {"type": "button", "id": "sv_delete", "text": "Delete", "font_size": 18},
        {"type": "button", "id": "sv_close", "text": "Close", "font_size": 18}
      ]}
    ]
  }
})";
}

StoryRuntime::StoryRuntime() = default;

StoryRuntime::~StoryRuntime() {
  StoryRuntime* self = this;
  g_latest.compare_exchange_strong(self, nullptr);
}

StoryRuntime* StoryRuntime::latest() { return g_latest.load(); }

void StoryRuntime::start(StoryHost host, StoryFeatures features) {
  host_ = std::move(host);
  features_ = features;
  settings = load_story_settings(host_.project_dir);
  running_ = true;
  playtime_ = 0.0;
  since_autosave_ = 0.0;
  load_errors_.clear();
  vars.clear();
  quests = QuestLog{};
  dialogue = DialogueRunner{};
  if (features_.quests) {
    std::string errs;
    quests.load_dir(host_.project_dir, &errs);
    load_errors_ += errs;
    for (const std::string& l : lint_quests([&] {
           std::vector<QuestDef> v;
           for (const QuestDef* q : quests.defs()) v.push_back(*q);
           return v;
         }())) {
      load_errors_ += l + "\n";
    }
    quests.run_action = [this](const std::string& a) { run_action(a); };
  }
  if (features_.dialogue) {
    std::string errs;
    dialogue.load_dir(host_.project_dir, &errs);
    load_errors_ += errs;
    dialogue.vars = &vars;
    dialogue.run_action = [this](const std::string& a) { run_action(a); };
    dialogue.env.vars = &vars;
    if (features_.quests) {
      dialogue.env.quest_status = [this](const std::string& q) { return std::string(quest_status_name(quests.status(q))); };
      dialogue.env.objective = [this](const std::string& q, const std::string& o) {
        return quests.objective_progress(q, o);
      };
    }
  }
  if (!load_errors_.empty() && host_.log) {
    std::istringstream in(load_errors_);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty()) host_.log(1, line);
    }
  }
  // Saves: sections for what is on.
  saves = SaveStore(host_.project_dir);
  saves.autosave_slots = settings.autosave_slots;
  saves.scene = host_.scene_name;
  saves.provide({"vars", [this] { return vars.to_json(); }, [this](const json::Value& v) { vars.from_json(v); }});
  if (features_.quests) {
    saves.provide({"quests", [this] { return quests.to_json(); },
                   [this](const json::Value& v) {
                     quests.from_json(v);
                     quest_dirty_ = true;
                   }});
  }
  if (features_.dialogue) {
    saves.provide({"dialogue", [this] { return dialogue.to_json(); },
                   [this](const json::Value& v) {
                     dialogue.from_json(v);
                     dlg_dirty_ = true;
                   }});
  }
  if (host_.get_player && host_.set_player) {
    saves.provide({"player",
                   [this] {
                     float p[3] = {0, 0, 0};
                     json::Value v = json::Value::object();
                     if (host_.get_player(p)) v.set("position", json::floats(p, 3));
                     return v;
                   },
                   [this](const json::Value& v) {
                     float p[3];
                     if (v.floats("position", p, 3)) host_.set_player(p);
                   }});
  }
  // Built-in UI (a project's own ui/<name>.ui.json wins).
  auto add_doc = [this](const char* name, const std::string& text) {
    runtime::UiCommand c;
    c.kind = runtime::UiCommand::Kind::AddDocument;
    c.target = name;
    c.text = text;
    post(std::move(c));
  };
  if (features_.dialogue) add_doc("dialogue", builtin_dialogue_ui());
  if (features_.quests) {
    add_doc("journal", builtin_journal_ui());
    add_doc("quest_tracker", builtin_tracker_ui());
  }
  if (features_.saves) add_doc("save_menu", builtin_save_menu_ui());
  dlg_dirty_ = quest_dirty_ = true;
  g_latest = this;
}

void StoryRuntime::stop() {
  running_ = false;
  dialogue.stop();
  journal_open_ = save_menu_open_ = false;
  StoryRuntime* self = this;
  g_latest.compare_exchange_strong(self, nullptr);
}

void StoryRuntime::post(runtime::UiCommand c) {
  ++ui_posts_;
  if (host_.ui) host_.ui(std::move(c));
}

void StoryRuntime::ui_text(const std::string& id, const std::string& text) {
  runtime::UiCommand c;
  c.kind = runtime::UiCommand::Kind::SetText;
  c.target = id;
  c.text = text;
  post(std::move(c));
}

void StoryRuntime::ui_visible(const std::string& id, bool on) {
  runtime::UiCommand c;
  c.kind = runtime::UiCommand::Kind::SetVisible;
  c.target = id;
  c.value = on ? 1.0 : 0.0;
  post(std::move(c));
}

void StoryRuntime::ui_show(const std::string& doc, bool on) {
  runtime::UiCommand c;
  c.kind = on ? runtime::UiCommand::Kind::Show : runtime::UiCommand::Kind::Hide;
  c.target = doc;
  post(std::move(c));
}

void StoryRuntime::say(const std::string& text, double seconds) {
  if (settings.toasts && host_.toast) host_.toast(text, seconds);
}

void StoryRuntime::hook(const char* name, const std::string& text, double number) {
  ++hooks_fired_;
  if (host_.broadcast) host_.broadcast(name, text, number);
}

// ---- Actions -----------------------------------------------------------------

void StoryRuntime::run_action(const std::string& text) {
  Action a;
  std::string err;
  if (!parse_action(text, &a, &err)) {
    if (host_.log) host_.log(1, err);
    return;
  }
  if (a.verb == "set") {
    vars.set(a.args[0], VarValue::parse(a.args[1]));
  } else if (a.verb == "add") {
    vars.add(a.args[0], VarValue::parse(a.args[1]).num());
  } else if (a.verb == "quest") {
    if (!features_.quests) {
      if (host_.log) host_.log(1, "\"" + text + "\": ts.quests is not enabled");
      return;
    }
    const std::string& v = a.args[0];
    std::string why;
    bool ok = true;
    if (v == "start") ok = quests.start(a.args[1], &why);
    else if (v == "complete") ok = quests.complete(a.args[1]);
    else if (v == "fail") ok = quests.fail(a.args[1]);
    else if (v == "track") quests.track(a.args[1]);
    else if (v == "progress") {
      const int n = a.args.size() > 3 ? static_cast<int>(VarValue::parse(a.args[3]).num()) : 1;
      ok = quests.progress(a.args[1], a.args[2], n);
    }
    if (!ok && !why.empty() && host_.log) host_.log(0, why);
    quest_dirty_ = true;
  } else if (a.verb == "event") {
    const std::string arg = a.args.size() > 1 ? a.args[1] : std::string();
    last_event_ = a.args[0];
    last_event_arg_ = arg;
    pending_events_.push_back({a.args[0], VarValue::parse(arg).num()});
  } else if (a.verb == "save") {
    if (!features_.saves) return;
    if (a.args.empty()) {
      autosave_due_ = true;
    } else {
      std::string e;
      if (!save(a.args[0], "", &e) && host_.log) host_.log(1, e);
    }
  } else if (a.verb == "end") {
    dialogue.stop();
  }
  dlg_dirty_ = true;
}

// ---- Saves ---------------------------------------------------------------------

bool StoryRuntime::save(const std::string& slot, const std::string& label, std::string* error) {
  saves.playtime = playtime_;
  if (!saves.save(slot, label, error)) return false;
  pending_hooks_.push_back({"on_game_saved", slot});
  return true;
}

bool StoryRuntime::autosave(std::string* slot, std::string* error) {
  saves.playtime = playtime_;
  std::string s;
  if (!saves.autosave(&s, error)) return false;
  ++autosaves_;
  since_autosave_ = 0.0;
  if (slot) *slot = s;
  pending_hooks_.push_back({"on_game_saved", s});
  return true;
}

bool StoryRuntime::load(const std::string& slot, std::string* error) {
  dialogue.stop();
  dialogue.take_ended();
  if (!saves.load(slot, error)) return false;
  playtime_ = saves.playtime;
  quests.take_events();  // the loaded state is not news
  pending_events_.clear();
  dlg_dirty_ = quest_dirty_ = true;
  pending_hooks_.push_back({"on_game_loaded", slot});
  return true;
}

// ---- Tick ------------------------------------------------------------------------

void StoryRuntime::flush_events() {
  for (const QuestLog::Event& e : quests.take_events()) {
    quest_dirty_ = true;
    const QuestDef* d = quests.def(e.quest);
    const std::string title = d ? d->title : e.quest;
    switch (e.kind) {
      case QuestLog::Event::Kind::Started:
        say("New bounty: " + title, 3.0);
        hook("on_quest_started", e.quest, 0);
        break;
      case QuestLog::Event::Kind::Objective:
        hook("on_objective", e.quest + "/" + e.objective, e.value);
        break;
      case QuestLog::Event::Kind::Completed:
        say("Bounty done: " + title, 3.0);
        hook("on_quest_completed", e.quest, 0);
        if (features_.saves && settings.autosave_on_quest) autosave_due_ = true;
        break;
      case QuestLog::Event::Kind::Failed:
        say("Bounty lost: " + title, 3.0);
        hook("on_quest_failed", e.quest, 0);
        break;
    }
  }
  const auto evs = std::move(pending_events_);
  pending_events_.clear();
  for (const auto& [name, n] : evs) hook("on_dialogue_event", name, n);
  for (const std::string& id : dialogue.take_ended()) {
    dlg_dirty_ = true;
    hook("on_dialogue_end", id, 0);
  }
  const auto hs = std::move(pending_hooks_);
  pending_hooks_.clear();
  for (const auto& [name, slot] : hs) hook(name.c_str(), slot, 0);
}

void StoryRuntime::tick(double dt) {
  if (!running_) return;
  playtime_ += dt;
  since_autosave_ += dt;
  // Hooks may change things again; a few rounds settle chains.
  for (int round = 0; round < 4; ++round) {
    const bool more = !quests.pending_events().empty() || !pending_events_.empty() || !pending_hooks_.empty();
    if (!more && round > 0) break;
    flush_events();
  }
  if (features_.saves) {
    if (settings.autosave_seconds > 0 && since_autosave_ >= settings.autosave_seconds) autosave_due_ = true;
    if (autosave_due_ && !dialogue.active()) {
      autosave_due_ = false;
      std::string err, slot;
      if (!autosave(&slot, &err)) {
        if (host_.log) host_.log(1, "autosave: " + err);
      } else if (host_.log) {
        host_.log(0, "autosaved to " + slot);
      }
    }
  }
  if (!pending_hooks_.empty()) flush_events();  // on_game_saved this tick
  if (dlg_dirty_) refresh_dialogue();
  if (quest_dirty_) {
    refresh_tracker();
    if (journal_open_) refresh_journal();
  }
}

// ---- UI --------------------------------------------------------------------------

void StoryRuntime::refresh_dialogue() {
  dlg_dirty_ = false;
  if (!features_.dialogue) return;
  if (!dialogue.active()) {
    if (dlg_shown_) ui_show("dialogue", false);
    dlg_shown_ = false;
    return;
  }
  const DialogueRunner::Line& l = dialogue.line();
  ui_text("dialogue.dlg_speaker", l.speaker);
  ui_text("dialogue.dlg_text", wrap_text(l.text, 78));
  const int n = static_cast<int>(l.choices.size());
  for (int i = 0; i < kMaxChoices; ++i) {
    const std::string id = "dialogue.dlg_choice_" + std::to_string(i + 1);
    if (i < n) ui_text(id, std::to_string(i + 1) + ". " + l.choices[static_cast<std::size_t>(i)]);
    ui_visible(id, i < n);
  }
  ui_visible("dialogue.dlg_next", n == 0);
  if (!dlg_shown_) ui_show("dialogue", true);
  dlg_shown_ = true;
  runtime::UiCommand f;
  f.kind = runtime::UiCommand::Kind::Focus;
  f.target = n > 0 ? "dialogue.dlg_choice_1" : "dialogue.dlg_next";
  post(std::move(f));
}

void StoryRuntime::refresh_tracker() {
  quest_dirty_ = false;
  if (!features_.quests) return;
  const std::string q = quests.tracked();
  const QuestDef* d = q.empty() ? nullptr : quests.def(q);
  if (!settings.tracker || !d || quests.status(q) != QuestStatus::Active) {
    ui_show("quest_tracker", false);
    return;
  }
  std::string text;
  for (const ObjectiveDef& o : d->objectives) {
    const int p = quests.objective_progress(q, o.id);
    if (o.hidden && p == 0) continue;
    if (!text.empty()) text += "\n";
    text += (p >= o.count ? "[x] " : "[ ] ") + o.text;
    if (o.count > 1) text += " (" + std::to_string(p) + "/" + std::to_string(o.count) + ")";
    if (o.optional) text += " (optional)";
  }
  ui_text("quest_tracker.qt_title", d->title);
  ui_text("quest_tracker.qt_text", wrap_text(text, 40));
  ui_show("quest_tracker", true);
}

void StoryRuntime::show_journal(bool on) {
  if (!features_.quests) return;
  journal_open_ = on;
  if (on) {
    journal_sel_ = 0;
    const auto order = quests.journal_order();
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == quests.tracked()) journal_sel_ = static_cast<int>(i);
    }
    refresh_journal();
    runtime::UiCommand f;
    f.kind = runtime::UiCommand::Kind::Focus;
    f.target = "journal.jr_list";
    post(std::move(f));
  }
  ui_show("journal", on);
}

void StoryRuntime::refresh_journal() {
  journal_ids_ = quests.journal_order();
  std::string items;
  for (const std::string& id : journal_ids_) {
    const QuestDef* d = quests.def(id);
    const QuestStatus s = quests.status(id);
    const char* mark = s == QuestStatus::Active ? (id == quests.tracked() ? "> " : "  ")
                       : s == QuestStatus::Completed ? "+ " : "x ";
    if (!items.empty()) items += "\n";
    items += mark + (d ? d->title : id);
  }
  runtime::UiCommand c;
  c.kind = runtime::UiCommand::Kind::SetItems;
  c.target = "journal.jr_list";
  c.text = items;
  post(std::move(c));
  runtime::UiCommand sel;
  sel.kind = runtime::UiCommand::Kind::SetValue;
  sel.target = "journal.jr_list";
  sel.value = journal_sel_;
  post(std::move(sel));
  if (journal_ids_.empty()) {
    ui_text("journal.jr_title", "No bounties yet");
    ui_text("journal.jr_meta", "");
    ui_text("journal.jr_desc", "Talk to folk around town.");
    ui_text("journal.jr_objectives", "");
    ui_visible("journal.jr_track", false);
    return;
  }
  journal_sel_ = std::clamp(journal_sel_, 0, static_cast<int>(journal_ids_.size()) - 1);
  const std::string& id = journal_ids_[static_cast<std::size_t>(journal_sel_)];
  const QuestDef* d = quests.def(id);
  if (!d) return;
  const QuestStatus s = quests.status(id);
  std::string meta = quest_status_name(s);
  meta[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(meta[0])));
  if (!d->giver.empty()) meta += "  -  from " + d->giver;
  std::string objs;
  for (const ObjectiveDef& o : d->objectives) {
    const int p = quests.objective_progress(id, o.id);
    if (o.hidden && p == 0) continue;
    if (!objs.empty()) objs += "\n";
    objs += (p >= o.count ? "[x] " : "[ ] ") + o.text;
    if (o.count > 1) objs += " (" + std::to_string(p) + "/" + std::to_string(o.count) + ")";
    if (o.optional) objs += " (optional)";
  }
  ui_text("journal.jr_title", d->title);
  ui_text("journal.jr_meta", meta);
  ui_text("journal.jr_desc", wrap_text(d->description, 52));
  ui_text("journal.jr_objectives", wrap_text(objs, 52));
  ui_visible("journal.jr_track", s == QuestStatus::Active && quests.tracked() != id);
}

void StoryRuntime::show_save_menu(bool on) {
  if (!features_.saves) return;
  save_menu_open_ = on;
  if (on) {
    save_sel_ = 0;
    refresh_save_menu();
    ui_text("save_menu.sv_note", "Pick a slot. Autosaves rotate through auto-1..auto-" +
                                     std::to_string(settings.autosave_slots) + ".");
  }
  ui_show("save_menu", on);
}

void StoryRuntime::refresh_save_menu() {
  save_slots_.clear();
  std::string items = "+ New save";
  save_slots_.push_back("");
  for (const SlotInfo& s : saves.list()) {
    save_slots_.push_back(s.slot);
    std::string t = s.time.size() >= 16 ? s.time.substr(0, 10) + " " + s.time.substr(11, 5) : s.time;
    items += "\n" + s.slot + "  " + (s.label.empty() ? std::string() : s.label + "  ") + t + " UTC  " +
             clock_text(s.playtime);
  }
  runtime::UiCommand c;
  c.kind = runtime::UiCommand::Kind::SetItems;
  c.target = "save_menu.sv_list";
  c.text = items;
  post(std::move(c));
  save_sel_ = std::clamp(save_sel_, 0, static_cast<int>(save_slots_.size()) - 1);
  runtime::UiCommand sel;
  sel.kind = runtime::UiCommand::Kind::SetValue;
  sel.target = "save_menu.sv_list";
  sel.value = save_sel_;
  post(std::move(sel));
}

bool StoryRuntime::ui_event(const std::string& widget, bool change, double value, const std::string& /*text*/) {
  if (!running_) return false;
  if (features_.dialogue && widget.rfind("dlg_", 0) == 0) {
    if (change) return true;
    if (widget == "dlg_next") {
      dialogue.advance();
    } else if (widget.rfind("dlg_choice_", 0) == 0) {
      dialogue.choose(std::atoi(widget.c_str() + 11) - 1);
    } else {
      return false;
    }
    dlg_dirty_ = true;
    refresh_dialogue();
    return true;
  }
  if (features_.quests && widget.rfind("jr_", 0) == 0) {
    if (widget == "jr_list" && change) {
      journal_sel_ = static_cast<int>(value);
      refresh_journal();
    } else if (widget == "jr_close" && !change) {
      show_journal(false);
    } else if (widget == "jr_track" && !change) {
      if (journal_sel_ >= 0 && journal_sel_ < static_cast<int>(journal_ids_.size())) {
        quests.track(journal_ids_[static_cast<std::size_t>(journal_sel_)]);
      }
      refresh_journal();
      refresh_tracker();
    }
    return true;
  }
  if (features_.saves && widget.rfind("sv_", 0) == 0) {
    if (widget == "sv_list" && change) {
      save_sel_ = static_cast<int>(value);
      return true;
    }
    if (change) return true;
    const std::string slot =
        save_sel_ >= 0 && save_sel_ < static_cast<int>(save_slots_.size()) ? save_slots_[static_cast<std::size_t>(save_sel_)] : "";
    std::string err;
    if (widget == "sv_close") {
      show_save_menu(false);
    } else if (widget == "sv_save") {
      std::string target = slot;
      if (target.empty() || target.rfind("auto-", 0) == 0) {
        for (int i = 1; i < 1000; ++i) {
          if (!saves.exists(std::to_string(i))) {
            target = std::to_string(i);
            break;
          }
        }
      }
      if (save(target, host_.scene_name, &err)) {
        ui_text("save_menu.sv_note", "Saved to slot " + target + ".");
      } else {
        ui_text("save_menu.sv_note", err);
      }
      refresh_save_menu();
    } else if (widget == "sv_load") {
      if (slot.empty()) {
        ui_text("save_menu.sv_note", "Pick a save to load.");
      } else if (load(slot, &err)) {
        show_save_menu(false);
        say("Loaded " + slot, 2.0);
      } else {
        ui_text("save_menu.sv_note", err);
      }
    } else if (widget == "sv_delete") {
      if (!slot.empty() && saves.remove(slot)) ui_text("save_menu.sv_note", "Deleted " + slot + ".");
      refresh_save_menu();
    }
    return true;
  }
  return false;
}

}  // namespace story
}  // namespace tombstone
}  // namespace ts
