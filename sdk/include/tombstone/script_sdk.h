// TombStone C++ script SDK (header-only). Write gameplay scripts in
// <project>/scripts/*.cpp:
//
//   #include <tombstone/script_sdk.h>
//   struct Door : ts::Script {
//     int opened = 0;
//     void on_start() override { log("door ready, reach " + std::to_string(prop("reach", 24.0))); }
//     void on_interact(ts::Entity rider) override {
//       ++opened;
//       toast("The door creaks (" + std::to_string(opened) + ")");
//       set("open", true);
//     }
//     // Hot reload keeps what you save here (world state via set() is kept anyway).
//     std::string save() const override { return std::to_string(opened); }
//     void load(const std::string& s) override { opened = std::atoi(s.c_str()); }
//   };
//   TS_SCRIPT(Door, "door")   // file stem: scripts/door.cpp
//
// The editor generates a CMake project per claim (<project>/.tombstone/
// native), builds every scripts/*.cpp into one module (ts_scripts.dll /
// .so) and hot-reloads it when a file is saved: save() -> new module ->
// load() -> on_reload(). Build errors go to the Telegraph.

#pragma once

#include "tombstone/script_abi.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#define TS_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#else
#define TS_SCRIPT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace ts {

using Entity = std::uint64_t;

enum Button : std::uint32_t {
  kAction = 1u << 0, kAlt = 1u << 1, kStart = 1u << 2, kUse = 1u << 3,
  kPlace = 1u << 4, kFire = 1u << 5, kNext = 1u << 6, kPrev = 1u << 7,
};

struct PropValue {
  char type = 'n';  // n / b / t
  double number = 0.0;
  std::string text;
};

inline PropValue arg(double v) { PropValue p; p.type = 'n'; p.number = v; return p; }
inline PropValue arg(bool v) { PropValue p; p.type = 'b'; p.number = v ? 1 : 0; return p; }
inline PropValue arg(const std::string& v) { PropValue p; p.type = 't'; p.text = v; return p; }
inline PropValue arg(const char* v) { return arg(std::string(v)); }

class Script {
 public:
  virtual ~Script() = default;

  // Hooks (override what you need).
  virtual void on_start() {}
  virtual void on_tick(double /*dt*/) {}
  virtual void on_trigger_enter(Entity /*other*/) {}
  virtual void on_trigger_exit(Entity /*other*/) {}
  virtual void on_interact(Entity /*rider*/) {}
  virtual void on_reload() {}
  virtual void on_destroy() {}
  virtual void on_hand(const std::string& /*hook*/, const std::string& /*hand*/, double /*n*/) {}
  // Plugin events (ts.quests / ts.dialogue / ts.saves, ...). The default
  // on_event routes the story hooks to the virtuals below; override on_event
  // to see every event, including ones from other plugins.
  virtual void on_event(const std::string& hook, const std::string& text, double n) {
    if (hook == "on_quest_started") on_quest_started(text);
    else if (hook == "on_quest_completed") on_quest_completed(text);
    else if (hook == "on_quest_failed") on_quest_failed(text);
    else if (hook == "on_objective") on_objective(text, n);
    else if (hook == "on_dialogue_event") on_dialogue_event(text, n);
    else if (hook == "on_dialogue_end") on_dialogue_end(text);
    else if (hook == "on_game_saved") on_game_saved(text);
    else if (hook == "on_game_loaded") on_game_loaded(text);
  }
  virtual void on_quest_started(const std::string& /*id*/) {}
  virtual void on_quest_completed(const std::string& /*id*/) {}
  virtual void on_quest_failed(const std::string& /*id*/) {}
  virtual void on_objective(const std::string& /*quest_slash_objective*/, double /*count*/) {}
  virtual void on_dialogue_event(const std::string& /*name*/, double /*n*/) {}
  virtual void on_dialogue_end(const std::string& /*dialogue_id*/) {}
  virtual void on_game_saved(const std::string& /*slot*/) {}
  virtual void on_game_loaded(const std::string& /*slot*/) {}
  virtual void on_ui(const std::string& /*widget*/, bool /*change*/, double /*value*/,
                     const std::string& /*text*/) {}
  // Hot reload state.
  virtual std::string save() const { return {}; }
  virtual void load(const std::string& /*state*/) {}

