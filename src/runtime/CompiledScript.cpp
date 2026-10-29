#include "runtime/CompiledScript.h"

#include "runtime/ScriptAbi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>

namespace ts {
namespace tombstone {
namespace runtime {

namespace fs = std::filesystem;

namespace {
std::atomic<bool> g_blocking{false};
}
void set_compiled_scripts_blocking(bool on) { g_blocking = on; }
bool compiled_scripts_blocking() { return g_blocking; }

CompiledModule::~CompiledModule() = default;

namespace {

struct Inst {
  std::uint64_t entity = 0;
  int slot = 0;  // place in the entity's script stack
  std::string path, stem, name;
  std::vector<ScriptProp> props;
  void* obj = nullptr;
  bool started = false, enabled = true;
};

std::string stem_of(const std::string& rel) {
  return fs::path(rel).stem().string();
}

}  // namespace

struct CompiledScriptHost::Impl {
  CompiledBackend be;
  ScriptWorld* world = nullptr;
  std::string project_dir;
  TsScriptApi api{};
  std::unique_ptr<CompiledModule> mod;
  std::vector<std::unique_ptr<Inst>> insts;
  bool tried = false;
  bool prebuilt = false;
  std::int64_t stamp = 0;
  int generation = 0;
  // Background build (off the game tick); swapped in by poll().
  std::future<ScriptBuildResult> pending;
  std::int64_t pending_stamp = 0;

  bool building() const { return pending.valid(); }
  void start_build(bool force) {
    if (building()) return;
    pending_stamp = be.stamp(project_dir);
    const std::string dir = project_dir;
    pending = std::async(std::launch::async, [dir, force, build = be.build] { return build(dir, force); });
  }

  void log(LogLevel lv, const std::string& text, const std::string& file = {}, int line = 0,
           std::uint64_t entity = 0) {
    LogEntry e;
    e.level = lv;
    e.channel = "script";
    e.text = text;
    e.file = file;
    e.line = line;
    e.entity = entity;
    e.tick = world->script_tick();
    world->script_log(std::move(e));
  }

  std::unique_ptr<CompiledModule> open_module(const std::string& file, std::string* err) {
    return be.open(file, prebuilt, ++generation, err);
  }

  void report(const ScriptBuildResult& b) {
    for (const LogEntry& p : b.problems) log(p.level, p.text, p.file, p.line);
    if (!b.ok) {
      std::string tail = b.output.size() > 1500 ? b.output.substr(b.output.size() - 1500) : b.output;
      log(LogLevel::Error, b.error + (b.problems.empty() && !tail.empty() ? ":\n" + tail : ""));
    }
  }

  bool ensure_module() {
    if (mod || tried) return mod != nullptr;
    tried = true;
    std::error_code ec;
    std::string err;
    std::string pre = be.prebuilt(project_dir);
    if (!pre.empty() && !fs::exists(pre, ec)) pre.clear();
    std::string why;
    if (be.usable && !be.usable(!pre.empty(), &why)) {
      log(LogLevel::Warn, why);
      return false;
    }
    if (!pre.empty()) {
      prebuilt = true;
      mod = open_module(pre, &err);
      if (!mod) log(LogLevel::Error, be.label + " scripts: " + err);
      return mod != nullptr;
    }
    if (!compiled_scripts_blocking()) {
      log(LogLevel::Info, be.label + " scripts: building the project module in the background");
      start_build(false);
      return false;
    }
    log(LogLevel::Info, be.label + " scripts: building the project module");
    const ScriptBuildResult b = be.build(project_dir, false);
    stamp = be.stamp(project_dir);
    return install(b) >= 0 && mod != nullptr;
  }

  // A finished build: report it, then load the module (first time: create
  // every waiting instance; later: hot swap). -1 failed, 0 nothing, 1 swapped.
  int install(const ScriptBuildResult& b) {
    report(b);
    if (!b.ok) {
      if (mod) log(LogLevel::Error, be.label + " reload skipped, the previous module keeps riding");
      return -1;
    }
    std::string err;
    auto next = open_module(b.module, &err);
    if (!next) {
      log(LogLevel::Error, (mod ? be.label + " reload: " : be.label + " scripts: ") + err);
      return -1;
    }
    if (!mod) {
      mod = std::move(next);
      log(LogLevel::Info, be.label + " scripts: module loaded (" + fs::path(b.module).filename().string() + ")");
      for (auto& in : insts) if (!in->obj) create(*in);
      return 0;
    }
    swap(std::move(next));
    return 1;
  }

