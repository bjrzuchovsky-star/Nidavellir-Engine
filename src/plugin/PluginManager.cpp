#include "plugin/PluginManager.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <set>
#include <utility>

namespace ts {
namespace tombstone {
namespace plugin {

using runtime::LogLevel;

bool Resolution::has_errors() const {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic& d) { return d.level == LogLevel::Error; });
}

std::vector<std::string> split_plugin_list(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&]() {
    std::size_t a = 0;
    std::size_t b = cur.size();
    while (a < b && std::isspace(static_cast<unsigned char>(cur[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(cur[b - 1]))) --b;
    std::string item = cur.substr(a, b - a);
    if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) {
      out.push_back(std::move(item));
    }
    cur.clear();
  };
  for (char c : text) {
    if (c == ',') {
      flush();
    } else {
      cur += c;
    }
  }
  flush();
  return out;
}

std::string join_plugin_list(const std::vector<std::string>& ids) {
  std::string out;
  for (const std::string& id : ids) {
    if (id.empty()) continue;
    if (!out.empty()) out += ",";
    out += id;
  }
  return out;
}

// --- Registrar ------------------------------------------------------------------

class PluginManager::Collector final : public Registrar {
 public:
  Collector(PluginManager& m, std::string id) : m_(m), id_(std::move(id)) {}
  const std::string& plugin_id() const override { return id_; }

  void add_component(const std::string& name,
                     const std::string& description) override {
    m_.world_->components.push_back({name, id_, description});
  }
  void add_system(const std::string& name,
                  std::function<void(runtime::World&, double)> run) override {
    if (!run) return;
    m_.world_->systems.push_back({name, id_, std::move(run)});
  }
  void add_lua_api(const std::string& name,
                   std::function<void(lua_State*)> push) override {
    if (name.empty() || !push) return;
    for (const auto& api : m_.world_->lua) {
      if (api.name == name) {
        m_.report(LogLevel::Warn, id_,
                  "Lua table '" + name + "' is already claimed by " +
                      api.owner + "; this one is ignored");
        return;
      }
    }
    m_.world_->lua.push_back({name, id_, std::move(push)});
  }
  void add_asset_type(const AssetType& type) override {
    AssetType t = type;
    std::transform(t.extension.begin(), t.extension.end(), t.extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t.extension.empty()) return;
    if (t.extension[0] != '.') t.extension.insert(t.extension.begin(), '.');
    m_.assets_.push_back({id_, std::move(t)});
  }
  void add_editor_panel(EditorPanel panel) override {
    if (!panel.draw || panel.title.empty()) return;
    if (panel.menu_label.empty()) panel.menu_label = panel.title;
    m_.panels_.push_back({id_, std::move(panel)});
  }
  void add_menu_item(EditorMenuItem item) override {
    if (item.menu.empty()) item.menu = "Tools";
    m_.menus_.push_back({id_, std::move(item)});
  }
  void add_draw_layer(const std::string& name, bool over, DrawFn draw) override {
    if (!draw) return;
    m_.world_->layers.push_back({name, id_, over, std::move(draw)});
  }
  void add_blocker(const std::string& name, SolidsFn solids) override {
    if (!solids) return;
    m_.world_->blockers.push_back({name, id_, std::move(solids)});
  }
  void add_net_channel(const std::string& name, ReceiveFn receive) override {
    if (name.empty() || !receive) return;
    for (const auto& ch : m_.world_->channels) {
      if (ch.name == name) {
        m_.report(LogLevel::Warn, id_,
                  "network channel '" + name + "' is already claimed by " + ch.owner +
                      "; this one is ignored");
        return;
      }
    }
    m_.world_->channels.push_back({name, id_, std::move(receive)});
  }