  // The world.
  Entity self() const { return self_; }
  std::uint64_t tick() const { return api_->tick(api_->world); }
  Entity find(const std::string& name) const { return api_->find(api_->world, name.c_str()); }
  Entity player(int slot1) const { return api_->player(api_->world, slot1 - 1); }
  bool position(Entity e, float* x, float* y) const { return api_->position(api_->world, e, x, y) != 0; }
  void move(Entity e, float x, float y) { api_->move(api_->world, e, x, y); }
  void set_visible(Entity e, bool on) { api_->set_visible(api_->world, e, on ? 1 : 0); }
  void set_collider(Entity e, bool on) { api_->set_collider(api_->world, e, on ? 1 : 0); }
  bool play(Entity e, const std::string& clip, bool hold = false) {
    return api_->play(api_->world, e, clip.c_str(), hold ? 1 : 0) != 0;
  }
  Entity spawn(const std::string& tmpl, float x, float y) {
    return api_->spawn(api_->world, tmpl.c_str(), x, y);
  }
  void destroy(Entity e) { api_->destroy(api_->world, e); }
  // Entity state (shared with Lua / graphs / C#, replicated in multiplayer).
  double get(const std::string& key, double fallback = 0.0, Entity e = 0) const {
    double v = fallback;
    return api_->get_number(api_->world, e ? e : self_, key.c_str(), &v) ? v : fallback;
  }
  std::string get_text(const std::string& key, Entity e = 0) const {
    char buf[1024];
    const int n = api_->get_text(api_->world, e ? e : self_, key.c_str(), buf, sizeof(buf));
    return n < 0 ? std::string() : std::string(buf, static_cast<std::size_t>(n < 1024 ? n : 1023));
  }
  void set(const std::string& key, double v, Entity e = 0) {
    api_->set_number(api_->world, e ? e : self_, key.c_str(), v);
  }
  void set(const std::string& key, bool v, Entity e = 0) { set(key, v ? 1.0 : 0.0, e); }
  void set(const std::string& key, const std::string& v, Entity e = 0) {
    api_->set_text(api_->world, e ? e : self_, key.c_str(), v.c_str());
  }
  void toast(const std::string& text, int slot1 = 0, double seconds = 2.0) {
    api_->toast(api_->world, slot1 <= 0 ? -1 : slot1 - 1, text.c_str(), seconds, self_);
  }
  void log(const std::string& text) { api_->log(api_->world, TS_LOG_INFO, text.c_str(), self_); }
  void warn(const std::string& text) { api_->log(api_->world, TS_LOG_WARN, text.c_str(), self_); }
  void sound(const std::string& clip, float volume = 1.0f) {
    api_->sound(api_->world, clip.c_str(), volume, self_);
  }
  void music(const std::string& clip, float fade = 1.0f) { api_->music(api_->world, clip.c_str(), fade); }
  void burst(const std::string& preset, Entity at = 0) {
    api_->fx_burst(api_->world, preset.c_str(), at ? at : self_);
  }
  void ui_show(const std::string& doc) { api_->ui(api_->world, TS_UI_SHOW, doc.c_str(), "", 0); }
  void ui_hide(const std::string& doc) { api_->ui(api_->world, TS_UI_HIDE, doc.c_str(), "", 0); }
  void ui_text(const std::string& id, const std::string& t) {
    api_->ui(api_->world, TS_UI_SET_TEXT, id.c_str(), t.c_str(), 0);
  }
  // Plugin call bridge: table.fn(args...) on a plugin's Lua table, e.g.
  //   ts::PropValue n = plugin("tumbleweed", "count", {ts::arg(3.0), ts::arg("west")});
  // .type 'x' when the call failed (the error is in the Telegraph).
  PropValue plugin(const std::string& table, const std::string& fn,
                   const std::vector<PropValue>& args = {}) {
    std::string enc;
    for (const PropValue& a : args) {
      enc += a.type;
      enc += '\t';
      enc += a.type == 't' ? a.text : a.type == 'b' ? (a.number != 0 ? "1" : "0") : std::to_string(a.number);
      enc += '\n';
    }
    PropValue r;
    r.type = 'x';
    if (!api_->plugin_call) return r;
    std::string buf(256, '\0');
    int n = api_->plugin_call(api_->world, table.c_str(), fn.c_str(), enc.c_str(), buf.data(), 256);
    if (n > 256) {
      buf.assign(static_cast<std::size_t>(n), '\0');
      n = api_->plugin_call(api_->world, table.c_str(), fn.c_str(), enc.c_str(), buf.data(), n);
    }
    if (n < 2) return r;
    buf.resize(static_cast<std::size_t>(n));
    r.type = buf[0];
    r.text = buf.substr(2);
    r.number = r.type == 'b' ? (r.text == "1" ? 1.0 : 0.0) : r.type == 'n' ? std::atof(r.text.c_str()) : 0.0;
    return r;
  }
  bool held(Button b, int slot1 = 1) const { return (api_->buttons(api_->world, slot1 - 1) & b) != 0; }
  // Props (scene overrides of the defaults you pass).
  double prop(const std::string& name, double fallback) const {
    const auto it = props_.find(name);
    return it == props_.end() ? fallback : it->second.number;
  }
  std::string prop(const std::string& name, const char* fallback) const {
    const auto it = props_.find(name);
    return it == props_.end() ? std::string(fallback) : it->second.text;
  }