  // Save every instance with the old code, swap, restore with the new.
  void swap(std::unique_ptr<CompiledModule> next) {
    std::vector<std::string> saved(insts.size());
    for (std::size_t i = 0; i < insts.size(); ++i) {
      Inst& in = *insts[i];
      if (!in.obj || !mod) continue;
      {
        const int need = mod->save(in.obj, nullptr, 0);
        saved[i].resize(static_cast<std::size_t>(std::max(0, need)));
        if (need > 0) mod->save(in.obj, saved[i].data(), need);
      }
      mod->destroy(in.obj);
      in.obj = nullptr;
    }
    const std::string old_path = mod ? mod->path : std::string();
    mod = std::move(next);
    std::error_code ec;
    if (!old_path.empty()) fs::remove(old_path, ec);  // best effort (Windows may hold it briefly)
    int good = 0;
    for (std::size_t i = 0; i < insts.size(); ++i) {
      Inst& in = *insts[i];
      if (!create(in)) continue;
      mod->load(in.obj, saved[i].data(), static_cast<int>(saved[i].size()));
      ++good;
      if (in.started) call(in, TS_HOOK_RELOAD, 0, 0, "");
    }
    log(LogLevel::Info, "Reloaded " + be.label + " scripts (" + std::to_string(good) + " of " +
                            std::to_string(insts.size()) + " running)");
  }

