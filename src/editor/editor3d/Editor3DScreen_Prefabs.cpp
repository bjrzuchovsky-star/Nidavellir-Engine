// Prefabs in the 3D editor: save a selection as a .prefab.json, place
// instances, per-instance overrides, apply / revert, asset edits that
// update every instance. The algorithms live in scene/Prefab.h; this file
// converts the scene to / from entity JSON and wraps each change in one
// undo step.
#include "editor/editor3d/Editor3DScreen.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <set>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;
using prefab::Dim;
using render3d::Vec3;
using runtime::LogLevel;

namespace {
bool any_instance(const scene3d::Scene3D& s) {
  return std::any_of(s.entities.begin(), s.entities.end(),
                     [](const scene3d::Entity3D& e) { return e.prefab.has_value(); });
}
}  // namespace

bool Editor3DScreen::refresh_prefab_overrides() {
  if (!any_instance(scene_)) return false;
  std::vector<json::Value> js = scene3d::entities_to_json(scene_.entities);
  if (!prefab::refresh_overrides(Dim::D3, js, prefabs_.lookup())) return false;
  // Only the links change (no float round trip of the rest).
  for (std::size_t i = 0; i < js.size() && i < scene_.entities.size(); ++i) {
    prefab::Link l;
    if (prefab::json_link(js[i], &l)) scene_.entities[i].prefab = l;
  }
  return true;
}

bool Editor3DScreen::prefab_set_entities(const std::vector<json::Value>& js,
                                         const std::string& label,
                                         const scene3d::Scene3D& before) {
  std::vector<scene3d::Entity3D> ents;
  std::string err;
  if (!scene3d::entities_from_json(js, &ents, &err)) {
    note("Prefab: " + err, LogLevel::Error);
    return false;
  }
  scene_.entities = std::move(ents);
  std::vector<std::uint64_t> sel;
  for (std::uint64_t id : scene_.selection) {
    if (scene_.find(id)) sel.push_back(id);
  }
  scene_.selection = std::move(sel);
  if (view_) view_->clear();
  push_undo(label, before);
  return true;
}

void Editor3DScreen::prefab_sync_all_on_load() {
  if (!any_instance(scene_)) return;
  std::set<std::string> rels;
  for (const auto& e : scene_.entities) {
    if (e.prefab) rels.insert(e.prefab->asset);
  }
  std::vector<json::Value> js = scene3d::entities_to_json(scene_.entities);
  std::uint64_t next = scene_.next_id();
  std::size_t changed = 0;
  for (const std::string& rel : rels) {
    if (const prefab::Asset* a = prefabs_.get(rel)) {
      changed += prefab::sync_instances(Dim::D3, js, rel, *a, &next);
    } else {
      note("Prefab " + rel + " is missing; its instances keep their last values",
           LogLevel::Warn);
    }
  }
  if (changed == 0) return;
  std::vector<scene3d::Entity3D> ents;
  std::string err;
  if (scene3d::entities_from_json(js, &ents, &err)) {
    scene_.entities = std::move(ents);
    note("Prefabs: " + std::to_string(changed) + " instance entit(ies) caught up with their assets");
    save_scene(&err);
  }
}

std::string Editor3DScreen::save_selection_as_prefab(const std::string& name,
                                                     std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    note("Save as Prefab: " + why, LogLevel::Warn);
    return std::string();
  };
  if (playing()) return fail("not while playing");
  const std::vector<std::uint64_t> roots = selection_roots();
  if (roots.empty()) return fail("select something first");
  std::set<std::uint64_t> ids(roots.begin(), roots.end());
  for (std::uint64_t r : roots) {
    for (std::uint64_t d : scene3d::descendants_of(scene_.entities, r)) ids.insert(d);
  }
  std::vector<json::Value> all = scene3d::entities_to_json(scene_.entities);
  std::vector<json::Value> picked;
  for (const json::Value& v : all) {
    if (ids.count(prefab::json_id(v))) picked.push_back(v);
  }
  // Roots first so the anchor is the first selected root.
  std::stable_partition(picked.begin(), picked.end(), [&](const json::Value& v) {
    return prefab::json_id(v) == roots.front();
  });
  std::map<std::uint64_t, std::uint64_t> local;
  const std::string display = name.empty() ? "Prefab" : name;
  prefab::Asset asset = prefab::make_asset(Dim::D3, display, picked, &local);
  std::string rel = prefab::default_asset_rel(display);
  for (int n = 2; fs::exists(fs::u8path(prefabs_.abs_path(rel))); ++n) {
    rel = prefab::default_asset_rel(display + " " + std::to_string(n));
  }
  const std::string label = "Make Prefab " + display;
  std::string err;
  if (!prefabs_.save(rel, asset, &err, undo_.size() + 1, label)) return fail(err);
  const scene3d::Scene3D before = scene_;
  const std::uint64_t inst =
      prefab::max_instance(scene3d::entities_to_json(scene_.entities)) + 1;
  for (auto& e : scene_.entities) {
    const auto it = local.find(e.id);
    if (it == local.end()) continue;
    prefab::Link l;
    l.asset = rel;
    l.instance = inst;
    l.source = it->second;
    e.prefab = l;
  }
  push_undo(label, before);
  note("Saved " + std::to_string(ids.size()) + " entit(ies) as " + rel);
  return rel;
}