 private:
  PluginManager& m_;
  std::string id_;
};

// --- Manager --------------------------------------------------------------------

PluginManager::PluginManager(const Catalog& catalog)
    : catalog_(catalog), world_(std::make_shared<runtime::WorldExtensions>()) {}

PluginManager::~PluginManager() {
  // No sink calls from a half-destroyed owner.
  sink_ = nullptr;
  unload_all();
}

void PluginManager::report(LogLevel level, const std::string& plugin,
                           std::string text) {
  Diagnostic d{level, plugin, std::move(text)};
  if (sink_) sink_(d);
  diagnostics_.push_back(std::move(d));
}

std::size_t PluginManager::error_count() const {
  return static_cast<std::size_t>(std::count_if(
      diagnostics_.begin(), diagnostics_.end(),
      [](const Diagnostic& d) { return d.level == LogLevel::Error; }));
}

std::vector<std::string> PluginManager::defaults(const Catalog& catalog,
                                                 ProjectType type) {
  std::vector<std::string> out;
  for (const CatalogEntry& e : catalog.entries()) {
    if (e.info.default_enabled && (e.info.project_types & type_bit(type))) {
      out.push_back(e.info.id);
    }
  }
  return out;
}

Resolution PluginManager::resolve(const std::vector<std::string>& enabled,
                                  ProjectType type) const {
  Resolution r;
  auto diag = [&](LogLevel lv, const std::string& id, std::string text) {
    r.diagnostics.push_back({lv, id, std::move(text)});
  };

  // Candidates in catalog order (deterministic regardless of list order).
  std::vector<std::string> wanted;
  for (const std::string& id : enabled) {
    if (std::find(wanted.begin(), wanted.end(), id) != wanted.end()) continue;
    wanted.push_back(id);
  }
  std::set<std::string> active;
  for (const std::string& id : wanted) {
    const CatalogEntry* e = catalog_.find(id);
    if (!e) {
      diag(LogLevel::Warn, id,
           "'" + id + "' is switched on for this project but isn't in the "
           "wagon (missing plugin library?); riding on without it");
      r.skipped.push_back(id);
      continue;
    }
    if (!(e->info.project_types & type_bit(type))) {
      diag(LogLevel::Warn, id,
           e->info.name + " doesn't work in " + to_string(type) +
               " projects; left in the wagon");
      r.skipped.push_back(id);
      continue;
    }
    active.insert(id);
  }

  // Dependencies (removal cascades until nothing changes).
  std::map<std::string, std::string> why;  // first failure per plugin
  bool changed = true;
  while (changed) {
    changed = false;
    for (const CatalogEntry& e : catalog_.entries()) {
      if (!active.count(e.info.id)) continue;
      for (const Dependency& d : e.info.deps) {
        const CatalogEntry* dep = catalog_.find(d.id);
        std::string fail;
        if (!dep) {
          if (!d.optional) {
            fail = e.info.name + " needs " + d.id + " " + d.min.str() +
                   "+, which isn't installed";
          }
        } else if (!dep->info.version.satisfies(d.min)) {
          const std::string text =
              e.info.name + " needs " + dep->info.name + " " + d.min.str() +
              "+ (same major), found " + dep->info.version.str();
          if (d.optional) {
            if (!why.count(e.info.id + "#" + d.id)) {
              why[e.info.id + "#" + d.id] = text;
              diag(LogLevel::Warn, e.info.id, text + "; optional, ignored");
            }
          } else {
            fail = text;
          }
        } else if (!d.optional && !active.count(d.id)) {
          const bool was_enabled =
              std::find(wanted.begin(), wanted.end(), d.id) != wanted.end();
          fail = was_enabled
                     ? e.info.name + " needs " + dep->info.name +
                           ", which couldn't hitch up"
                     : e.info.name + " needs " + dep->info.name + " (" + d.id +
                           "); switch it on too";
        }
        if (!fail.empty()) {
          active.erase(e.info.id);
          why[e.info.id] = fail;
          diag(LogLevel::Error, e.info.id, fail);
          r.skipped.push_back(e.info.id);
          changed = true;
          break;
        }
      }
    }
  }

  // Kahn's sort; ready plugins go in catalog order.
  std::map<std::string, std::set<std::string>> needs;  // id -> unmet deps
  for (const std::string& id : active) {
    const CatalogEntry* e = catalog_.find(id);
    for (const Dependency& d : e->info.deps) {
      if (active.count(d.id)) needs[id].insert(d.id);
    }
  }
  std::set<std::string> placed;
  while (placed.size() < active.size()) {
    const CatalogEntry* next = nullptr;
    for (const CatalogEntry& e : catalog_.entries()) {
      const std::string& id = e.info.id;
      if (!active.count(id) || placed.count(id)) continue;
      bool ready = true;
      for (const std::string& d : needs[id]) {
        if (!placed.count(d)) {
          ready = false;
          break;
        }
      }
      if (ready) {
        next = &e;
        break;
      }
    }
    if (!next) break;  // the rest are in a loop
    placed.insert(next->info.id);
    r.order.push_back(next->info.id);
  }
  if (placed.size() < active.size()) {
    std::string loop;
    for (const CatalogEntry& e : catalog_.entries()) {
      if (active.count(e.info.id) && !placed.count(e.info.id)) {
        if (!loop.empty()) loop += " -> ";
        loop += e.info.id;
      }
    }
    for (const CatalogEntry& e : catalog_.entries()) {
      if (active.count(e.info.id) && !placed.count(e.info.id)) {
        diag(LogLevel::Error, e.info.id,
             "dependency loop (" + loop + "); " + e.info.name +
                 " can't hitch up");
        r.skipped.push_back(e.info.id);
      }
    }
  }
  return r;
}

std::size_t PluginManager::load(const std::vector<std::string>& enabled,
                                const PluginContext& ctx) {
  unload_all();
  diagnostics_.clear();
  ctx_ = ctx;

  Resolution r = resolve(enabled, ctx.project_type);
  for (Diagnostic& d : r.diagnostics) {
    report(d.level, d.plugin, std::move(d.text));
  }

  std::set<std::string> up;
  for (const std::string& id : r.order) {
    const CatalogEntry* e = catalog_.find(id);
    // A dependency that failed at load time takes its dependents down.
    std::string missing;
    for (const Dependency& d : e->info.deps) {
      if (!d.optional && !up.count(d.id)) {
        missing = d.id;
        break;
      }
    }
    if (!missing.empty()) {
      report(LogLevel::Error, id,
             e->info.name + " needs " + missing + ", which failed to load");
      continue;
    }
    Plugin* p = nullptr;
    try {
      p = e->create();
    } catch (const std::exception& ex) {
      report(LogLevel::Error, id, e->info.name + " wouldn't build: " + ex.what());
      continue;
    } catch (...) {
      report(LogLevel::Error, id, e->info.name + " wouldn't build");
      continue;
    }
    if (!p) {
      report(LogLevel::Error, id, e->info.name + " wouldn't build");
      continue;
    }
    std::string err;
    bool ok = false;
    try {
      ok = p->on_load(ctx_, &err);
    } catch (const std::exception& ex) {
      err = ex.what();
    } catch (...) {
      err = "unknown exception";
    }
    if (!ok) {
      report(LogLevel::Error, id,
             e->info.name + " refused to load: " + (err.empty() ? "no reason given" : err));
      e->destroy(p);
      continue;
    }
    // Contributions go in only if on_register finishes.
    const auto world_before = *world_;
    const std::size_t panels = panels_.size();
    const std::size_t menus = menus_.size();
    const std::size_t assets = assets_.size();
    try {
      Collector reg(*this, id);
      p->on_register(reg);
    } catch (const std::exception& ex) {
      err = ex.what();
      ok = false;
    } catch (...) {
      err = "unknown exception";
      ok = false;
    }
    if (!ok) {
      *world_ = world_before;
      panels_.resize(panels);
      menus_.resize(menus);
      assets_.resize(assets);
      report(LogLevel::Error, id, e->info.name + " failed to register: " + err);
      try {
        p->on_unload();
      } catch (...) {
      }
      e->destroy(p);
      continue;
    }
    loaded_.push_back({id, p, e});
    order_.push_back(id);
    up.insert(id);
    report(LogLevel::Info, id,
           "Hitched up " + e->info.name + " " + e->info.version.str() +
               (e->dynamic ? " (" + e->source + ")" : ""));
  }
  return loaded_.size();
}

void PluginManager::unload_all() {
  if (in_play_) play_end();
  for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
    try {
      it->plugin->on_unload();
    } catch (...) {
      report(LogLevel::Warn, it->id, "threw while unhitching");
    }
    it->entry->destroy(it->plugin);
  }
  loaded_.clear();
  order_.clear();
  panels_.clear();
  menus_.clear();
  assets_.clear();
  // A fresh object: a World still holding the old one keeps it alive, but
  // its functions belong to plugins that are gone -- callers stop rides
  // before unloading.
  world_ = std::make_shared<runtime::WorldExtensions>();
}

