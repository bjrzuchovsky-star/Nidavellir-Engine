// ts_admin --smoke-plugin-split: physics and rider movement as Trading Post
// plugins (0.2.0). Four parts:
//   1. templates switch on the plugins they need (and new claims are split)
//   2. a pre-0.2.0 claim migrates once: plugins added, backup, marker, log;
//      reopening changes nothing and a plugin switched off stays off
//   3. play with and without the plugins (2D + 3D): no-op physics, riders
//      stand still with a Telegraph warning, nothing crashes
//   4. an export ships only the plugins the claim uses
// It drops the ambient provider hold main() takes for smoke runs and puts
// it back at the end.

#include "SmokePluginSplit.h"

#include "editor/ProjectInfo.h"
#include "editor/launch/GameLauncher.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/projects/ProjectStore.h"
#include "editor/templates/TemplateStore.h"
#include "export/Exporter.h"
#include "export/Plugins.h"
#include "physics2d/Physics2D.h"
#include "physics3d/Physics3D.h"
#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "render3d/Scene3D.h"
#include "runtime/Input.h"
#include "runtime/World.h"
#include "scene/SceneData.h"
#include "scene/SceneJson.h"
#include "vfs/Vfs.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {

namespace {

namespace s3 = scene3d;
namespace w3 = world3d;
namespace sp = plugins;

int fail(const std::string& why) {
  std::cerr << "[smoke] plugin split FAILED: " << why << '\n';
  smoke_provider_hold().hold(sp::split_plugin_ids());
  return 1;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream(p, std::ios::binary) << text;
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) out += (out.empty() ? "" : ",") + s;
  return out;
}