  // Engine side (set by the module before on_start).
  void ts_error(const std::string& what) {
    if (api_) api_->log(api_->world, TS_LOG_ERROR, what.c_str(), self_);
  }
  void ts_bind(const TsScriptApi* api, Entity self, const char* props) {
    api_ = api;
    self_ = self;
    const char* p = props ? props : "";
    while (*p) {
      const char* end = std::strchr(p, '\n');
      const std::string line(p, end ? static_cast<std::size_t>(end - p) : std::strlen(p));
      const std::size_t a = line.find('\t'), b = line.find('\t', a + 1);
      if (a != std::string::npos && b != std::string::npos) {
        PropValue v;
        v.type = line[a + 1];
        v.text = line.substr(b + 1);
        v.number = v.type == 'b' ? (v.text == "true" ? 1.0 : 0.0) : std::atof(v.text.c_str());
        props_[line.substr(0, a)] = v;
      }
      if (!end) break;
      p = end + 1;
    }
  }

 private:
  const TsScriptApi* api_ = nullptr;
  Entity self_ = 0;
  std::map<std::string, PropValue> props_;
};

using ScriptFactory = Script* (*)();
inline std::map<std::string, ScriptFactory>& script_registry() {
  static std::map<std::string, ScriptFactory> r;
  return r;
}
struct ScriptRegistrar {
  ScriptRegistrar(const char* stem, ScriptFactory f) { script_registry()[stem] = f; }
};

}  // namespace ts

#define TS_SCRIPT_CAT2(a, b) a##b
#define TS_SCRIPT_CAT(a, b) TS_SCRIPT_CAT2(a, b)
#define TS_SCRIPT(Class, stem)                                                   \
  static ::ts::ScriptRegistrar TS_SCRIPT_CAT(ts_script_reg_, __LINE__)(          \
      stem, []() -> ::ts::Script* { return new Class(); });

// Exactly one translation unit of the module (the generated module_main.cpp)
// defines TS_SCRIPT_MODULE_MAIN before including this header.
#if defined(TS_SCRIPT_MODULE_MAIN)
TS_SCRIPT_EXPORT int ts_script_abi(void) { return TS_SCRIPT_ABI; }
TS_SCRIPT_EXPORT const char* ts_script_classes(void) {
  static std::string list;
  list.clear();
  for (const auto& [k, v] : ::ts::script_registry()) list += k + "\n";
  return list.c_str();
}
TS_SCRIPT_EXPORT void* ts_script_create(const char* cls, const TsScriptApi* api, uint64_t self,
                                        const char* props) {
  const auto it = ::ts::script_registry().find(cls ? cls : "");
  if (it == ::ts::script_registry().end() || !api || api->abi != TS_SCRIPT_ABI) return nullptr;
  ::ts::Script* s = it->second();
  s->ts_bind(api, self, props);
  return s;
}
TS_SCRIPT_EXPORT void ts_script_destroy(void* s) { delete static_cast<::ts::Script*>(s); }
TS_SCRIPT_EXPORT int ts_script_has(void* /*s*/, int hook) {
  return hook >= 0 && hook < TS_HOOK_COUNT;  // every hook has a (default) body
}
TS_SCRIPT_EXPORT void ts_script_call(void* sp, int hook, uint64_t e, double n, const char* text) {
  auto* s = static_cast<::ts::Script*>(sp);
  const std::string t = text ? text : "";
  const std::size_t tab = t.find('\t');
  try {
  switch (hook) {
    case TS_HOOK_START: s->on_start(); break;
    case TS_HOOK_TICK: s->on_tick(n); break;
    case TS_HOOK_TRIGGER_ENTER: s->on_trigger_enter(e); break;
    case TS_HOOK_TRIGGER_EXIT: s->on_trigger_exit(e); break;
    case TS_HOOK_INTERACT: s->on_interact(e); break;
    case TS_HOOK_RELOAD: s->on_reload(); break;
    case TS_HOOK_DESTROY: s->on_destroy(); break;
    case TS_HOOK_HAND: s->on_hand(t.substr(0, tab), tab == std::string::npos ? "" : t.substr(tab + 1), n); break;
    case TS_HOOK_UI: s->on_ui(t.substr(0, tab), e != 0, n, tab == std::string::npos ? "" : t.substr(tab + 1)); break;
    case TS_HOOK_EVENT: s->on_event(t.substr(0, tab), tab == std::string::npos ? "" : t.substr(tab + 1), n); break;
    default: break;
  }
  } catch (const std::exception& ex) {
    s->ts_error(std::string("C++ script threw: ") + ex.what());
  } catch (...) {
    s->ts_error("C++ script threw an unknown exception");
  }
}
TS_SCRIPT_EXPORT int ts_script_save(void* s, char* buf, int cap) {
  const std::string st = static_cast<::ts::Script*>(s)->save();
  if (buf && cap >= static_cast<int>(st.size())) std::memcpy(buf, st.data(), st.size());
  return static_cast<int>(st.size());
}
TS_SCRIPT_EXPORT void ts_script_load(void* s, const char* data, int len) {
  static_cast<::ts::Script*>(s)->load(std::string(data ? data : "", data ? static_cast<std::size_t>(len) : 0));
}
#endif
