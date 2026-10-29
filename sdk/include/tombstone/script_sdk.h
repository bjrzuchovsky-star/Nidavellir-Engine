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

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <source_location>
#include <sstream>
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

// Same shape as the C# Vector3: public fields, Zero / Up, + and * float.
struct Vector3 {
  float X = 0, Y = 0, Z = 0;
  static Vector3 Zero() { return {}; }
  static Vector3 Up() { return {0, 1, 0}; }
  static Vector3 Forward() { return {0, 0, -1}; }
  static Vector3 Right() { return {1, 0, 0}; }
  Vector3 operator+(Vector3 o) const { return {X + o.X, Y + o.Y, Z + o.Z}; }
  Vector3 operator-(Vector3 o) const { return {X - o.X, Y - o.Y, Z - o.Z}; }
  Vector3 operator*(float s) const { return {X * s, Y * s, Z * s}; }
  friend Vector3 operator*(float s, Vector3 v) { return v * s; }
  std::string ToString() const {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "(" << X << ", " << Y << ", " << Z << ")";
    return o.str();
  }
};

struct Axes {
  Vector3 Right, Forward, Up;
};

struct RayHit {
  Entity entity = 0;
  Vector3 point;
  float distance = 0;
  explicit operator bool() const { return entity != 0; }
};

class Script;
class EntityRef {
 public:
  EntityRef() = default;
  EntityRef(Script* s, Entity id) : s_(s), id_(id) {}
  explicit operator bool() const { return id_ != 0; }
  operator Entity() const { return id_; }
  Entity id() const { return id_; }
  void SetLocalRotation(float x, float y, float z) const;
  Vector3 Position() const;

 private:
  Script* s_ = nullptr;
  Entity id_ = 0;
};

class Script {
 public:
  virtual ~Script() = default;

  // Hooks. OnStart / OnUpdate match the C# sample. on_start / on_tick stay
  // for older scripts: their defaults call the new names, and a script that
  // overrides the old name does not run the new one.
  virtual void Awake() {}
  virtual void OnEnable() {}
  virtual void OnDisable() {}
  virtual void OnStart() {}
  virtual void OnFixedUpdate(float /*deltaTime*/) {}
  virtual void OnUpdate(float /*deltaTime*/) {}
  virtual void OnLateUpdate(float /*deltaTime*/) {}
  virtual void OnCollisionEnter(Entity /*other*/) {}
  virtual void OnCollisionStay(Entity /*other*/) {}
  virtual void OnCollisionExit(Entity /*other*/) {}
  virtual void on_start() { OnStart(); }
  virtual void on_tick(double dt) { OnUpdate(static_cast<float>(dt)); }
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
  bool PlayAnimation(const std::string& clip) {
    return Query("anim", std::to_string(self_) + "\t" + clip) == "1";
  }
  bool SetFov(Entity camera, float degrees) {
    return Query("fov", std::to_string(camera) + "\t" + std::to_string(degrees)) == "1";
  }
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
  // Localisation: tr("menu.start"), tr("gold.count", {"12"}) fills {0}.
  std::string tr(const std::string& key, const std::vector<std::string>& args = {}) const {
    if (!api_->translate) return key;
    std::string enc;
    for (const std::string& a : args) enc += a + "\n";
    std::string buf(256, '\0');
    int n = api_->translate(api_->world, key.c_str(), enc.c_str(), buf.data(), 256);
    if (n > 256) {
      buf.assign(static_cast<std::size_t>(n), '\0');
      n = api_->translate(api_->world, key.c_str(), enc.c_str(), buf.data(), n);
    }
    buf.resize(static_cast<std::size_t>(n > 0 ? n : 0));
    return buf;
  }
  bool set_language(const std::string& lang) { return api_->lang_set && api_->lang_set(api_->world, lang.c_str()) != 0; }
  std::string language() const {
    char b[64];
    const int n = api_->lang_get ? api_->lang_get(api_->world, b, sizeof(b)) : 0;
    return std::string(b, static_cast<std::size_t>(n > 0 && n <= 64 ? n : 0));
  }
  bool held(Button b, int slot1 = 1) const { return (api_->buttons(api_->world, slot1 - 1) & b) != 0; }

