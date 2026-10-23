// Live Team Claims: the 2D editor binding (Workspace2D <-> team doc).
#include "editor/screens/Editor2DScreen.h"
#include "editor/team/TeamApply.h"
#include "editor/team/TeamSession.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <set>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

constexpr const char* kProps[] = {"tile_solidity", "lighting"};

json::Value entity_json(const Entity2D& e) { return scene_json::entity_to_json(e); }
bool entity_from(const json::Value& v, Entity2D* out) {
  return scene_json::entity_from_json(v, out, nullptr);
}

// Scene-level blocks as scene.json writes them.
std::map<std::string, json::Value> props_of(const TileSolidity& solid, const fx::Lighting2D& light) {
  scene_json::SceneDoc doc;
  doc.tile_solidity = solid;
  doc.lighting = light;
  json::Value root;
  std::string err;
  std::map<std::string, json::Value> out;
  if (!json::parse(scene_json::write(doc), &root, &err)) return out;
  for (const char* k : kProps) {
    if (root.has(k)) out[k] = root[k];
  }
  return out;
}

bool props_into(const std::map<std::string, json::Value>& props, TileSolidity* solid,
                fx::Lighting2D* light) {
  json::Value root;
  std::string err;
  if (!json::parse(scene_json::write(scene_json::SceneDoc{}), &root, &err)) return false;
  json::Value out = json::Value::object();
  for (const auto& [k, v] : root.members()) {
    if (k != "tile_solidity" && k != "lighting") out.set(k, v);
  }
  for (const char* k : kProps) {
    auto it = props.find(k);
    if (it != props.end()) out.set(k, it->second);
  }
  scene_json::SceneDoc doc;
  if (!scene_json::parse(json::write(out), &doc, &err, "team")) return false;
  *solid = std::move(doc.tile_solidity);
  *light = doc.lighting;
  return true;
}

}  // namespace

class Team2DBinding final : public TeamBinding {
 public:
  explicit Team2DBinding(Editor2DScreen& s) : s_(s) { reshadow(); }

  const char* space() const override { return "2d"; }

  bool busy() const override {
    const Workspace2D& w = s_.workspace();
    return w.edit_open() || w.move_active() || w.transform_active() || w.xform_running() ||
           s_.is_playing() ||
           s_.stroke_active();
  }

  team::SceneDoc capture() const override {
    const Workspace2D& w = s_.workspace();
    team::SceneDoc d;
    for (const Entity2D& e : w.entities()) d.entities.emplace_back(e.id, entity_json(e));
    d.props = props_of(w.tile_solidity(), w.lighting());
    return d;
  }

  void load(const team::SceneDoc& doc) override {
    Workspace2D& w = s_.workspace();
    std::vector<Entity2D> ents;
    for (const auto& [id, v] : doc.entities) {
      Entity2D e;
      if (entity_from(v, &e)) {
        e.id = id;
        ents.push_back(std::move(e));
      }
    }
    w.clear_selection();
    w.replace_entities(std::move(ents));
    TileSolidity solid;
    fx::Lighting2D light;
    if (props_into(doc.props, &solid, &light)) {
      w.set_tile_solidity(std::move(solid));
      w.set_lighting(light);
    }
    w.clear_history();
    reshadow();
    s_.mark_dirty_and_autosave();
  }

  std::vector<team::Op> take_local_ops(std::string* label) override {
    const Workspace2D& w = s_.workspace();
    std::vector<team::Op> ops = team_apply::diff_entities(shadow_, w.entities(), entity_json);
    team_apply::drop_previewed(ops, previewed_);
    if (!(w.tile_solidity() == shadow_solid_) || !(w.lighting() == shadow_light_)) {
      const auto before = props_of(shadow_solid_, shadow_light_);
      const auto now = props_of(w.tile_solidity(), w.lighting());
      for (const char* k : kProps) {
        auto a = before.find(k), b = now.find(k);
        const bool ha = a != before.end(), hb = b != now.end();
        if (ha != hb || (ha && !team::same_json(a->second, b->second))) {
          ops.push_back(team::Op::set(k, hb ? b->second : json::Value::null()));
        }
      }
    }
    if (!ops.empty()) {
      reshadow();
      if (label) *label = w.undo_label();
    }
    return ops;
  }