std::uint64_t Editor3DScreen::place_prefab(const std::string& rel, const render3d::Vec3& pos) {
  if (playing()) return 0;
  const prefab::Asset* asset = prefabs_.get(rel);
  if (!asset) {
    note("Prefab " + rel + " does not load", LogLevel::Error);
    return 0;
  }
  if (asset->dim != Dim::D3) {
    note(rel + " is a 2D prefab", LogLevel::Warn);
    return 0;
  }
  const scene3d::Scene3D before = scene_;
  std::vector<json::Value> js = scene3d::entities_to_json(scene_.entities);
  std::uint64_t next = scene_.next_id();
  const std::uint64_t first = next;
  const double p[3] = {pos.x, pos.y, pos.z};
  for (json::Value& v : prefab::instantiate(*asset, rel, p, prefab::max_instance(js) + 1, &next)) {
    js.push_back(std::move(v));
  }
  if (!prefab_set_entities(js, "Place " + asset->name, before)) return 0;
  scene_.selection = {first};
  autosave();
  note("Placed " + asset->name);
  return first;
}

std::vector<std::uint64_t> Editor3DScreen::prefab_instance_members(std::uint64_t id) const {
  std::vector<std::uint64_t> out;
  const scene3d::Entity3D* e = find(id);
  if (!e || !e->prefab) return out;
  for (const auto& o : scene_.entities) {
    if (o.prefab && o.prefab->asset == e->prefab->asset &&
        o.prefab->instance == e->prefab->instance) {
      out.push_back(o.id);
    }
  }
  return out;
}

bool Editor3DScreen::prefab_apply(std::uint64_t id, const std::string& key, bool whole) {
  const scene3d::Entity3D* e = find(id);
  if (playing() || !e || !e->prefab) return false;
  refresh_prefab_overrides();
  const std::string rel = e->prefab->asset;
  const prefab::Asset* cur = prefabs_.get(rel);
  if (!cur) return false;
  prefab::Asset asset = *cur;
  const scene3d::Scene3D before = scene_;
  std::vector<json::Value> js = scene3d::entities_to_json(scene_.entities);
  if (!prefab::apply_overrides(Dim::D3, js, id, key, whole, &asset)) {
    note("Nothing to apply");
    return false;
  }
  std::uint64_t next = scene_.next_id();
  prefab::sync_instances(Dim::D3, js, rel, asset, &next);
  const std::string label = "Apply to Prefab " + asset.name;
  std::string err;
  if (!prefabs_.save(rel, asset, &err, undo_.size() + 1, label)) {
    note("Apply: " + err, LogLevel::Error);
    return false;
  }
  if (!prefab_set_entities(js, label, before)) return false;
  note("Applied to " + rel + "; every instance updated");
  return true;
}

bool Editor3DScreen::prefab_revert(std::uint64_t id, const std::string& key, bool whole) {
  const scene3d::Entity3D* e = find(id);
  if (playing() || !e || !e->prefab) return false;
  refresh_prefab_overrides();
  const prefab::Asset* asset = prefabs_.get(e->prefab->asset);
  if (!asset) return false;
  const scene3d::Scene3D before = scene_;
  std::vector<json::Value> js = scene3d::entities_to_json(scene_.entities);
  std::uint64_t next = scene_.next_id();
  if (!prefab::revert_overrides(Dim::D3, js, id, key, whole, *asset, &next)) return false;
  return prefab_set_entities(js, key.empty() ? "Revert Overrides" : "Revert " + key, before);
}