  // PascalCase API, the same names as C# (C++ returns values from methods).
  float Prop(const std::string& name, float fallback) const { return static_cast<float>(prop(name, static_cast<double>(fallback))); }
  int Prop(const std::string& name, int fallback) const { return static_cast<int>(prop(name, static_cast<double>(fallback))); }
  double Prop(const std::string& name, double fallback) const { return prop(name, fallback); }
  bool Prop(const std::string& name, bool fallback) const { return prop(name, fallback); }
  std::string Prop(const std::string& name, const char* fallback) const { return prop(name, fallback); }
  std::string Prop(const std::string& name, const std::string& fallback) const {
    const auto it = props_.find(name);
    return it == props_.end() ? fallback : it->second.text;
  }
  void Toast(const std::string& title, const std::string& message) {
    toast(message.empty() ? title : title + " — " + message);
  }
  void Log(const std::string& text, const std::source_location& loc = std::source_location::current()) {
    if (api_ && api_->log_at) api_->log_at(api_->world, TS_LOG_INFO, text.c_str(), self_, loc.file_name(), static_cast<int>(loc.line()));
    else log(text);
  }
  Vector3 Position() const { return pose_at(self_, 0); }
  void SetPosition(Vector3 p) { set_position(self_, p); }
  Axes Transform() const {
    Axes a;
    a.Forward = pose_at(self_, 9);
    a.Right = pose_at(self_, 12);
    a.Up = pose_at(self_, 15);
    return a;
  }
  void Move(Vector3 delta) { move_by(self_, delta); }
  void Rotate(Vector3 eulerDegrees) { rotate_by(self_, eulerDegrees); }
  EntityRef Find(const std::string& name) { return EntityRef(this, find(name)); }
  EntityRef Player() { return EntityRef(this, player(1)); }
  std::string Query(const std::string& op, const std::string& args) const;
  bool HasComponent(const std::string& name) const { return Query("has", std::to_string(self_) + "\t" + name) == "1"; }
  bool AddComponent(const std::string& name) { return Query("add", std::to_string(self_) + "\t" + name) == "1"; }
  bool RemoveComponent(const std::string& name) { return Query("remove", std::to_string(self_) + "\t" + name) == "1"; }
  void SetTag(const std::string& tag) { Query("set_tag", std::to_string(self_) + "\t" + tag); }
  std::string Tag() const { return Query("tag", std::to_string(self_)); }
  bool Alive(Entity e) const { return Query("alive", std::to_string(e)) == "1"; }
  EntityRef FindWithTag(const std::string& tag) {
    const std::string id = Query("find_tag", tag);
    return EntityRef(this, static_cast<Entity>(std::strtoull(id.c_str(), nullptr, 10)));
  }
  EntityRef Instantiate(const std::string& what, Vector3 at) {
    const std::string args = what + "\t" + std::to_string(at.X) + "\t" + std::to_string(at.Y) + "\t" + std::to_string(at.Z);
    const std::string id = Query("instantiate", args);
    return EntityRef(this, static_cast<Entity>(std::strtoull(id.c_str(), nullptr, 10)));
  }
  void Destroy(Entity e, float delaySeconds) {
    Query("destroy_in", std::to_string(e) + "\t" + std::to_string(delaySeconds));
  }
  RayHit Raycast(Vector3 origin, Vector3 direction, float maxDistance = 100.0f) const {
    RayHit hit;
    const std::string args = std::to_string(origin.X) + " " + std::to_string(origin.Y) + " " + std::to_string(origin.Z) +
                             " " + std::to_string(direction.X) + " " + std::to_string(direction.Y) + " " +
                             std::to_string(direction.Z) + " " + std::to_string(maxDistance);
    const std::string res = Query("raycast", args);
    unsigned long long id = 0;
    float dist = 0, px = 0, py = 0, pz = 0;
    if (std::sscanf(res.c_str(), "%llu %f %f %f %f", &id, &dist, &px, &py, &pz) == 5 && id != 0) {
      hit.entity = static_cast<Entity>(id);
      hit.distance = dist;
      hit.point = {px, py, pz};
    }
    return hit;
  }
  struct Event {
    std::vector<std::function<void()>> listeners;
    void Add(std::function<void()> fn) { if (fn) listeners.push_back(std::move(fn)); }
    void Invoke() {
      const auto copy = listeners;
      for (const auto& fn : copy)
        if (fn) fn();
    }
  };
  int After(float seconds, std::function<void()> fn) {
    timers_.push_back({seconds, std::move(fn)});
    return static_cast<int>(timers_.size());
  }
  void pump_timers(float dt) {
    for (auto it = timers_.begin(); it != timers_.end();) {
      it->left -= dt;
      if (it->left <= 0.0f) {
        auto fn = std::move(it->fn);
        it = timers_.erase(it);
        if (fn) fn();
      } else {
        ++it;
      }
    }
  }
  Entity Overlap(Vector3 point, float radius) const {
    const std::string args = std::to_string(point.X) + " " + std::to_string(point.Y) + " " + std::to_string(point.Z) +
                             " " + std::to_string(radius);
    return static_cast<Entity>(std::strtoull(Query("overlap", args).c_str(), nullptr, 10));
  }
  template <class T>
  T* GetComponent();
  template <class T>
  T* AddComponent();
  template <class T>
  void RemoveComponent();
  // Props (scene overrides of the defaults you pass).
  double prop(const std::string& name, double fallback) const {
    const auto it = props_.find(name);
    return it == props_.end() ? fallback : it->second.number;
  }
  std::string prop(const std::string& name, const char* fallback) const {
    const auto it = props_.find(name);
    return it == props_.end() ? std::string(fallback) : it->second.text;
  }
  bool prop(const std::string& name, bool fallback) const {
    const auto it = props_.find(name);
    return it == props_.end() ? fallback : it->second.number != 0.0;
  }
  double prop(const std::string& name, int fallback) const {
    return prop(name, static_cast<double>(fallback));
  }
  // The editor's Inspector lists every prop("name", literal) call it finds
  // in the file, with the literal as the default.

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

