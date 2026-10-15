// Prefabs in the 2D editor (same model as the 3D editor; docs/prefabs.md).
// The scene goes to entity JSON, scene/Prefab.h does the work, and each
// change lands as one Workspace2D undo step + autosave.
#include "editor/screens/Editor2DScreen.h"

#include "scene/SceneJson.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;
using prefab::Dim;

namespace {
bool any_instance(const std::vector<Entity2D>& ents) {
  return std::any_of(ents.begin(), ents.end(),
                     [](const Entity2D& e) { return e.prefab.has_value(); });
}
}  // namespace

bool Editor2DScreen::refresh_prefab_overrides() {
  std::vector<Entity2D>& ents = workspace_.entities();
  if (!any_instance(ents)) return false;
  std::vector<json::Value> js = scene_json::entities_to_json(ents);
  if (!prefab::refresh_overrides(Dim::D2, js, prefabs_.lookup())) return false;
  for (std::size_t i = 0; i < js.size() && i < ents.size(); ++i) {
    prefab::Link l;
    if (prefab::json_link(js[i], &l)) ents[i].prefab = l;
  }
  return true;
}

bool Editor2DScreen::prefab_set_entities(const std::vector<json::Value>& js,
                                         const std::string& label,
                                         Workspace2D::Snapshot before) {
  std::vector<Entity2D> ents;
  std::string err;
  if (!scene_json::entities_from_json(js, &ents, &err)) {
    note("Prefab: " + err);
    return false;
  }
  workspace_.replace_entities(std::move(ents));
  return commit_discrete(std::move(before), label, "");
}

void Editor2DScreen::prefab_sync_all_on_load() {
  if (!any_instance(workspace_.entities())) return;
  std::set<std::string> rels;
  for (const Entity2D& e : workspace_.entities()) {
    if (e.prefab) rels.insert(e.prefab->asset);
  }
  std::vector<json::Value> js = scene_json::entities_to_json(workspace_.entities());
  std::uint64_t next = workspace_.peek_next_id();
  std::size_t changed = 0;
  for (const std::string& rel : rels) {
    if (const prefab::Asset* a = prefabs_.get(rel)) {
      changed += prefab::sync_instances(Dim::D2, js, rel, *a, &next);
    } else {
      std::cout << "[Editor2D] prefab " << rel << " is missing; instances keep their values\n";
    }
  }
  if (changed == 0) return;
  std::vector<Entity2D> ents;
  std::string err;
  if (scene_json::entities_from_json(js, &ents, &err)) {
    workspace_.replace_entities(std::move(ents));
    std::cout << "[Editor2D] prefabs: " << changed
              << " instance entit(ies) caught up with their assets\n";
    mark_dirty_and_autosave();
  }
}

std::string Editor2DScreen::save_selection_as_prefab(const std::string& name,
                                                     std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    note("Save as Prefab: " + why);
    return std::string();
  };
  if (is_playing()) return fail("not while riding");
  const std::vector<std::uint64_t> roots = workspace_.selection_roots();
  if (roots.empty()) return fail("select something first");
  Workspace2D::Snapshot before = prepare_edit();
  std::set<std::uint64_t> ids(roots.begin(), roots.end());
  for (std::uint64_t r : roots) {
    for (std::uint64_t d : workspace_.descendants_of(r)) ids.insert(d);
  }
  std::vector<json::Value> picked;
  for (const json::Value& v : scene_json::entities_to_json(workspace_.entities())) {
    if (ids.count(prefab::json_id(v))) picked.push_back(v);
  }
  std::stable_partition(picked.begin(), picked.end(), [&](const json::Value& v) {
    return prefab::json_id(v) == roots.front();
  });
  std::map<std::uint64_t, std::uint64_t> local;
  const std::string display = name.empty() ? "Prefab" : name;
  prefab::Asset asset = prefab::make_asset(Dim::D2, display, picked, &local);
  std::string rel = prefab::default_asset_rel(display);
  for (int n = 2; fs::exists(fs::u8path(prefabs_.abs_path(rel))); ++n) {
    rel = prefab::default_asset_rel(display + " " + std::to_string(n));
  }
  const std::string label = "Make Prefab " + display;
  std::string err;
  if (!prefabs_.save(rel, asset, &err, workspace_.undo_count() + 1, label)) return fail(err);
  const std::uint64_t inst =
      prefab::max_instance(scene_json::entities_to_json(workspace_.entities())) + 1;
  for (Entity2D& e : workspace_.entities()) {
    const auto it = local.find(e.id);
    if (it == local.end()) continue;
    prefab::Link l;
    l.asset = rel;
    l.instance = inst;
    l.source = it->second;
    e.prefab = l;
  }
  commit_discrete(std::move(before), label,
                  "Saved " + std::to_string(ids.size()) + " entit(ies) as " + rel);
  return rel;
}