bool has(const std::vector<std::string>& v, const std::string& id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

std::vector<std::string> sorted(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  return v;
}

std::vector<std::string> ids_of(const std::string& list) {
  std::vector<std::string> out;
  std::stringstream ss(list);
  std::string id;
  while (std::getline(ss, id, ',')) {
    if (!id.empty()) out.push_back(id);
  }
  return out;
}

// ---- Scenes ----------------------------------------------------------------------

s3::Entity3D ground3d() {
  s3::Entity3D g;
  g.id = 100;
  g.name = "Ground";
  g.transform.position = {0.0f, -0.5f, 0.0f};
  s3::Collider3D c;
  c.size = {60.0f, 1.0f, 60.0f};
  g.collider = c;
  return g;
}

s3::Entity3D rider3d() {
  s3::Entity3D p;
  p.id = 1;
  p.name = "Player";
  p.transform.position = {0.0f, 1.0f, 0.0f};
  s3::Collider3D c;
  c.shape = s3::Collider3D::Shape::Capsule;
  c.size = {0.4f, 2.0f, 0.4f};
  p.collider = c;
  p.player = s3::Player3D{};
  return p;
}

s3::Entity3D crate3d() {
  s3::Entity3D b;
  b.id = 2;
  b.name = "Crate";
  b.transform.position = {4.0f, 5.0f, 0.0f};
  s3::Collider3D c;
  c.size = {1.0f, 1.0f, 1.0f};
  b.collider = c;
  b.rigid_body = s3::RigidBody3D{};
  return b;
}

s3::Scene3D scene3d_with(bool rider, bool crate) {
  s3::Scene3D s;
  s.entities.push_back(ground3d());
  if (rider) s.entities.push_back(rider3d());
  if (crate) s.entities.push_back(crate3d());
  return s;
}

Entity2D rider2d(bool platformer) {
  Entity2D p;
  p.id = 1;
  p.name = "Player";
  p.x = 100.0f;
  p.y = 100.0f;
  p.w = 24.0f;
  p.h = 40.0f;
  p.player = PlayerControllerData{};
  if (platformer) p.player->mode = PlayerControllerData::Mode::Platformer;
  p.collider = default_collider(p);
  return p;
}

Entity2D crate2d() {
  Entity2D b;
  b.id = 2;
  b.name = "Crate";
  b.x = 400.0f;
  b.y = 100.0f;
  b.w = 32.0f;
  b.h = 32.0f;
  b.collider = default_collider(b);
  b.body = RigidBody2DData{};
  return b;
}

bool save_2d(const fs::path& dir, std::vector<Entity2D> entities, std::string* err) {
  scene_json::SceneDoc doc;
  doc.entities = std::move(entities);
  return scene_json::save_file(scene_json::scene_path_for_project(dir.string()), doc, err);
}

bool save_3d(const fs::path& dir, const s3::Scene3D& s, std::string* err) {
  return s3::save_file(s3::scene_path(dir.string()), s, err);
}

// ---- 1. Templates ------------------------------------------------------------------

int templates_part(const fs::path& root, std::string* note) {
  editor::TemplateStore ts;
  ts.refresh_dirs(editor::TemplateStore::engine_template_dirs(),
                  editor::TemplateStore::user_template_dir(root.string()));
  if (!ts.errors().empty()) return fail("templates load: " + ts.errors().front());
  editor::ProjectStore store(root.string());
  store.refresh();
  struct Want {
    const char* id;
    std::vector<std::string> needs;
  };
  const std::vector<Want> wants = {
      {"empty-3d-fps", {sp::kJoltId, sp::kRider3DId}},
      {"empty-3d-tps", {sp::kJoltId, sp::kRider3DId}},
      {"empty-vr", {sp::kJoltId, sp::kRider3DId}},
      {"empty-2d-topdown", {sp::kTopDownId}},
      {"empty-2d-platformer", {sp::kPlatformerId}},
      {"empty-3d", {}},
      {"dusty-town", {sp::kTopDownId}},
      {"ghost-town-3d", {sp::kJoltId, sp::kRider3DId}},
      {"haunted-saloon-vr", {sp::kJoltId, sp::kRider3DId}},
  };
  int n = 0;
  for (const Want& w : wants) {
    const editor::ProjectTemplate* t = ts.find(w.id);
    if (!t) return fail(std::string("template missing: ") + w.id);
    for (const std::string& id : w.needs) {
      if (!has(t->plugins, id)) return fail(std::string(w.id) + " should declare " + id);
    }
    editor::ProjectInfo p;
    std::string err;
    if (!editor::TemplateStore::create_project(store, *t, std::string("Split ") + w.id, &p, &err)) {
      return fail(std::string("create from ") + w.id + ": " + err);
    }
    if (!sp::claim_is_split(p.path)) return fail(std::string(w.id) + ": new claims are split");
    for (const std::string& id : w.needs) {
      if (!has(p.plugins, id)) return fail(std::string(w.id) + ": claim should enable " + id);
    }
    for (const std::string& id : p.plugins) {
      if (sp::is_split_plugin(id) && !has(w.needs, id)) {
        return fail(std::string(w.id) + ": unneeded " + id + " switched on");
      }
    }
    // Opening the claim hitches the plugins, and they switch the
    // providers on; closing switches them off again.
    editor::EditorPlugins ep;
    std::vector<std::string> said;
    ep.open(&p, "smoke", nullptr, [&](runtime::LogLevel, const std::string& s) { said.push_back(s); });
    for (const std::string& id : w.needs) {
      if (!ep.loaded(id) || !sp::provider_active(id)) {
        return fail(std::string(w.id) + ": " + id + " should ride when the claim opens");
      }
    }
    for (const std::string& s : said) {
      if (s.find("plugin split") != std::string::npos) {
        return fail(std::string(w.id) + ": a new claim must not migrate (" + s + ")");
      }
    }
    ep.close();
    for (const std::string& id : sp::split_plugin_ids()) {
      if (sp::provider_active(id)) return fail(id + " still active after the claim closed");
    }
    ++n;
  }
  *note += std::to_string(n) + " templates auto-add; ";
  return 0;
}

// ---- 2. Migration ------------------------------------------------------------------

int migration_part(const fs::path& root, std::string* note) {
  // A 3D claim from 0.1.x: a rider and a crate with a rigid body, ts.ai on.
  const fs::path dir = root / "old-rider-3d";
  const std::string original =
      "{\n  \"name\": \"Old Rider 3D\",\n  \"dimension\": \"3d\",\n  \"created\": "
      "\"2026-05-01 10:00:00\",\n  \"plugins\": \"ts.ai\",\n  \"version\": \"0.3.1\"\n}\n";
  write_file(dir / "project.json", original);
  std::string err;
  if (!save_3d(dir, scene3d_with(true, true), &err)) return fail("old scene: " + err);
  if (sp::claim_is_split(dir.string())) return fail("an old claim is not split yet");

  // The runtime rides it with the plugins it needs, in memory only.
  std::vector<std::string> added;
  const std::vector<std::string> eff =
      sp::effective_plugins(dir.string(), {"ts.ai"}, true, {}, &added);
  if (sorted(added) != sorted({sp::kJoltId, sp::kRider3DId}) || !has(eff, "ts.ai")) {
    return fail("runtime effective list: " + join(eff));
  }
  if (read_file(dir / "project.json") != original) return fail("the runtime must not rewrite");

  // The editor migrates it once.
  editor::ProjectInfo info;
  if (!editor::ProjectStore::load_project_json(dir.string(), &info, &err)) return fail(err);
  std::vector<std::string> said;
  auto sink = [&](runtime::LogLevel, const std::string& s) { said.push_back(s); };
  {
    editor::EditorPlugins ep;
    ep.open(&info, "smoke", nullptr, sink);
    if (!ep.loaded(sp::kJoltId) || !ep.loaded(sp::kRider3DId) || !ep.loaded("ts.ai")) {
      return fail("migrated claim plugins: " + join(ep.enabled()));
    }
    if (!sp::provider_active(sp::kJoltId) || !sp::provider_active(sp::kRider3DId)) {
      return fail("migrated claim providers");
    }
  }
  bool told = false;
  for (const std::string& s : said) told = told || s.find("0.2.0 plugin split") != std::string::npos;
  if (!told) return fail("the Telegraph should say what the migration did");
  editor::ProjectInfo disk;
  editor::ProjectStore::load_project_json(dir.string(), &disk, &err);
  if (disk.plugins != std::vector<std::string>{"ts.ai", sp::kJoltId, sp::kRider3DId}) {
    return fail("project.json after migration: " + join(disk.plugins));
  }
  const std::string after = read_file(dir / "project.json");
  if (after.find("\"version\": \"0.3.1\"") == std::string::npos ||
      after.find("2026-05-01 10:00:00") == std::string::npos) {
    return fail("the migration must keep other project.json keys: " + after);
  }
  const fs::path backup = dir / sp::kBackupDirRel / "project.json.pre-0.2.0";
  if (read_file(backup) != original) return fail("backup of project.json");
  if (!fs::is_regular_file(dir / sp::kMarkerRel)) return fail("marker");
  const std::string log1 = read_file(dir / sp::kMigrationLogRel);
  if (log1.find("switched on") == std::string::npos ||
      log1.find(sp::kJoltId) == std::string::npos || log1.find("Player") == std::string::npos) {
    return fail("migrations.log: " + log1);
  }

  // Reopen: nothing more happens.
  said.clear();
  {
    editor::ProjectInfo again;
    editor::ProjectStore::load_project_json(dir.string(), &again, &err);
    editor::EditorPlugins ep;
    ep.open(&again, "smoke", nullptr, sink);
    // The user switches Jolt off: it stays off.
    if (!ep.set_enabled(sp::kJoltId, false, &err)) return fail("switch Jolt off: " + err);
  }
  for (const std::string& s : said) {
    if (s.find("plugin split") != std::string::npos) return fail("a split claim migrates again");
  }
  if (fs::exists(dir / sp::kBackupDirRel / "project.json.pre-0.2.0-2")) return fail("second backup");
  if (read_file(dir / sp::kMigrationLogRel) != log1) return fail("second log entry");
  {
    editor::ProjectInfo third;
    editor::ProjectStore::load_project_json(dir.string(), &third, &err);
    editor::EditorPlugins ep;
    ep.open(&third, "smoke", nullptr, sink);
    if (ep.loaded(sp::kJoltId) || !ep.loaded(sp::kRider3DId) || sp::provider_active(sp::kJoltId)) {
      return fail("Jolt switched off should stay off: " + join(ep.enabled()));
    }
  }

  // A 2D claim without a list (defaults) and a platformer + crate.
  const fs::path d2 = root / "old-rider-2d";
  write_file(d2 / "project.json", "{\n  \"name\": \"Old Rider 2D\",\n  \"dimension\": \"2d\"\n}\n");
  if (!save_2d(d2, {rider2d(true), crate2d()}, &err)) return fail("old 2D scene: " + err);
  {
    editor::ProjectInfo i2;
    editor::ProjectStore::load_project_json(d2.string(), &i2, &err);
    editor::EditorPlugins ep;
    ep.open(&i2, "smoke", nullptr, sink);
    if (!ep.loaded(sp::kPlatformerId) || !ep.loaded(sp::kBox2DId) || ep.loaded(sp::kTopDownId) ||
        !ep.loaded("ts.posse")) {
      return fail("2D migration: " + join(ep.enabled()));
    }
  }
  if (!fs::is_regular_file(d2 / sp::kBackupDirRel / "project.json.pre-0.2.0")) {
    return fail("2D backup");
  }

  // A claim with no physics / movement: marked, project.json untouched.
  const fs::path d3 = root / "old-quiet";
  const std::string quiet = "{\n  \"name\": \"Old Quiet\",\n  \"dimension\": \"3d\"\n}\n";
  write_file(d3 / "project.json", quiet);
  if (!save_3d(d3, scene3d_with(false, false), &err)) return fail("quiet scene: " + err);
  const sp::MigrationReport r = sp::migrate_claim(d3.string(), {});
  if (!r.ran || r.changed || !r.added.empty() || read_file(d3 / "project.json") != quiet ||
      fs::exists(d3 / sp::kBackupDirRel) || !sp::claim_is_split(d3.string())) {
    return fail("a claim without physics should only be marked");
  }
  *note += "migration (backup, marker, log, reopen no-op, off stays off, 2D, quiet); ";
  return 0;
}

// ---- 3. Play -----------------------------------------------------------------------

struct Ride3D {
  bool started = false;
  bool physics = false;
  float moved = 0.0f;
  float crate_drop = 0.0f;
  std::vector<std::string> warnings;
};

Ride3D ride3d() {
  Ride3D r;
  w3::World3D w;
  w.set_log_sink([&](const runtime::LogEntry& e) {
    if (e.level == runtime::LogLevel::Warn) r.warnings.push_back(e.text);
  });
  std::string why;
  r.started = w.start(scene3d_with(true, true), "", &why);
  if (!r.started) return r;
  r.physics = w.physics() != nullptr;
  w.run_ticks(20, {});
  const render3d::Vec3 a = w.find(w.player_id())->transform.position;
  const float crate0 = w.find(2)->transform.position.y;
  w3::Input3D in;
  in.move_forward = 1.0f;
  w.run_ticks(60, in);
  const render3d::Vec3 b = w.find(w.player_id())->transform.position;
  r.moved = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
  (void)crate0;
  r.crate_drop = 5.0f - w.find(2)->transform.position.y;  // authored at y = 5
  if (b.y < -1.0f) r.moved = -1.0f;  // fell through the world
  w.stop();
  return r;
}

struct Ride2D {
  bool built = false;
  bool physics = false;
  float moved = 0.0f;
  float crate_drop = 0.0f;
  std::vector<std::string> warnings;
};

Ride2D ride2d(bool platformer) {
  Ride2D r;
  runtime::World w;
  std::string err;
  r.built = w.build({rider2d(platformer), crate2d()}, "", &err);
  if (!r.built) return r;
  r.physics = w.physics2d() != nullptr;
  const float x0 = w.find(1)->data.x;
  const float y0 = w.find(2)->data.y;
  runtime::InputFrame in;
  in.slot(0).move_x = 1.0f;
  in.slot(0).connected = true;
  for (int i = 0; i < 30; ++i) w.step(in);
  r.moved = w.find(1)->data.x - x0;
  r.crate_drop = w.find(2)->data.y - y0;
  for (const runtime::LogEntry& e : w.take_logs()) {
    if (e.level == runtime::LogLevel::Warn) r.warnings.push_back(e.text);
  }
  return r;
}

bool warned(const std::vector<std::string>& w, const std::string& id) {
  for (const std::string& s : w) {
    if (s.find(id) != std::string::npos) return true;
  }
  return false;
}

int play_part(std::string* note) {
  // Without any plugin: no-op physics, the rider stands still, a warning
  // names the plugin to switch on.
  for (const std::string& id : sp::split_plugin_ids()) {
    if (sp::provider_active(id)) return fail("providers off for the no-plugin ride: " + id);
  }
  if (physics3d::create_world() || physics2d::create_world()) {
    return fail("no physics plugin, no physics world");
  }
  const Ride3D off3 = ride3d();
  if (!off3.started || off3.physics) return fail("3D without plugins should start, no physics");
  if (off3.moved < 0.0f || off3.moved > 0.01f) return fail("3D: no Rider 3D, no walking");
  if (std::fabs(off3.crate_drop) > 0.001f) return fail("3D: no Jolt, the crate stays put");
  if (!warned(off3.warnings, sp::kRider3DId) || !warned(off3.warnings, sp::kJoltId)) {
    return fail("3D: the Telegraph should name Rider 3D and Jolt");
  }
  for (bool platformer : {false, true}) {
    const Ride2D off2 = ride2d(platformer);
    if (!off2.built || off2.physics) return fail("2D without plugins should build, no physics");
    if (std::fabs(off2.moved) > 0.01f) return fail("2D: no rider plugin, no walking");
    if (std::fabs(off2.crate_drop) > 0.001f) return fail("2D: no Box2D, the crate stays put");
    if (!warned(off2.warnings, platformer ? sp::kPlatformerId : sp::kTopDownId) ||
        !warned(off2.warnings, sp::kBox2DId)) {
      return fail("2D: the Telegraph should name the rider plugin and Box2D");
    }
  }

  // With the plugins.
  sp::ProviderHold hold;
  hold.hold({sp::kJoltId, sp::kRider3DId});
  const Ride3D on3 = ride3d();
  if (!on3.started || !on3.physics) return fail("3D with Jolt should have a physics world");
  if (on3.moved < 0.5f) return fail("3D: Rider 3D walks (" + std::to_string(on3.moved) + " m)");
  if (on3.crate_drop < 0.3f) return fail("3D: Jolt drops the crate");
  if (!on3.warnings.empty()) return fail("3D with plugins: " + on3.warnings.front());
  hold.clear();
  hold.hold({sp::kBox2DId, sp::kTopDownId, sp::kPlatformerId});
  for (bool platformer : {false, true}) {
    const Ride2D on2 = ride2d(platformer);
    if (!on2.built || !on2.physics) return fail("2D with Box2D should have a physics world");
    if (on2.moved < 10.0f) return fail("2D: the rider walks (" + std::to_string(on2.moved) + " px)");
    if (on2.crate_drop < 1.0f) return fail("2D: Box2D drops the crate");
  }
  // Only the rider plugin, no Box2D: walking works, bodies stay put.
  hold.clear();
  hold.hold({sp::kTopDownId});
  const Ride2D mix = ride2d(false);
  if (mix.moved < 10.0f || std::fabs(mix.crate_drop) > 0.001f) {
    return fail("2D: a rider plugin without Box2D walks, bodies stay put");
  }
  hold.clear();
  *note += "play 2D + 3D with and without plugins; ";
  return 0;
}

// ---- 4. Export ---------------------------------------------------------------------

std::string game_json_plugins(const fs::path& game_dir) {
  const std::string text = read_file(game_dir / "game.json");
  const std::string key = "\"plugins\":";
  std::size_t at = text.find(key);
  if (at == std::string::npos) return "<missing>";
  at = text.find('"', at + key.size());
  const std::size_t end = text.find('"', at + 1);
  return text.substr(at + 1, end - at - 1);
}

int export_part(const fs::path& root, std::string* note) {
  struct Claim {
    std::string name;
    bool three_d;
    std::string listed;      // project.json "plugins" ("-" = no key)
    bool split;              // marked past the split
    std::string want;        // shipped list
  };
  const std::vector<Claim> claims = {
      {"Export Topdown", false, sp::kTopDownId, true, sp::kTopDownId},
      {"Export Rider3D", true, std::string(sp::kJoltId) + "," + sp::kRider3DId, true,
       std::string(sp::kJoltId) + "," + sp::kRider3DId},
      {"Export Bare", true, "", true, ""},
      // A pre-0.2.0 claim: ships what its scene needs; Posse is editor-only.
      {"Export Old", false, "ts.posse", false, std::string(sp::kTopDownId) + "," + sp::kBox2DId},
  };
  const std::string runtime = editor::launcher::find_game_executable();
  int ran = 0;
  for (const Claim& c : claims) {
    const fs::path dir = root / "export" / editor::ProjectStore::make_slug(c.name);
    std::string pj = "{\n  \"name\": \"" + c.name + "\",\n  \"dimension\": \"" +
                     (c.three_d ? "3d" : "2d") + "\"";
    if (c.listed != "-") pj += ",\n  \"plugins\": \"" + c.listed + "\"";
    write_file(dir / "project.json", pj + "\n}\n");
    std::string err;
    bool ok = true;
    if (c.name == "Export Topdown") ok = save_2d(dir, {rider2d(false)}, &err);
    // The exported 3D --smoke wants something drawn: the starter claim.
    if (c.name == "Export Rider3D") ok = save_3d(dir, s3::make_starter_scene(), &err);
    if (c.name == "Export Bare") {
      s3::Scene3D bare = s3::make_starter_scene();
      for (s3::Entity3D& e : bare.entities) {
        e.player.reset();
        e.rigid_body.reset();
        e.grabbable.reset();
        e.joint.reset();
      }
      ok = save_3d(dir, bare, &err);
    }
    if (c.name == "Export Old") ok = save_2d(dir, {rider2d(false), crate2d()}, &err);
    if (!ok) return fail(c.name + " scene: " + err);
    if (c.split) sp::mark_claim_split(dir.string(), {});

    // What the export would ship (the same list the exporter writes).
    std::vector<std::string> used;
    for (const exporter::PluginNeed& n : exporter::project_plugins(dir.string(), {})) {
      if (!n.editor_only) used.push_back(n.id);
    }
    if (join(sorted(used)) != join(sorted(ids_of(c.want)))) {
      return fail(c.name + ": export list " + join(used) + " (want " + c.want + ")");
    }
    if (runtime.empty()) continue;

    exporter::ExportOptions opt;
    opt.project_dir = dir.string();
    opt.out_dir = (root / "export_out").string();
    opt.runtime_exe = runtime;
    opt.profile = "dev";
    opt.check_dependencies = false;
    exporter::ExportResult res;
    if (!exporter::export_project(opt, &res)) return fail(c.name + " export: " + res.text());
    const std::string shipped = game_json_plugins(res.game_dir);
    if (join(sorted(ids_of(shipped))) != join(sorted(ids_of(c.want)))) {
      return fail(c.name + ": game.json plugins \"" + shipped + "\" (want \"" + c.want + "\")");
    }
    // The cooked project.json pins the list and is past the split.
    vfs::Vfs v;
    std::string cooked;
    if (!v.mount_pak((fs::path(res.game_dir) / "game.tspak").string(), &err) ||
        !v.read("project.json", &cooked)) {
      return fail(c.name + ": read the pak's project.json: " + err);
    }
    if (cooked.find("\"plugin_split\"") == std::string::npos) {
      return fail(c.name + ": the cooked project.json is marked split: " + cooked);
    }
    const std::vector<std::string> want_ids = ids_of(c.want);
    for (const std::string& id : sp::split_plugin_ids()) {
      const bool want = has(want_ids, id);
      if (!want && cooked.find(id) != std::string::npos) {
        return fail(c.name + ": " + id + " should not ship: " + cooked);
      }
    }
    // Ride the export (from another folder) and check what it rode with.
    std::string cmd = "\"" + res.exe_path + "\" --smoke";
    const std::string log = (fs::path(res.game_dir) / "split_smoke.log").string();
#if defined(_WIN32)
    cmd = "\"" + cmd + " > \"" + log + "\" 2>&1\"";
#else
    cmd = "cd / && LD_LIBRARY_PATH=\"" + res.game_dir + "/lib\" " + cmd + " > \"" + log + "\" 2>&1";
#endif
    const bool needs_rider = c.name != "Export Bare";
    const int rc = std::system(cmd.c_str());
    const std::string out = read_file(log);
    std::string want_line = "[ts_game] physics / movement: ";
    {
      std::string list;
      for (const std::string& id : sp::split_plugin_ids()) {
        if (has(ids_of(c.want), id)) list += (list.empty() ? "" : ", ") + id;
      }
      want_line += list.empty() ? "none" : list;
    }
    if (out.find(want_line) == std::string::npos) {
      return fail(c.name + ": exported game should say \"" + want_line + "\" (see " + log + ")");
    }
    // The 3D --smoke wants a rider; the bare claim has none, so it only has
    // to get that far (scene loaded, no crash).
    if (needs_rider && rc != 0) return fail(c.name + ": exported ts_game --smoke failed (see " + log + ")");
    if (!needs_rider && out.find("no rider") == std::string::npos && rc != 0) {
      return fail(c.name + ": the bare export should load and run (see " + log + ")");
    }
    ++ran;
  }
  *note += runtime.empty() ? "export lists checked (ts_game not built: no export run)"
                           : "export: " + std::to_string(ran) + " claims shipped only their plugins and rode";
  return 0;
}

}  // namespace

