#include "runtime/ScriptAbi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ts {
namespace tombstone {
namespace runtime {

namespace fs = std::filesystem;

namespace {
ScriptWorld* W(void* w) { return static_cast<ScriptWorld*>(w); }

uint64_t a_tick(void* w) { return W(w)->script_tick(); }
uint64_t a_find(void* w, const char* n) { return W(w)->script_find(n ? n : ""); }
uint64_t a_player(void* w, int slot) { return W(w)->script_player(slot); }
int a_position(void* w, uint64_t id, float* x, float* y) {
  const Entity2D* e = W(w)->script_entity(id);
  if (!e) return 0;
  if (x) *x = e->x;
  if (y) *y = e->y;
  return 1;
}
void a_move(void* w, uint64_t id, float x, float y) { W(w)->script_move(id, x, y); }
void a_visible(void* w, uint64_t id, int on) { W(w)->script_set_visible(id, on != 0); }
void a_collider(void* w, uint64_t id, int on) { W(w)->script_set_collider(id, on != 0); }
int a_play(void* w, uint64_t id, const char* clip, int hold) {
  return W(w)->script_play(id, clip ? clip : "", hold != 0) ? 1 : 0;
}
uint64_t a_spawn(void* w, const char* t, float x, float y) { return W(w)->script_spawn(t ? t : "", x, y); }
void a_destroy(void* w, uint64_t id) { W(w)->script_destroy(id); }
int a_get_number(void* w, uint64_t id, const char* key, double* out) {
  const ScriptValue* v = W(w)->script_get(id, key ? key : "");
  if (!v) return 0;
  if (out) {
    *out = v->type == ScriptValue::Type::Number ? v->number
           : v->type == ScriptValue::Type::Bool ? (v->flag ? 1.0 : 0.0)
                                                : std::atof(v->text.c_str());
  }
  return 1;
}
void a_set_number(void* w, uint64_t id, const char* key, double v) {
  const ScriptValue sv = ScriptValue::of_number(v);
  W(w)->script_set(id, key ? key : "", &sv);
}
int a_get_text(void* w, uint64_t id, const char* key, char* buf, int cap) {
  const ScriptValue* v = W(w)->script_get(id, key ? key : "");
  if (!v) return -1;
  const std::string t = v->type == ScriptValue::Type::Text ? v->text : describe(*v);
  if (buf && cap > 0) {
    const std::size_t n = std::min<std::size_t>(t.size(), static_cast<std::size_t>(cap - 1));
    std::memcpy(buf, t.data(), n);
    buf[n] = '\0';
  }
  return static_cast<int>(t.size());
}
void a_set_text(void* w, uint64_t id, const char* key, const char* v) {
  const ScriptValue sv = ScriptValue::of_text(v ? v : "");
  W(w)->script_set(id, key ? key : "", &sv);
}
void a_toast(void* w, int slot, const char* t, double s, uint64_t from) {
  W(w)->script_toast(slot, t ? t : "", s, from);
}
void a_log(void* w, int level, const char* t, uint64_t entity) {
  LogEntry e;
  e.level = level == TS_LOG_ERROR ? LogLevel::Error : level == TS_LOG_WARN ? LogLevel::Warn : LogLevel::Info;
  e.channel = "script";
  e.text = t ? t : "";
  e.entity = entity;
  e.tick = W(w)->script_tick();
  W(w)->script_log(std::move(e));
}
void a_sound(void* w, const char* clip, float volume, uint64_t entity) {
  SoundEvent ev;
  ev.clip = clip ? clip : "";
  ev.volume = volume;
  ev.entity = entity;
  W(w)->script_sound(std::move(ev));
}
void a_music(void* w, const char* clip, float fade) {
  SoundEvent ev;
  ev.kind = SoundEvent::Kind::Music;
  ev.bus = "music";
  ev.clip = clip ? clip : "";
  ev.fade = fade;
  W(w)->script_sound(std::move(ev));
}
void a_fx(void* w, const char* preset, uint64_t entity) {
  FxEvent ev;
  ev.preset = preset ? preset : "";
  ev.entity = entity;
  ev.at_entity = true;
  if (const Entity2D* e = W(w)->script_entity(entity)) {
    ev.x = e->x;
    ev.y = e->y;
  }
  W(w)->script_fx(std::move(ev));
}
void a_ui(void* w, int kind, const char* target, const char* text, double value) {
  UiCommand c;
  c.kind = kind == TS_UI_HIDE ? UiCommand::Kind::Hide
           : kind == TS_UI_SET_TEXT ? UiCommand::Kind::SetText
           : kind == TS_UI_SET_VALUE ? UiCommand::Kind::SetValue : UiCommand::Kind::Show;
  c.target = target ? target : "";
  c.text = text ? text : "";
  c.value = value;
  W(w)->script_ui(std::move(c));
}
uint32_t a_buttons(void* w, int slot) { return W(w)->script_buttons(slot); }
int a_plugin_call(void* w, const char* table, const char* fn, const char* args, char* out, int cap) {
  std::vector<ScriptValue> vals;
  const char* p = args ? args : "";
  while (*p) {
    const char* end = std::strchr(p, '\n');
    const std::string line(p, end ? static_cast<std::size_t>(end - p) : std::strlen(p));
    p = end ? end + 1 : p + line.size();
    if (line.size() < 2 || line[1] != '\t') continue;
    const std::string v = line.substr(2);
    vals.push_back(line[0] == 'b' ? ScriptValue::of_bool(v == "1" || v == "true")
                   : line[0] == 't' ? ScriptValue::of_text(v)
                                    : ScriptValue::of_number(std::atof(v.c_str())));
  }
  ScriptValue r;
  std::string err;
  if (!W(w)->script_plugin_call(table ? table : "", fn ? fn : "", vals, &r, &err)) {
    LogEntry e;
    e.level = LogLevel::Error;
    e.channel = "plugin";
    e.text = err;
    e.tick = W(w)->script_tick();
    W(w)->script_log(std::move(e));
    return -1;
  }
  char num[64];
  std::string s;
  if (r.type == ScriptValue::Type::Bool) s = std::string("b\t") + (r.flag ? "1" : "0");
  else if (r.type == ScriptValue::Type::Text) s = "t\t" + r.text;
  else {
    std::snprintf(num, sizeof(num), "%.17g", r.number);
    s = std::string("n\t") + num;
  }
  const int need = static_cast<int>(s.size());
  if (out && cap >= need) std::memcpy(out, s.data(), s.size());
  return need;
}
}  // namespace

TsScriptApi make_script_api(ScriptWorld* world) {
  TsScriptApi a{};
  a.abi = TS_SCRIPT_ABI;
  a.world = world;
  a.tick = a_tick;
  a.find = a_find;
  a.player = a_player;
  a.position = a_position;
  a.move = a_move;
  a.set_visible = a_visible;
  a.set_collider = a_collider;
  a.play = a_play;
  a.spawn = a_spawn;
  a.destroy = a_destroy;
  a.get_number = a_get_number;
  a.set_number = a_set_number;
  a.get_text = a_get_text;
  a.set_text = a_set_text;
  a.toast = a_toast;
  a.log = a_log;
  a.sound = a_sound;
  a.music = a_music;
  a.fx_burst = a_fx;
  a.ui = a_ui;
  a.buttons = a_buttons;
  a.plugin_call = a_plugin_call;
  return a;
}

std::string encode_script_props(const std::vector<ScriptProp>& props) {
  std::string out;
  for (const ScriptProp& p : props) {
    const char t = p.value.type == ScriptValue::Type::Bool ? 'b'
                   : p.value.type == ScriptValue::Type::Text ? 't' : 'n';
    out += p.name + '\t' + t + '\t' + (t == 't' ? p.value.text : describe(p.value)) + '\n';
  }
  return out;
}

SharedLib::~SharedLib() { close(); }

const char* SharedLib::extension() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

bool SharedLib::open(const std::string& path, std::string* error) {
  close();
#if defined(_WIN32)
  h_ = LoadLibraryW(fs::path(path).wstring().c_str());
  if (!h_ && error) *error = "LoadLibrary failed (" + std::to_string(GetLastError()) + ") for " + path;
#else
  h_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h_ && error) {
    const char* e = dlerror();
    *error = e ? e : ("dlopen failed for " + path);
  }
#endif
  return h_ != nullptr;
}

void SharedLib::close() {
  if (!h_) return;
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(h_));
#else
  dlclose(h_);
#endif
  h_ = nullptr;
}