std::uint64_t Editor2DScreen::place_prefab(const std::string& rel, float x, float y) {
  if (is_playing()) return 0;
  const prefab::Asset* asset = prefabs_.get(rel);
  if (!asset) {
    note("Prefab " + rel + " does not load");
    return 0;
  }
  if (asset->dim != Dim::D2) {
    note(rel + " is a 3D prefab");
    return 0;
  }
  Workspace2D::Snapshot before = prepare_edit();
  std::vector<json::Value> js = scene_json::entities_to_json(workspace_.entities());
  std::uint64_t next = workspace_.peek_next_id();
  const std::uint64_t first = next;
  const double p[3] = {x, y, 0.0};
  for (json::Value& v : prefab::instantiate(*asset, rel, p, prefab::max_instance(js) + 1, &next)) {
    js.push_back(std::move(v));
  }
  std::vector<Entity2D> ents;
  std::string err;
  if (!scene_json::entities_from_json(js, &ents, &err)) {
    note("Place prefab: " + err);
    return 0;
  }
  workspace_.replace_entities(std::move(ents));
  workspace_.select(first);
  commit_discrete(std::move(before), "Place " + asset->name, "Placed " + asset->name);
  return first;
}

std::vector<std::uint64_t> Editor2DScreen::prefab_instance_members(std::uint64_t id) const {
  std::vector<std::uint64_t> out;
  const Entity2D* e = workspace_.find(id);
  if (!e || !e->prefab) return out;
  for (const Entity2D& o : workspace_.entities()) {
    if (o.prefab && o.prefab->asset == e->prefab->asset &&
        o.prefab->instance == e->prefab->instance) {
      out.push_back(o.id);
    }
  }
  return out;
}

bool Editor2DScreen::prefab_apply(std::uint64_t id, const std::string& key, bool whole) {
  const Entity2D* e = workspace_.find(id);
  if (is_playing() || !e || !e->prefab) return false;
  Workspace2D::Snapshot before = prepare_edit();
  refresh_prefab_overrides();
  const std::string rel = e->prefab->asset;
  const prefab::Asset* cur = prefabs_.get(rel);
  if (!cur) return false;
  prefab::Asset asset = *cur;
  std::vector<json::Value> js = scene_json::entities_to_json(workspace_.entities());
  if (!prefab::apply_overrides(Dim::D2, js, id, key, whole, &asset)) {
    note("Nothing to apply");
    return false;
  }
  std::uint64_t next = workspace_.peek_next_id();
  prefab::sync_instances(Dim::D2, js, rel, asset, &next);
  const std::string label = "Apply to Prefab " + asset.name;
  std::string err;
  if (!prefabs_.save(rel, asset, &err, workspace_.undo_count() + 1, label)) {
    note("Apply: " + err);
    return false;
  }
  if (!prefab_set_entities(js, label, std::move(before))) return false;
  note("Applied to " + rel + "; every instance updated");
  return true;
}

bool Editor2DScreen::prefab_revert(std::uint64_t id, const std::string& key, bool whole) {
  const Entity2D* e = workspace_.find(id);
  if (is_playing() || !e || !e->prefab) return false;
  Workspace2D::Snapshot before = prepare_edit();
  refresh_prefab_overrides();
  const prefab::Asset* asset = prefabs_.get(e->prefab->asset);
  if (!asset) return false;
  std::vector<json::Value> js = scene_json::entities_to_json(workspace_.entities());
  std::uint64_t next = workspace_.peek_next_id();
  if (!prefab::revert_overrides(Dim::D2, js, id, key, whole, *asset, &next)) return false;
  return prefab_set_entities(js, key.empty() ? "Revert Overrides" : "Revert " + key,
                             std::move(before));
}

bool Editor2DScreen::prefab_unpack(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (is_playing() || !e || !e->prefab) return false;
  Workspace2D::Snapshot before = prepare_edit();
  const std::vector<std::uint64_t> members = prefab_instance_members(id);
  for (Entity2D& o : workspace_.entities()) {
    if (std::find(members.begin(), members.end(), o.id) != members.end()) o.prefab.reset();
  }
  return commit_discrete(std::move(before), "Unpack Prefab", "Unpacked the prefab instance");
}