  // Background build done? Install it. 1 when it hot-swapped.
  int poll(bool wait = false) {
    if (!building()) return 0;
    if (!wait && pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return 0;
    const ScriptBuildResult b = pending.get();
    stamp = pending_stamp;
    return install(b) == 1 ? 1 : 0;
  }

  bool create(Inst& in) {
    if (!mod) return false;
    const std::string props = encode_script_props(in.props);
    in.obj = mod->create(in.stem.c_str(), &api, in.entity, props.c_str());
    if (!in.obj) {
      log(LogLevel::Error, "no " + be.label + " script class registered as \"" + in.stem + "\" (" +
                               be.class_hint(in.stem) + ") -- \"" + in.name + "\" (" + in.path + ")",
          in.path, 0, in.entity);
    }
    return in.obj != nullptr;
  }

  Inst* find(std::uint64_t e, int slot) const {
    for (const auto& i : insts) if (i->entity == e && i->slot == slot) return i.get();
    return nullptr;
  }

  void call(Inst& in, int hook, std::uint64_t e, double n, const std::string& text) {
    if (!in.obj || !in.enabled || !mod) return;
    mod->call(in.obj, hook, e, n, text.c_str());
  }
};

CompiledScriptHost::CompiledScriptHost(ScriptWorld& world, std::string project_dir, CompiledBackend backend)
    : m_(std::make_unique<Impl>()) {
  m_->be = std::move(backend);
  m_->world = &world;
  m_->project_dir = std::move(project_dir);
  m_->api = make_script_api(&world);
}

CompiledScriptHost::~CompiledScriptHost() {
  if (m_->building()) m_->pending.wait();
  if (m_->mod) {
    for (auto& i : m_->insts) {
      if (i->obj) m_->mod->destroy(i->obj);
    }
  }
  m_->insts.clear();
  m_->mod.reset();
}

bool CompiledScriptHost::attach(const Entity2D& e, int slot) {
  if (!e.script || m_->find(e.id, slot)) return false;
  auto in = std::make_unique<Inst>();
  in->entity = e.id;
  in->slot = slot;
  in->path = e.script->path;
  in->stem = stem_of(e.script->path);
  in->name = e.name;
  in->props = e.script->props;
  Inst& ref = *in;
  m_->insts.push_back(std::move(in));
  if (m_->project_dir.empty()) {
    m_->log(LogLevel::Error,
            m_->be.label + " scripts need a project folder -- \"" + e.name + "\" (" + ref.path + ")",
            ref.path, 0, e.id);
    return false;
  }
  if (!m_->ensure_module()) return m_->building();  // waits for the background build
  return m_->create(ref);
}

void CompiledScriptHost::detach(std::uint64_t entity) {
  // Every script on the entity, in stack order (on_destroy first to last).
  for (auto it = m_->insts.begin(); it != m_->insts.end();) {
    if ((*it)->entity != entity) {
      ++it;
      continue;
    }
    Inst& in = **it;
    if (in.obj && m_->mod) {
      if (in.started) m_->call(in, TS_HOOK_DESTROY, 0, 0, "");
      m_->mod->destroy(in.obj);
    }
    it = m_->insts.erase(it);
  }
}

bool CompiledScriptHost::has(std::uint64_t e, int slot) const { return m_->find(e, slot) != nullptr; }
bool CompiledScriptHost::running(std::uint64_t e, int slot) const {
  const Inst* i = m_->find(e, slot);
  return i && i->obj && i->enabled;
}
bool CompiledScriptHost::has_hook(std::uint64_t e, int slot, const char* hook) {
  const Inst* i = m_->find(e, slot);
  if (!i || !i->obj) return false;
  const std::string h = hook;
  return h == "on_start" || h == "on_tick" || h == "on_interact" || h == "on_trigger_enter" ||
         h == "on_trigger_exit" || h == "on_reload" || h.rfind("on_", 0) == 0;
}
std::size_t CompiledScriptHost::instance_count() const { return m_->insts.size(); }

void CompiledScriptHost::start_pending(int slot) {
  for (std::size_t i = 0; i < m_->insts.size(); ++i) {
    Inst& in = *m_->insts[i];
    if (in.slot != slot || in.started || !in.obj) continue;
    in.started = true;
    m_->call(in, TS_HOOK_START, 0, 0, "");
  }
}

void CompiledScriptHost::trigger(bool enter, std::uint64_t t, int slot, std::uint64_t other) {
  if (Inst* in = m_->find(t, slot); in && in->started) {
    m_->call(*in, enter ? TS_HOOK_TRIGGER_ENTER : TS_HOOK_TRIGGER_EXIT, other, 0, "");
  }
}
void CompiledScriptHost::interact(std::uint64_t t, int slot, std::uint64_t rider) {
  if (Inst* in = m_->find(t, slot); in && in->started) m_->call(*in, TS_HOOK_INTERACT, rider, 0, "");
}
void CompiledScriptHost::hand_event(std::uint64_t t, int slot, const char* hook, const std::string& text,
                                    double n) {
  if (Inst* in = m_->find(t, slot); in && in->started) {
    m_->call(*in, TS_HOOK_HAND, 0, n, std::string(hook) + "\t" + text);
  }
}
void CompiledScriptHost::ui_event(const std::string& widget, bool change, double value, const std::string& text) {
  for (std::size_t i = 0; i < m_->insts.size(); ++i) {
    Inst& in = *m_->insts[i];
    if (in.started) m_->call(in, TS_HOOK_UI, change ? 1 : 0, value, widget + "\t" + text);
  }
}
void CompiledScriptHost::broadcast(int slot, const char* hook, const std::string& text, double n) {
  std::vector<std::uint64_t> ids;
  for (const auto& in : m_->insts) if (in->started && in->slot == slot) ids.push_back(in->entity);
  std::sort(ids.begin(), ids.end());
  const std::string payload = std::string(hook) + "\t" + text;
  for (std::uint64_t id : ids) {
    if (Inst* in = m_->find(id, slot); in && in->started) m_->call(*in, TS_HOOK_EVENT, 0, n, payload);
  }
}
void CompiledScriptHost::tick(double dt, int slot) {
  if (slot == 0) m_->poll();  // housekeeping once per world tick
  const std::size_t n = m_->insts.size();
  for (std::size_t i = 0; i < n && i < m_->insts.size(); ++i) {
    Inst& in = *m_->insts[i];
    if (in.slot == slot && in.started) m_->call(in, TS_HOOK_TICK, 0, dt, "");
  }
}

void CompiledScriptHost::fixed_tick(double dt, int slot) {
  for (std::size_t i = 0; i < m_->insts.size(); ++i) {
    Inst& in = *m_->insts[i];
    if (in.slot == slot && in.started) m_->call(in, TS_HOOK_FIXED, 0, dt, "");
  }
}

void CompiledScriptHost::late_tick(double dt, int slot) {
  for (std::size_t i = 0; i < m_->insts.size(); ++i) {
    Inst& in = *m_->insts[i];
    if (in.slot == slot && in.started) m_->call(in, TS_HOOK_LATE, 0, dt, "");
  }
}

bool CompiledScriptHost::uses(const std::string& rel) const {
  for (const auto& i : m_->insts) if (i->path == rel) return true;
  return false;
}

int CompiledScriptHost::reload_changed() {
  if (m_->prebuilt || m_->insts.empty() || m_->project_dir.empty()) return 0;
  if (m_->building()) return m_->poll();
  const std::int64_t now = m_->be.stamp(m_->project_dir);
  if (now == m_->stamp && m_->mod) return 0;
  m_->stamp = now;
  if (!compiled_scripts_blocking()) {
    m_->log(LogLevel::Info, m_->be.label + " scripts changed: rebuilding in the background");
    m_->start_build(true);
    return 0;
  }
  return reload("") ? 1 : 0;
}

bool CompiledScriptHost::reload(const std::string&) {
  Impl& m = *m_;
  if (m.building()) m.poll(true);
  const ScriptBuildResult b = m.be.build(m.project_dir, true);
  const int r = m.install(b);
  return r == 1 || (r == 0 && m.mod);
}

bool CompiledScriptHost::building() const { return m_->building(); }
bool CompiledScriptHost::wait_build() { return m_->poll(true) == 1 || m_->mod != nullptr; }

const char* CompiledScriptHost::language() const { return m_->be.lang.c_str(); }
bool CompiledScriptHost::module_loaded() const { return m_->mod != nullptr; }
const std::string& CompiledScriptHost::module_path() const {
  static const std::string none;
  return m_->mod ? m_->mod->path : none;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