void* SharedLib::symbol(const char* name) const {
  if (!h_) return nullptr;
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h_), name));
#else
  return dlsym(h_, name);
#endif
}

int run_command(const std::string& command, std::string* output) {
  std::error_code ec;
  static int counter = 0;
  const fs::path log = fs::temp_directory_path(ec) /
                       ("ts_cmd_" + std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) + "_" +
                        std::to_string(++counter) + ".log");
#if defined(_WIN32)
  const std::string full = "\"" + command + " > \"" + log.string() + "\" 2>&1\"";
#else
  const std::string full = command + " > \"" + log.string() + "\" 2>&1";
#endif
  int rc = std::system(full.c_str());
#if !defined(_WIN32)
  if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
  if (output) {
    std::ifstream in(log, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    *output = ss.str();
  }
  fs::remove(log, ec);
  return rc;
}

std::vector<LogEntry> parse_build_output(const std::string& output, const std::string& project_dir) {
  std::vector<LogEntry> out;
  static const std::regex gcc(R"(^(.*?):(\d+):(?:\d+:)?\s*(fatal error|error|warning):\s*(.*)$)");
  static const std::regex msvc(R"(^\s*(.*?)\((\d+)(?:,\d+)?\)\s*:\s*(fatal error|error|warning)\s+\w+\s*:\s*(.*?)(?:\s*\[.*\])?$)");
  std::istringstream in(output);
  std::string line;
  const std::string root = fs::path(project_dir).lexically_normal().generic_string();
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (!std::regex_match(line, m, gcc) && !std::regex_match(line, m, msvc)) continue;
    LogEntry e;
    e.channel = "script";
    e.level = m[3].str() == "warning" ? LogLevel::Warn : LogLevel::Error;
    std::string file = fs::path(m[1].str()).lexically_normal().generic_string();
    if (!root.empty() && file.rfind(root + "/", 0) == 0) file = file.substr(root.size() + 1);
    e.file = file;
    e.line = std::atoi(m[2].str().c_str());
    e.text = m[4].str();
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