  void apply(const std::vector<team::Op>& ops) override {
    Workspace2D& w = s_.workspace();
    bool props_changed = false;
    std::map<std::string, json::Value> props = props_of(w.tile_solidity(), w.lighting());
    for (const team::Op& op : ops) {
      if (op.kind == team::Op::Kind::Set) {
        if (op.v.is_null()) props.erase(op.key);
        else props[op.key] = op.v;
        props_changed = true;
        continue;
      }
      if (op.kind == team::Op::Kind::Asset) continue;  // the Vault fetch (later)
      if (op.kind == team::Op::Kind::Put || op.kind == team::Op::Kind::Del) previewed_.erase(op.id);
      team_apply::apply_entity_op(w.entities(), op, entity_from);
      w.for_each_history_state([&](std::vector<Entity2D>& ents, TileSolidity&) {
        team_apply::apply_entity_op(ents, op, entity_from);
      });
    }
    if (props_changed) {
      TileSolidity solid;
      fx::Lighting2D light;
      if (props_into(props, &solid, &light)) {
        w.for_each_history_state(
            [&](std::vector<Entity2D>&, TileSolidity& ts) { ts = solid; });
        w.set_tile_solidity(std::move(solid));
        w.set_lighting(light);
      }
    }
    // Drop selected ids that are gone.
    std::vector<std::uint64_t> sel;
    for (std::uint64_t id : w.selection()) {
      if (w.find(id)) sel.push_back(id);
    }
    if (sel.size() != w.selection().size()) {
      std::optional<std::uint64_t> primary = w.selected_id();
      if (primary && !w.find(*primary)) primary.reset();
      w.set_selection(sel, primary);
    }
    reshadow();
    s_.mark_dirty_and_autosave();
  }

  void set_id_range(std::uint64_t lo, std::uint64_t hi) override {
    s_.workspace().set_id_range(lo, hi);
  }

  std::vector<std::uint64_t> selection() const override { return s_.workspace().selection(); }

  std::vector<team::Op> preview_ops() const override {
    return team_apply::preview_puts(shadow_, s_.workspace().entities(), entity_json, previewed_);
  }
  void apply_preview(const std::vector<team::Op>& puts) override {
    team_apply::apply_preview(s_.workspace().entities(), puts, entity_from, previewed_);
  }
  void end_preview(const std::vector<std::uint64_t>& ids) override {
    team_apply::end_preview(s_.workspace().entities(), shadow_, ids, previewed_);
  }

 private:
  void reshadow() {
    const Workspace2D& w = s_.workspace();
    shadow_ = w.entities();
    shadow_solid_ = w.tile_solidity();
    shadow_light_ = w.lighting();
  }

  Editor2DScreen& s_;
  std::vector<Entity2D> shadow_;
  std::set<std::uint64_t> previewed_;  // under someone else's drag
  TileSolidity shadow_solid_;
  fx::Lighting2D shadow_light_;
};

std::unique_ptr<TeamBinding> make_team_binding(Editor2DScreen& screen) {
  return std::make_unique<Team2DBinding>(screen);
}

TeamSession& Editor2DScreen::team() {
  if (!team_) team_ = std::make_unique<TeamSession>(make_team_binding(*this), project_);
  return *team_;
}

void Editor2DScreen::tick_team(float dt) {
  if (!team_) return;
  team_->tick(dt);
  for (const std::string& n : team_->take_notes()) {
    note(n);
    telegraph_.add(runtime::LogLevel::Info, "team", n);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
