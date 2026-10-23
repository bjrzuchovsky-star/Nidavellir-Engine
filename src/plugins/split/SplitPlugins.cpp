// The five split plugins (physics backends + rider controllers) as Trading
// Post entries, the provider switches they flip, and the 0.2.0 migration.

#include "plugins/split/Split.h"

#include "physics2d/Physics2D.h"
#include "physics3d/Physics3D.h"
#include "plugins/movement2d/Riders2D.h"
#include "plugins/physics_box2d/Box2DWorld.h"
#include "runtime/RiderController2D.h"
#include "scene/JsonTree.h"
#include "world3d/RiderController3D.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace ts {
namespace tombstone {
namespace plugins {

namespace fs = std::filesystem;

namespace {

struct Spec {
  const char* id;
  const char* name;
  const char* description;
  std::uint32_t types;
};

const Spec kSpecs[] = {
    {kJoltId, "Jolt Physics (3D)",
     "3D / VR rigid bodies on Jolt: dynamic and kinematic bodies, triggers as sensors, joints, "
     "ray and shape casts, impacts, VR grab-and-throw and the rider's character sweep. Off: "
     "bodies stay put, the rider uses the core box collision.",
     plugin::kType3D | plugin::kTypeVR},
    {kBox2DId, "Box2D Physics (2D)",
     "2D rigid bodies on Box2D v3: dynamic / kinematic bodies, joints, ray casts, hit events and "
     "the Shipyard's Box2D hulls. Off: bodies stay put; riders, tiles and colliders keep the "
     "core collision.",
     plugin::kType2D},
    {kRider3DId, "Rider 3D (first / third person)",
     "The 3D Player controller: WASD / stick walking, jump, first- or third-person view, look "
     "keys and right stick, mouse look with Sensitivity, Invert Y, Smoothing and cursor "
     "capture. Also rides a VR claim flat on the desktop.",
     plugin::kType3D | plugin::kTypeVR},
    {kTopDownId, "Top-down Rider (2D)",
     "The 2D top-down Player controller: eight-way stick / WASD movement with wall sliding and "
     "Face mouse aiming (cursor or right stick).",
     plugin::kType2D},
    {kPlatformerId, "Platformer Rider (2D)",
     "The 2D platformer Player controller: walk, gravity, jump off the ground, ceiling bumps.",
     plugin::kType2D},
};

class SplitPlugin final : public plugin::Plugin {
 public:
  explicit SplitPlugin(const Spec& s) {
    info_.id = s.id;
    info_.name = s.name;
    info_.description = s.description;
    info_.author = "TombStone";
    info_.version = plugin::Version{1, 0, 0};
    info_.category = plugin::PluginCategory::Engine;
    info_.project_types = s.types;
    info_.default_enabled = false;  // claims say what they use (templates, migration)
  }
  const plugin::PluginInfo& info() const override { return info_; }
  bool on_load(const plugin::PluginContext&, std::string*) override {
    acquire_provider(info_.id);
    held_ = true;
    return true;
  }
  void on_register(plugin::Registrar& reg) override {
    if (info_.id == kJoltId) {
      reg.add_component("rigid_body", "3D rigid body on Jolt (dynamic / kinematic)");
    } else if (info_.id == kBox2DId) {
      reg.add_component("body", "2D rigid body on Box2D");
    } else {
      reg.add_component("player", std::string("Player movement: ") + info_.name);
    }
  }
  void on_unload() override {
    if (held_) release_provider(info_.id);
    held_ = false;
  }
  ~SplitPlugin() override {
    if (held_) release_provider(info_.id);
  }