  Vector3 pose_at(Entity e, int at) const {
    float o[18] = {};
    if (api_ && api_->pose && api_->pose(api_->world, e, o)) return {o[at], o[at + 1], o[at + 2]};
    if (at == 0 && api_ && api_->position) {
      float x = 0, y = 0;
      if (api_->position(api_->world, e, &x, &y)) return {x, y, 0};
    }
    if (at == 9) return {0, 0, -1};
    if (at == 12) return {1, 0, 0};
    if (at == 15) return {0, 1, 0};
    return {};
  }
  void move_by(Entity e, Vector3 d) {
    if (api_ && api_->move_by) api_->move_by(api_->world, e, d.X, d.Y, d.Z);
  }
  void rotate_by(Entity e, Vector3 d) {
    if (api_ && api_->rotate_by) api_->rotate_by(api_->world, e, d.X, d.Y, d.Z);
  }
  void set_position(Entity e, Vector3 p) {
    if (api_ && api_->set_position) api_->set_position(api_->world, e, p.X, p.Y, p.Z);
    else if (api_) api_->move(api_->world, e, p.X, p.Y);
  }
  void set_local_euler(Entity e, float x, float y, float z) {
    if (api_ && api_->set_local_euler) api_->set_local_euler(api_->world, e, x, y, z);
  }
  const TsScriptApi* api_ptr() const { return api_; }

 private:
  struct PendingTimer {
    float left = 0;
    std::function<void()> fn;
  };
  const TsScriptApi* api_ = nullptr;
  Entity self_ = 0;
  std::map<std::string, PropValue> props_;
  std::vector<PendingTimer> timers_;
};

inline Script*& script_active() {
  static thread_local Script* cur = nullptr;
  return cur;
}

struct ScriptLive {
  Entity id = 0;
  Script* script = nullptr;
  bool runtime = false;
  bool started = false;
};
struct ScriptLiveStore {
  std::vector<ScriptLive> items;
  std::uint64_t pumped = 0;
  std::uint64_t pumped_fixed = 0;
  std::uint64_t pumped_late = 0;
  ~ScriptLiveStore() {
    for (ScriptLive& L : items)
      if (L.runtime) delete L.script;
  }
};
inline ScriptLiveStore& script_lives() {
  static ScriptLiveStore store;
  return store;
}

inline std::string Script::Query(const std::string& op, const std::string& args) const {
  if (!api_ || !api_->query) return {};
  std::string buf(128, '\0');
  int n = api_->query(api_->world, op.c_str(), args.c_str(), buf.data(), static_cast<int>(buf.size()));
  if (n < 0) return {};
  if (n > static_cast<int>(buf.size())) {
    buf.assign(static_cast<std::size_t>(n), '\0');
    n = api_->query(api_->world, op.c_str(), args.c_str(), buf.data(), n);
  }
  if (n < 0) return {};
  buf.resize(static_cast<std::size_t>(n > 0 ? n : 0));
  return buf;
}

template <class T>
T* Script::GetComponent() {
  for (const ScriptLive& L : script_lives().items) {
    if (L.id == self_ && L.script) {
      if (T* t = dynamic_cast<T*>(L.script)) return t;
    }
  }
  return nullptr;
}

template <class T>
T* Script::AddComponent() {
  T* t = new T();
  t->ts_bind(api_, self_, "");
  t->Awake();
  t->OnEnable();
  script_lives().items.push_back({self_, t, true, false});
  return t;
}

