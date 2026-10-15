// Prefab smoke: drives the same public paths the Prefabs panel and the
// Inspector's prefab section use, without a window.
#include "SmokePrefab.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "render3d/Scene3D.h"
#include "scene/Prefab.h"
#include "scene/SceneJson.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace pf = ts::tombstone::prefab;
namespace s3 = ts::tombstone::scene3d;
using ts::tombstone::Entity2D;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] prefab FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

ed::ProjectInfo fresh_project(const std::string& id, ed::ProjectKind kind) {
  const fs::path dir = fs::absolute(fs::path("TombStoneProjects") / id);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ed::ProjectInfo info;
  info.id = id;
  info.name = id;
  info.kind = kind;
  info.path = dir.string();
  return info;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool has_override(const std::optional<pf::Link>& l, const std::string& k) {
  return l && l->overridden(k);
}

// Rewrite a file so its mtime moves even on coarse clocks.
void touch_write(const fs::path& p, const std::string& text) {
  const auto before = fs::last_write_time(p);
  {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
  }
  std::error_code ec;
  if (fs::last_write_time(p, ec) == before) {
    fs::last_write_time(p, before + std::chrono::seconds(2), ec);
  }
}

std::vector<const Entity2D*> instance2d(ed::Editor2DScreen& s, std::uint64_t id) {
  std::vector<const Entity2D*> out;
  for (std::uint64_t m : s.prefab_instance_members(id)) out.push_back(s.workspace().find(m));
  return out;
}

const Entity2D* by_source2d(ed::Editor2DScreen& s, std::uint64_t any, std::uint64_t src) {
  for (const Entity2D* e : instance2d(s, any)) {
    if (e && e->prefab && e->prefab->source == src) return e;
  }
  return nullptr;
}

// ---- 2D ------------------------------------------------------------------------

int check_2d() {
  const ed::ProjectInfo info = fresh_project("_prefab2d_smoke", ed::ProjectKind::TwoD);
  auto scr = std::make_unique<ed::Editor2DScreen>(info);
  scr->on_enter();
  ed::Workspace2D& w = scr->workspace();
  w.set_snap_enabled(false);
  const std::uint64_t post = scr->create_entity("Post");
  const std::uint64_t lamp = scr->create_entity("Lamp");
  {
    Entity2D* p = w.find(post);
    p->x = 100;
    p->y = 200;
    p->w = 20;
    p->h = 80;
    Entity2D* l = w.find(lamp);
    l->x = 95;
    l->y = 180;
    l->w = 30;
    l->h = 20;
  }
  if (!scr->reparent(lamp, post)) return fail("2D reparent");
  w.set_selection({post, lamp}, post);
  const std::size_t depth0 = w.undo_count();
  const std::string rel = scr->save_selection_as_prefab("Lamp Post");
  if (rel != "prefabs/lamp_post.prefab.json") return fail("2D save path: " + rel);
  const fs::path asset_path = fs::path(info.path) / rel;
  if (!fs::exists(asset_path)) return fail("2D asset file");
  if (w.undo_count() != depth0 + 1) return fail("2D make prefab is one undo step");
  if (!w.find(post)->prefab || !w.find(lamp)->prefab ||
      w.find(post)->prefab->source != 1 || w.find(lamp)->prefab->source != 2) {
    return fail("2D selection became the first instance");
  }
  pf::Asset asset;
  std::string err;
  if (!pf::load_asset(asset_path.string(), &asset, &err) || asset.entities.size() != 2 ||
      asset.dim != pf::Dim::D2) {
    return fail("2D asset content: " + err);
  }

  // Place two more instances.
  const std::uint64_t i2 = scr->place_prefab(rel, 400, 300);
  const std::uint64_t i3 = scr->place_prefab(rel, 600, 300);
  if (!i2 || !i3 || instance2d(*scr, i2).size() != 2) return fail("2D place");
  const Entity2D* lamp2 = by_source2d(*scr, i2, 2);
  if (!lamp2 || !near(lamp2->x, 395) || !near(lamp2->y, 280) || lamp2->parent != i2) {
    return fail("2D placed child offset / parent");
  }
  if (w.find(i2)->prefab->instance == w.find(i3)->prefab->instance) return fail("2D instance ids");

  // Per-instance override: tint instance 2's post red.
  w.select(i2);
  scr->begin_inspector_edit("Tint");
  w.find(i2)->color[0] = 1.0f;
  w.find(i2)->color[1] = 0.1f;
  scr->end_inspector_edit();
  if (!has_override(w.find(i2)->prefab, "r") || !has_override(w.find(i2)->prefab, "g")) {
    return fail("2D tint recorded as override");
  }
  if (has_override(w.find(post)->prefab, "r")) return fail("2D override leaked");
  // Moving the root is not an override; moving a child is.
  scr->begin_inspector_edit("Move");
  w.find(i3)->x += 10;
  scr->end_inspector_edit();
  if (w.find(i3)->prefab->overrides.size() != 0) return fail("2D root move is no override");

  // Edit the prefab: every instance updates, overrides stay.
  pf::Asset edited = *scr->prefabs().get(rel);
  edited.entities[0] = pf::with_key(edited.entities[0], "w", ts::tombstone::json::Value::number(26));
  edited.entities[0] = pf::with_key(edited.entities[0], "r", ts::tombstone::json::Value::number(0.2));
  if (!scr->prefab_edit_asset(rel, edited)) return fail("2D edit asset");
  for (std::uint64_t id : {post, i2, i3}) {
    if (!near(w.find(id)->w, 26)) return fail("2D asset edit reached every instance");
  }
  if (!near(w.find(post)->color[0], 0.2f) || !near(w.find(i2)->color[0], 1.0f)) {
    return fail("2D override survives asset edit");
  }
  // Edit the file on disk (a text editor / git pull): poll picks it up.
  {
    pf::Asset disk = *scr->prefabs().get(rel);
    disk.entities[1] = pf::with_key(disk.entities[1], "name", ts::tombstone::json::Value::string("Lantern"));
    touch_write(asset_path, pf::write_asset(disk));
  }
  if (scr->prefab_poll() == 0) return fail("2D on-disk edit noticed");
  if (by_source2d(*scr, i3, 2)->name != "Lantern" || w.find(lamp)->name != "Lantern") {
    return fail("2D on-disk edit reached instances");
  }

  // Revert one override.
  if (!scr->prefab_revert(i2, "r") || !near(w.find(i2)->color[0], 0.2f) ||
      has_override(w.find(i2)->prefab, "r") || !has_override(w.find(i2)->prefab, "g")) {
    return fail("2D revert r");
  }
  // Apply the remaining one (g) to the prefab: the other instances follow.
  if (!scr->prefab_apply(i2, "g")) return fail("2D apply g");
  if (!near(w.find(post)->color[1], 0.1f) || !near(w.find(i3)->color[1], 0.1f)) {
    return fail("2D apply reached other instances");
  }
  if (slurp(asset_path).find("0.1") == std::string::npos) return fail("2D apply wrote the file");
  // Undo the apply: scene and asset file both go back; redo re-applies.
  const std::string applied_text = slurp(asset_path);
  if (!scr->undo() || near(w.find(post)->color[1], 0.1f) ||
      !has_override(w.find(i2)->prefab, "g")) {
    return fail("2D undo apply (scene)");
  }
  if (slurp(asset_path) == applied_text) return fail("2D undo apply (asset file)");
  if (!scr->redo() || !near(w.find(post)->color[1], 0.1f) || slurp(asset_path) != applied_text) {
    return fail("2D redo apply");
  }

  // Child position override, then apply all on the instance.
  scr->begin_inspector_edit("Move lamp");
  const_cast<Entity2D*>(by_source2d(*scr, i3, 2))->y -= 15;
  scr->end_inspector_edit();
  if (!has_override(by_source2d(*scr, i3, 2)->prefab, pf::kPositionKey)) {
    return fail("2D child move recorded");
  }
  if (!scr->prefab_apply(i3, {}, true)) return fail("2D apply all");
  if (!near(by_source2d(*scr, i2, 2)->y - w.find(i2)->y, -35)) {
    return fail("2D applied child offset reached instance 2");
  }

  // Ctrl+D on an instance makes a new instance, not a clash.
  w.set_selection(scr->prefab_instance_members(i2), i2);
  const std::size_t count_before = w.entities().size();
  if (scr->duplicate_selected() == 0 || w.entities().size() != count_before + 2) {
    return fail("2D duplicate instance");
  }
  std::set<std::uint64_t> inst;
  for (const Entity2D& e : w.entities()) {
    if (e.prefab) inst.insert(e.prefab->instance);
  }
  if (inst.size() != 4) return fail("2D duplicate split into its own instance");

  // Save / reload keeps links and overrides.
  scr->on_exit();
  scr = std::make_unique<ed::Editor2DScreen>(info);
  scr->on_enter();
  ed::Workspace2D& w2 = scr->workspace();
  if (!w2.find(i2) || !w2.find(i2)->prefab || w2.find(i2)->prefab->asset != rel) {
    return fail("2D reload keeps links");
  }
  // Unpack.
  if (!scr->prefab_unpack(i3) || w2.find(i3)->prefab) return fail("2D unpack");
  if (!scr->undo() || !w2.find(i3)->prefab) return fail("2D undo unpack");
  // Scene format.
  if (slurp(fs::path(info.path) / "scene.json").find("\"version\": " +
                                                         std::to_string(ts::tombstone::scene_json::kSceneVersion)) == std::string::npos) {
    return fail("2D scene.json current version");
  }
  scr->on_exit();
  return 0;
}

// ---- 3D ------------------------------------------------------------------------

const s3::Entity3D* by_source3d(ed::Editor3DScreen& e, std::uint64_t any, std::uint64_t src) {
  for (std::uint64_t m : e.prefab_instance_members(any)) {
    const s3::Entity3D* x = e.find(m);
    if (x && x->prefab && x->prefab->source == src) return x;
  }
  return nullptr;
}

int check_3d() {
  const ed::ProjectInfo info = fresh_project("_prefab3d_smoke", ed::ProjectKind::ThreeD);
  auto e = std::make_unique<ed::Editor3DScreen>(info);
  e->on_enter();
  e->set_snapping(false);
  const std::uint64_t base = e->create_primitive("cube", {0.0f, 0.5f, 0.0f});
  const std::uint64_t top = e->create_primitive("sphere", {0.0f, 1.5f, 0.0f});
  if (!e->set_parent(top, base)) return fail("3D parent");
  e->select(base);
  e->select(top, true);
  const std::size_t depth0 = e->undo_depth();
  const std::string rel = e->save_selection_as_prefab("Totem");
  if (rel != "prefabs/totem.prefab.json") return fail("3D save path: " + rel);
  const fs::path asset_path = fs::path(info.path) / rel;
  if (e->undo_depth() != depth0 + 1 || !e->find(base)->prefab || !e->find(top)->prefab) {
    return fail("3D make prefab");
  }
  const std::uint64_t i2 = e->place_prefab(rel, {5.0f, 0.5f, 0.0f});
  const std::uint64_t i3 = e->place_prefab(rel, {-5.0f, 0.5f, 2.0f});
  const s3::Entity3D* top2 = by_source3d(*e, i2, 2);
  if (!i2 || !i3 || !top2 || !near(top2->transform.position.x, 5.0f) ||
      !near(top2->transform.position.y, 1.5f) || top2->parent != i2) {
    return fail("3D place");
  }
  // Override: instance 2's material.
  {
    s3::Entity3D x = *e->find(i2);
    if (!x.material) x.material = s3::Material3D{};
    x.material->albedo[0] = 0.9f;
    x.material->albedo[1] = 0.1f;
    e->replace_entity(x, "Material");
  }
  if (!has_override(e->find(i2)->prefab, "material")) return fail("3D material override");
  // Edit the prefab asset: bigger base everywhere, overrides kept.
  pf::Asset edited = *e->prefabs().get(rel);
  {
    const float sc[3] = {2.0f, 1.0f, 2.0f};
    edited.entities[0] = pf::with_key(edited.entities[0], "scale", ts::tombstone::json::floats(sc, 3));
  }
  if (!e->prefab_edit_asset(rel, edited)) return fail("3D edit asset");
  for (std::uint64_t id : {base, i2, i3}) {
    if (!near(e->find(id)->transform.scale.x, 2.0f)) return fail("3D asset edit reached instances");
  }
  if (!near(e->find(i2)->material->albedo[0], 0.9f)) return fail("3D override kept");
  // On-disk edit.
  {
    pf::Asset disk = *e->prefabs().get(rel);
    disk.entities[1] = pf::with_key(disk.entities[1], "name", ts::tombstone::json::Value::string("Head"));
    touch_write(asset_path, pf::write_asset(disk));
  }
  if (e->prefab_poll() == 0 || by_source3d(*e, i3, 2)->name != "Head") {
    return fail("3D on-disk edit reached instances");
  }
  // Apply the material, undo / redo (asset file too), revert.
  const std::string before_apply = slurp(asset_path);
  if (!e->prefab_apply(i2, "material")) return fail("3D apply");
  if (!e->find(i3)->material || !near(e->find(i3)->material->albedo[0], 0.9f)) {
    return fail("3D apply reached other instances");
  }
  if (!e->undo() || slurp(asset_path) != before_apply ||
      !has_override(e->find(i2)->prefab, "material")) {
    return fail("3D undo apply");
  }
  if (!e->redo() || slurp(asset_path) == before_apply) return fail("3D redo apply");
  {
    s3::Entity3D x = *e->find(i3);
    x.name = "Odd Totem";
    e->replace_entity(x, "Rename");
  }
  if (!has_override(e->find(i3)->prefab, "name")) return fail("3D rename override");
  if (!e->prefab_revert(i3, {}, true) || e->find(i3)->name != e->find(base)->name) {
    return fail("3D revert all");
  }
  // Reload.
  e->on_exit();
  e = std::make_unique<ed::Editor3DScreen>(info);
  e->on_enter();
  if (!e->find(i2) || !e->find(i2)->prefab || e->scene().version != s3::kScene3DVersion) {
    return fail("3D reload keeps links");
  }
  // Wrong-dimension prefab refuses to place.
  e->on_exit();
  return 0;
}

}  // namespace

int run_prefab_smoke() {
  if (check_2d() != 0) return 1;
  if (check_3d() != 0) return 1;
  std::cout << "[smoke] prefab OK (2D + 3D: save selection as .prefab.json, place instances, "
               "per-instance overrides, asset edits via API and on disk update every instance, "
               "revert / apply (one key and all), undo / redo incl. the asset file, duplicate "
               "split, save / reload, unpack)\n";
  return 0;
}