 private:
  plugin::PluginInfo info_;
  bool held_ = false;
};

std::string now_utc() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_all(const fs::path& p, const std::string& text, std::string* error) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out) {
    if (error) *error = "could not write " + p.string();
    return false;
  }
  return true;
}

std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    const auto b = cur.find_first_not_of(" \t");
    const auto e = cur.find_last_not_of(" \t");
    if (b != std::string::npos) {
      const std::string id = cur.substr(b, e - b + 1);
      if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
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

std::string join_list(const std::vector<std::string>& ids) {
  std::string s;
  for (const std::string& id : ids) s += (s.empty() ? "" : ",") + id;
  return s;
}

void append_log(const fs::path& claim, const std::vector<std::string>& lines) {
  std::error_code ec;
  fs::create_directories((claim / kMigrationLogRel).parent_path(), ec);
  std::ofstream out(claim / kMigrationLogRel, std::ios::app);
  for (const std::string& l : lines) out << now_utc() << "  " << l << '\n';
}

}  // namespace

const std::vector<std::string>& split_plugin_ids() {
  static const std::vector<std::string> ids = {kJoltId, kBox2DId, kRider3DId, kTopDownId,
                                               kPlatformerId};
  return ids;
}

bool is_split_plugin(const std::string& id) {
  const auto& ids = split_plugin_ids();
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void register_split_plugins(plugin::Catalog& catalog, std::vector<std::string>* errors) {
  for (const Spec& s : kSpecs) {
    std::string err;
    const Spec* spec = &s;
    if (!catalog.add_static([spec] { return std::make_unique<SplitPlugin>(*spec); }, &err) &&
        errors) {
      errors->push_back(err);
    }
  }
}

void offer_builtin_providers() {
  physics2d::offer_box2d();
  offer_2d_riders();
}

void acquire_provider(const std::string& id) {
  if (id == kJoltId) physics3d::Backends::instance().acquire(id);
  else if (id == kBox2DId) physics2d::Backends::instance().acquire(id);
  else if (id == kRider3DId) world3d::RiderControllers3D::instance().acquire(id);
  else if (id == kTopDownId || id == kPlatformerId) runtime::RiderControllers2D::instance().acquire(id);
}

void release_provider(const std::string& id) {
  if (id == kJoltId) physics3d::Backends::instance().release(id);
  else if (id == kBox2DId) physics2d::Backends::instance().release(id);
  else if (id == kRider3DId) world3d::RiderControllers3D::instance().release(id);
  else if (id == kTopDownId || id == kPlatformerId) runtime::RiderControllers2D::instance().release(id);
}

bool provider_active(const std::string& id) {
  if (id == kJoltId) return physics3d::Backends::instance().active(id);
  if (id == kBox2DId) return physics2d::Backends::instance().active(id);
  if (id == kRider3DId) return world3d::RiderControllers3D::instance().active(id);
  if (id == kTopDownId || id == kPlatformerId) return runtime::RiderControllers2D::instance().active(id);
  return false;
}

std::string provider_summary() {
  std::string out;
  for (const std::string& id : split_plugin_ids()) {
    if (provider_active(id)) out += (out.empty() ? "" : ", ") + id;
  }
  return out.empty() ? "none" : out;
}

void ProviderHold::hold(const std::vector<std::string>& ids) {
  for (const std::string& id : ids) {
    if (!is_split_plugin(id)) continue;
    if (std::find(held_.begin(), held_.end(), id) != held_.end()) continue;
    acquire_provider(id);
    held_.push_back(id);
  }
}

void ProviderHold::clear() {
  for (const std::string& id : held_) release_provider(id);
  held_.clear();
}

// ---- Migration ---------------------------------------------------------------

bool claim_is_split(const std::string& claim_dir) {
  std::error_code ec;
  const fs::path dir(claim_dir);
  if (fs::is_regular_file(dir / kMarkerRel, ec)) return true;
  json::Value pj;
  std::string err;
  if (!json::parse(read_all(dir / "project.json"), &pj, &err) || !pj.is_object()) return false;
  return pj.has("plugin_split");
}

bool mark_claim_split(const std::string& claim_dir, const std::vector<std::string>& added,
                      std::string* error) {
  json::Value m = json::Value::object();
  m.set("version", json::Value::number(1));
  m.set("engine", json::Value::string("0.2.0"));
  m.set("when", json::Value::string(now_utc()));
  json::Value arr = json::Value::array();
  for (const std::string& id : added) arr.push(json::Value::string(id));
  m.set("added", std::move(arr));
  return write_all(fs::path(claim_dir) / kMarkerRel, json::write(m), error);
}

MigrationReport migrate_claim(const std::string& claim_dir,
                              const std::vector<std::string>& defaults) {
  MigrationReport r;
  const fs::path dir(claim_dir);
  std::error_code ec;
  if (claim_dir.empty() || !fs::is_regular_file(dir / "project.json", ec)) return r;
  if (claim_is_split(claim_dir)) return r;
  r.ran = true;
  json::Value pj;
  std::string err;
  const std::string text = read_all(dir / "project.json");
  if (!json::parse(text, &pj, &err) || !pj.is_object()) {
    r.error = "project.json unreadable (" + err + "); plugins not migrated";
    r.log.push_back("Plugin split: " + r.error);
    return r;
  }
  const bool listed = pj.has("plugins");
  std::vector<std::string> enabled = listed ? split_list(pj["plugins"].as_string_or("")) : defaults;
  const std::vector<std::string> needs = required_plugins(claim_dir, &r.why);
  for (const std::string& id : needs) {
    if (std::find(enabled.begin(), enabled.end(), id) != enabled.end()) continue;
    enabled.push_back(id);
    r.added.push_back(id);
  }
  r.enabled = enabled;
  if (!r.added.empty()) {
    // Back up project.json, then write the new list (all other keys and
    // their order kept).
    fs::path backup = dir / kBackupDirRel / "project.json.pre-0.2.0";
    for (int n = 2; fs::exists(backup, ec); ++n) {
      backup = dir / kBackupDirRel / ("project.json.pre-0.2.0-" + std::to_string(n));
    }
    if (!write_all(backup, text, &err)) {
      r.error = "backup failed: " + err + "; plugins not migrated";
      r.log.push_back("Plugin split: " + r.error);
      return r;
    }
    r.backup = fs::relative(backup, dir, ec).generic_string();
    pj.set("plugins", json::Value::string(join_list(enabled)));
    if (!write_all(dir / "project.json", json::write(pj), &err)) {
      r.error = err;
      r.log.push_back("Plugin split: " + err);
      return r;
    }
    r.changed = true;
  }
  if (!mark_claim_split(claim_dir, r.added, &err)) {
    r.error = err;
    r.log.push_back("Plugin split: " + err);
  }
  std::vector<std::string> lines;
  if (r.added.empty()) {
    lines.push_back("0.2.0 plugin split: nothing to add (no physics / movement in use)");
  } else {
    lines.push_back("0.2.0 plugin split: physics and movement are Trading Post plugins now; "
                    "switched on " + join_list(r.added) + " for this claim (backup: " +
                    r.backup + ")");
    for (const std::string& w : r.why) lines.push_back("  " + w);
  }
  append_log(dir, lines);
  r.log.insert(r.log.end(), lines.begin(), lines.end());
  return r;
}

std::vector<std::string> settle_new_claim(const std::string& claim_dir,
                                          const std::vector<std::string>& defaults,
                                          std::string* error) {
  std::vector<std::string> added;
  const fs::path dir(claim_dir);
  json::Value pj;
  std::string err;
  if (!json::parse(read_all(dir / "project.json"), &pj, &err) || !pj.is_object()) {
    if (error) *error = "project.json unreadable: " + err;
    return added;
  }
  const bool listed = pj.has("plugins");
  std::vector<std::string> enabled = listed ? split_list(pj["plugins"].as_string_or("")) : defaults;
  for (const std::string& id : required_plugins(claim_dir)) {
    if (std::find(enabled.begin(), enabled.end(), id) != enabled.end()) continue;
    enabled.push_back(id);
    added.push_back(id);
  }
  if (!added.empty()) {
    pj.set("plugins", json::Value::string(join_list(enabled)));
    if (!write_all(dir / "project.json", json::write(pj), &err)) {
      if (error) *error = err;
      return {};
    }
  }
  if (!mark_claim_split(claim_dir, added, &err) && error) *error = err;
  return added;
}

std::vector<std::string> effective_plugins(const std::string& claim_dir,
                                           const std::vector<std::string>& listed, bool is_listed,
                                           const std::vector<std::string>& defaults,
                                           std::vector<std::string>* added) {
  std::vector<std::string> ids = is_listed ? listed : defaults;
  if (claim_dir.empty() || claim_is_split(claim_dir)) return ids;
  for (const std::string& id : required_plugins(claim_dir)) {
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
    ids.push_back(id);
    if (added) added->push_back(id);
  }
  return ids;
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