bool Editor3DScreen::prefab_unpack(std::uint64_t id) {
  const scene3d::Entity3D* e = find(id);
  if (playing() || !e || !e->prefab) return false;
  const scene3d::Scene3D before = scene_;
  const std::vector<std::uint64_t> members = prefab_instance_members(id);
  for (auto& o : scene_.entities) {
    if (std::find(members.begin(), members.end(), o.id) != members.end()) o.prefab.reset();
  }
  push_undo("Unpack Prefab", before);
  return true;
}

bool Editor3DScreen::prefab_edit_asset(const std::string& rel, const prefab::Asset& asset) {
  if (playing()) return false;
  const std::string label = "Edit Prefab " + asset.name;
  refresh_prefab_overrides();
  std::string err;
  if (!prefabs_.save(rel, asset, &err, undo_.size() + 1, label)) {
    note("Edit Prefab: " + err, LogLevel::Error);
    return false;
  }
  const scene3d::Scene3D before = scene_;
  std::vector<json::Value> js = scene3d::entities_to_json(scene_.entities);
  std::uint64_t next = scene_.next_id();
  prefab::sync_instances(Dim::D3, js, rel, asset, &next);
  return prefab_set_entities(js, label, before);
}

std::size_t Editor3DScreen::prefab_poll() {
  if (playing()) return 0;
  const std::vector<std::string> changed = prefabs_.poll_changes();
  if (changed.empty()) return 0;
  const scene3d::Scene3D before = scene_;
  std::vector<json::Value> js = scene3d::entities_to_json(scene_.entities);
  std::uint64_t next = scene_.next_id();
  std::size_t n = 0;
  for (const std::string& rel : changed) {
    if (const prefab::Asset* a = prefabs_.get(rel)) {
      n += prefab::sync_instances(Dim::D3, js, rel, *a, &next);
    }
  }
  if (n > 0 && prefab_set_entities(js, "Prefab changed on disk", before)) {
    note("Prefab file changed: " + std::to_string(n) + " instance entit(ies) updated");
  }
  return n;
}

// ---- UI -----------------------------------------------------------------------

void Editor3DScreen::draw_prefabs_panel() {
  ImGui::TextDisabled("Save the selection, then place copies that follow it.");
  ImGui::SetNextItemWidth(-110);
  ImGui::InputText("##prefabname", prefab_name_, sizeof(prefab_name_));
  ImGui::SameLine();
  ImGui::BeginDisabled(scene_.selection.empty());
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
    if (!a || a->dim != Dim::D3) continue;
    ImGui::PushID(rel.c_str());
    std::size_t count = 0;
    std::set<std::uint64_t> inst;
    for (const auto& e : scene_.entities) {
      if (e.prefab && e.prefab->asset == rel) inst.insert(e.prefab->instance);
    }
    count = inst.size();
    if (ImGui::SmallButton("Place")) {
      const Vec3 at = scene_.editor_camera.target;
      place_prefab(rel, {at.x, 0.0f, at.z});
    }
    ImGui::SameLine();
    ImGui::Text("%s", a->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu ent, %zu placed)  %s", a->entities.size(), count, rel.c_str());
    if (ImGui::IsItemHovered() && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      const std::string payload = "prefab:" + rel;
      ImGui::SetDragDropPayload("TS_ASSET3D", payload.c_str(), payload.size() + 1);
      ImGui::Text("%s", a->name.c_str());
      ImGui::EndDragDropSource();
    }
    ImGui::PopID();
  }
}

void Editor3DScreen::draw_prefab_inspector(const scene3d::Entity3D& e) {
  const prefab::Link& l = *e.prefab;
  const std::uint64_t id = e.id;
  ImGui::PushID("prefab");
  ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "Prefab: %s",
                     prefab::name_from_rel(l.asset).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("#%llu", static_cast<unsigned long long>(l.instance));
  if (!prefabs_.get(l.asset)) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "asset missing: %s", l.asset.c_str());
  if (ImGui::SmallButton("Select instance")) {
    const auto m = prefab_instance_members(id);
    scene_.selection = m;
    if (std::find(m.begin(), m.end(), id) != m.end()) {
      scene_.selection.erase(std::find(scene_.selection.begin(), scene_.selection.end(), id));
      scene_.selection.push_back(id);
    }
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
    const std::vector<std::string> keys = l.overrides;
    for (const std::string& k : keys) {
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