template <class T>
void Script::RemoveComponent() {
  auto& items = script_lives().items;
  for (auto it = items.begin(); it != items.end(); ++it) {
    if (it->runtime && it->id == self_ && dynamic_cast<T*>(it->script)) {
      delete it->script;
      items.erase(it);
      return;
    }
  }
}

inline void EntityRef::SetLocalRotation(float x, float y, float z) const {
  if (s_) s_->set_local_euler(id_, x, y, z);
}
inline Vector3 EntityRef::Position() const { return s_ ? s_->pose_at(id_, 0) : Vector3{}; }

struct Input {
  static float GetAxis(const char* name) {
    Script* s = script_active();
    if (!s || !name) return 0.0f;
    const TsScriptApi* api = s->api_ptr();
    if (!api || !api->axis) return 0.0f;
    return api->axis(api->world, 0, name);
  }
  static bool GetButtonDown(const char* name) {
    Script* s = script_active();
    if (!s || !name) return false;
    const TsScriptApi* api = s->api_ptr();
    if (!api || !api->button_down) return false;
    return api->button_down(api->world, 0, name) != 0;
  }
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
  ::ts::script_lives().items.push_back({self, s, false});
  s->Awake();
  s->OnEnable();
  return s;
}
TS_SCRIPT_EXPORT void ts_script_destroy(void* sp) {
  auto* s = static_cast<::ts::Script*>(sp);
  auto& items = ::ts::script_lives().items;
  for (auto it = items.begin(); it != items.end();) {
    if (it->script == s) it = items.erase(it);
    else ++it;
  }
  delete s;
}
TS_SCRIPT_EXPORT int ts_script_has(void* /*s*/, int hook) {
  return hook >= 0 && hook < TS_HOOK_COUNT;  // every hook has a (default) body
}
TS_SCRIPT_EXPORT void ts_script_call(void* sp, int hook, uint64_t e, double n, const char* text) {
  auto* s = static_cast<::ts::Script*>(sp);
  struct Guard {
    explicit Guard(::ts::Script* p) { ::ts::script_active() = p; }
    ~Guard() { ::ts::script_active() = nullptr; }
  } guard(s);
  const std::string t = text ? text : "";
  const std::size_t tab = t.find('\t');
  try {
  switch (hook) {
    case TS_HOOK_START: s->on_start(); break;
    case TS_HOOK_FIXED: {
      s->OnFixedUpdate(static_cast<float>(n));
      ::ts::ScriptLiveStore& store = ::ts::script_lives();
      const std::uint64_t now = s->tick();
      if (store.pumped_fixed != now) {
        store.pumped_fixed = now;
        for (::ts::ScriptLive& L : store.items)
          if (L.runtime && L.script && L.script != s) L.script->OnFixedUpdate(static_cast<float>(n));
      }
      break;
    }
    case TS_HOOK_LATE: {
      s->OnLateUpdate(static_cast<float>(n));
      ::ts::ScriptLiveStore& store = ::ts::script_lives();
      const std::uint64_t now = s->tick();
      if (store.pumped_late != now) {
        store.pumped_late = now;
        for (::ts::ScriptLive& L : store.items)
          if (L.runtime && L.script && L.script != s) L.script->OnLateUpdate(static_cast<float>(n));
      }
      break;
    }
    case TS_HOOK_TICK: {
      s->on_tick(n);
      s->pump_timers(static_cast<float>(n));
      ::ts::ScriptLiveStore& store = ::ts::script_lives();
      const std::uint64_t now = s->tick();
      if (store.pumped != now) {
        store.pumped = now;
        std::vector<::ts::Script*> extra;
        for (::ts::ScriptLive& L : store.items) {
          if (!L.runtime || !L.script || L.script == s) continue;
          if (!L.started) {
            L.started = true;
            L.script->on_start();
          }
          extra.push_back(L.script);
        }
        for (::ts::Script* r : extra) r->on_tick(n);
      }
      break;
    }
    case TS_HOOK_COLLISION:
      if (t == "enter") s->OnCollisionEnter(e);
      else if (t == "stay") s->OnCollisionStay(e);
      else s->OnCollisionExit(e);
      break;
    case TS_HOOK_TRIGGER_ENTER: s->on_trigger_enter(e); break;
    case TS_HOOK_TRIGGER_EXIT: s->on_trigger_exit(e); break;
    case TS_HOOK_INTERACT: s->on_interact(e); break;
    case TS_HOOK_RELOAD: s->on_reload(); break;
    case TS_HOOK_DESTROY: s->OnDisable(); s->on_destroy(); break;
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