bool Editor2DScreen::prefab_edit_asset(const std::string& rel, const prefab::Asset& asset) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  refresh_prefab_overrides();
  const std::string label = "Edit Prefab " + asset.name;
  std::string err;
  if (!prefabs_.save(rel, asset, &err, workspace_.undo_count() + 1, label)) {
    note("Edit Prefab: " + err);
    return false;
  }
  std::vector<json::Value> js = scene_json::entities_to_json(workspace_.entities());
  std::uint64_t next = workspace_.peek_next_id();
  prefab::sync_instances(Dim::D2, js, rel, asset, &next);
  return prefab_set_entities(js, label, std::move(before));
}

std::size_t Editor2DScreen::prefab_poll() {
  if (is_playing() || workspace_.edit_open()) return 0;
  const std::vector<std::string> changed = prefabs_.poll_changes();
  if (changed.empty()) return 0;
  Workspace2D::Snapshot before = prepare_edit();
  std::vector<json::Value> js = scene_json::entities_to_json(workspace_.entities());
  std::uint64_t next = workspace_.peek_next_id();
  std::size_t n = 0;
  for (const std::string& rel : changed) {
    if (const prefab::Asset* a = prefabs_.get(rel)) {
      n += prefab::sync_instances(Dim::D2, js, rel, *a, &next);
    }
  }
  if (n > 0 && prefab_set_entities(js, "Prefab changed on disk", std::move(before))) {
    note("Prefab file changed: " + std::to_string(n) + " instance entit(ies) updated");
  }
  return n;
}

// ---- UI -----------------------------------------------------------------------

void Editor2DScreen::draw_prefabs_panel() {
  ImGui::TextDisabled("Save the selection, then place copies that follow it.");
  ImGui::SetNextItemWidth(-110);
  ImGui::InputText("##prefabname", prefab_name_, sizeof(prefab_name_));
  ImGui::SameLine();
  ImGui::BeginDisabled(workspace_.selection_count() == 0);
  if (ImGui::Button("Save Prefab")) save_selection_as_prefab(prefab_name_);
  ImGui::EndDisabled();
  ImGui::Separator();
  static std::vector<std::string> cached;
  static double last = -10.0;
  if (ImGui::GetTime() - last > 2.0) {
    cached = prefabs_.list();
    last = ImGui::GetTime();
  }
  if (cached.empty()) ImGui::TextDisabled("No prefabs yet (prefabs/*.prefab.json).");
  for (const std::string& rel : cached) {
    const prefab::Asset* a = prefabs_.get(rel);
    if (!a || a->dim != Dim::D2) continue;
    ImGui::PushID(rel.c_str());
    std::set<std::uint64_t> inst;
    for (const Entity2D& e : workspace_.entities()) {
      if (e.prefab && e.prefab->asset == rel) inst.insert(e.prefab->instance);
    }
    if (ImGui::SmallButton("Place")) {
      float x = 64.0f + static_cast<float>(inst.size() % 6) * 48.0f;
      float y = 64.0f + static_cast<float>(inst.size() % 4) * 48.0f;
      if (const Entity2D* s = workspace_.selected()) {
        x = s->x + 48.0f;
        y = s->y + 48.0f;
      }
      place_prefab(rel, x, y);
    }
    ImGui::SameLine();
    ImGui::Text("%s", a->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu ent, %zu placed)  %s", a->entities.size(), inst.size(),
                        rel.c_str());
    ImGui::PopID();
  }
}

void Editor2DScreen::draw_prefab_inspector(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || !e->prefab) return;
  const prefab::Link l = *e->prefab;
  ImGui::PushID("prefab");
  ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "Prefab: %s",
                     prefab::name_from_rel(l.asset).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("#%llu", static_cast<unsigned long long>(l.instance));
  if (!prefabs_.get(l.asset)) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "asset missing: %s", l.asset.c_str());
  }
  if (ImGui::SmallButton("Select instance")) {
    std::vector<std::uint64_t> m = prefab_instance_members(id);
    m.erase(std::remove(m.begin(), m.end(), id), m.end());
    m.push_back(id);
    workspace_.set_selection(m, id);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Apply all")) prefab_apply(id, {}, true);
  ImGui::SameLine();
  if (ImGui::SmallButton("Revert all")) prefab_revert(id, {}, true);
  ImGui::SameLine();
  if (ImGui::SmallButton("Unpack")) prefab_unpack(id);
  if (l.overrides.empty()) {
    ImGui::TextDisabled("No overrides on this entity.");
  } else {
    ImGui::TextDisabled("Overrides:");
    for (const std::string& k : l.overrides) {
      ImGui::PushID(k.c_str());
      ImGui::BulletText("%s", k.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Apply")) prefab_apply(id, k);
      ImGui::SameLine();
      if (ImGui::SmallButton("Revert")) prefab_revert(id, k);
      ImGui::PopID();
    }
  }
  ImGui::Separator();
  ImGui::PopID();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