int run_plugin_split_smoke() {
  const fs::path root = fs::path("TombStoneProjects") / "_plugin_split_smoke";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  // The five plugins, as the Trading Post lists them.
  {
    plugin::Catalog cat;
    plugins::register_builtin_plugins(cat, nullptr);
    struct Row {
      const char* id;
      std::uint32_t types;
    };
    for (const Row& r : {Row{sp::kJoltId, plugin::kType3D | plugin::kTypeVR},
                         Row{sp::kBox2DId, plugin::kType2D},
                         Row{sp::kRider3DId, plugin::kType3D | plugin::kTypeVR},
                         Row{sp::kTopDownId, plugin::kType2D},
                         Row{sp::kPlatformerId, plugin::kType2D}}) {
      const plugin::CatalogEntry* e = cat.find(r.id);
      if (!e) return fail(std::string("not in the catalog: ") + r.id);
      if (e->info.default_enabled || e->info.category != plugin::PluginCategory::Engine ||
          e->info.project_types != r.types || e->info.name.empty() || e->info.description.empty()) {
        return fail(std::string("catalog entry: ") + r.id);
      }
    }
  }

  // Ride without the ambient hold from here on.
  smoke_provider_hold().clear();
  std::string note;
  int rc = templates_part(root / "templates", &note);
  if (rc == 0) rc = migration_part(root / "migration", &note);
  if (rc == 0) rc = play_part(&note);
  if (rc == 0) rc = export_part(root, &note);
  smoke_provider_hold().hold(sp::split_plugin_ids());
  if (rc != 0) return rc;
  fs::remove_all(root, ec);
  std::cout << "[smoke] plugin split OK (" << note << ")\n";
  return 0;
}

}  // namespace tombstone
}  // namespace ts