bool PluginManager::loaded(const std::string& id) const {
  return get(id) != nullptr;
}

Plugin* PluginManager::get(const std::string& id) const {
  for (const Loaded& l : loaded_) {
    if (l.id == id) return l.plugin;
  }
  return nullptr;
}

const Owned<AssetType>* PluginManager::asset_type_for(
    const std::string& filename) const {
  std::string lower = filename;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const Owned<AssetType>* best = nullptr;
  for (const auto& a : assets_) {
    const std::string& ext = a.item.extension;
    if (lower.size() >= ext.size() &&
        lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
      if (!best || ext.size() > best->item.extension.size()) best = &a;
    }
  }
  return best;
}

void PluginManager::play_begin(runtime::World& world) {
  if (in_play_) play_end();
  in_play_ = true;
  for (const Loaded& l : loaded_) {
    try {
      l.plugin->on_play_begin(world);
    } catch (const std::exception& ex) {
      report(LogLevel::Error, l.id, std::string("on_play_begin threw: ") + ex.what());
    } catch (...) {
      report(LogLevel::Error, l.id, "on_play_begin threw");
    }
  }
}

void PluginManager::play_end() {
  if (!in_play_) return;
  in_play_ = false;
  for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
    try {
      it->plugin->on_play_end();
    } catch (...) {
      report(LogLevel::Warn, it->id, "on_play_end threw");
    }
  }
}

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts
