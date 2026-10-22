// Live Team Claims: the 3D / VR editor binding (Scene3D <-> team doc).
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/team/TeamApply.h"
#include "editor/team/TeamSession.h"
#include "render3d/Scene3D.h"

namespace ts {
namespace tombstone {
namespace editor {

namespace {

constexpr const char* kProps[] = {"environment", "physics"};

json::Value entity_json(const scene3d::Entity3D& e) { return scene3d::entity_to_json(e); }
bool entity_from(const json::Value& v, scene3d::Entity3D* out) {
  std::string err;
  return scene3d::entity_from_json(v, out, &err);
}

std::map<std::string, json::Value> props_of(const scene3d::Scene3D& s) {
  scene3d::Scene3D bare;
  bare.environment = s.environment;
  bare.physics = s.physics;
  json::Value root;
  std::string err;
  std::map<std::string, json::Value> out;
  if (!json::parse(scene3d::write(bare), &root, &err)) return out;
  for (const char* k : kProps) {
    if (root.has(k)) out[k] = root[k];
  }
  return out;
}

// Environment + physics from props into `s` (entities untouched).
bool props_into(const std::map<std::string, json::Value>& props, scene3d::Scene3D* s) {
  json::Value root;
  std::string err;
  if (!json::parse(scene3d::write(scene3d::Scene3D{}), &root, &err)) return false;
  json::Value out = json::Value::object();
  for (const auto& [k, v] : root.members()) {
    if (k != "environment" && k != "physics") out.set(k, v);
  }
  for (const char* k : kProps) {
    auto it = props.find(k);
    if (it != props.end()) out.set(k, it->second);
  }
  scene3d::Scene3D parsed;
  if (!scene3d::parse(json::write(out), &parsed, &err)) return false;
  s->environment = parsed.environment;
  s->physics = parsed.physics;
  return true;
}

}  // namespace

class Team3DBinding final : public TeamBinding {
 public:
  explicit Team3DBinding(Editor3DScreen& s) : s_(s) { reshadow(); }

  const char* space() const override { return "3d"; }

  bool busy() const override {
    return s_.gizmo_before_.has_value() || s_.inspector_before_.has_value() ||
           s_.stroke_.has_value() || s_.terrain_edit_before_.has_value() || s_.playing();
  }

  team::SceneDoc capture() const override {
    team::SceneDoc d;
    for (const scene3d::Entity3D& e : s_.scene_.entities) d.entities.emplace_back(e.id, entity_json(e));
    d.props = props_of(s_.scene_);
    return d;
  }

  void load(const team::SceneDoc& doc) override {
    scene3d::Scene3D& sc = s_.scene_;
    sc.entities.clear();
    for (const auto& [id, v] : doc.entities) {
      scene3d::Entity3D e;
      if (entity_from(v, &e)) {
        e.id = id;
        sc.entities.push_back(std::move(e));
      }
    }
    props_into(doc.props, &sc);
    sc.selection.clear();
    sc.id_lo = lo_;
    sc.id_hi = hi_;
    s_.undo_.clear();
    s_.redo_.clear();
    reshadow();
    s_.autosave();
  }

  std::vector<team::Op> take_local_ops(std::string* label) override {
    scene3d::Scene3D& sc = s_.scene_;
    sc.id_lo = lo_;  // undo can bring back a scene from before the grant
    sc.id_hi = hi_;
    std::vector<team::Op> ops = team_apply::diff_entities(shadow_, sc.entities, entity_json);
    if (!(sc.environment == shadow_env_) || !(sc.physics == shadow_physics_)) {
      scene3d::Scene3D old;
      old.environment = shadow_env_;
      old.physics = shadow_physics_;
      const auto before = props_of(old);
      const auto now = props_of(sc);
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
      if (label) *label = s_.undo_label();
    }
    return ops;
  }

  void apply(const std::vector<team::Op>& ops) override {
    scene3d::Scene3D& sc = s_.scene_;
    auto each_state = [&](auto&& fn) {
      fn(sc);
      for (auto& st : s_.undo_) fn(st.scene);
      for (auto& st : s_.redo_) fn(st.scene);
      if (s_.gizmo_before_) fn(*s_.gizmo_before_);
      if (s_.inspector_before_) fn(*s_.inspector_before_);
      if (s_.terrain_edit_before_) fn(*s_.terrain_edit_before_);
    };
    for (const team::Op& op : ops) {
      if (op.kind == team::Op::Kind::Asset) continue;  // the Vault fetch (later)
      if (op.kind == team::Op::Kind::Set) {
        std::map<std::string, json::Value> props = props_of(sc);
        if (op.v.is_null()) props.erase(op.key);
        else props[op.key] = op.v;
        scene3d::Scene3D tmp;
        if (props_into(props, &tmp)) {
          each_state([&](scene3d::Scene3D& st) {
            st.environment = tmp.environment;
            st.physics = tmp.physics;
          });
        }
        continue;
      }
      each_state([&](scene3d::Scene3D& st) {
        team_apply::apply_entity_op(st.entities, op, entity_from);
      });
    }
    auto& sel = sc.selection;
    sel.erase(std::remove_if(sel.begin(), sel.end(), [&](std::uint64_t id) { return !sc.find(id); }),
              sel.end());
    reshadow();
    s_.autosave();
  }

  void set_id_range(std::uint64_t lo, std::uint64_t hi) override {
    lo_ = lo;
    hi_ = hi;
    s_.scene_.id_lo = lo;
    s_.scene_.id_hi = hi;
  }

  std::vector<std::uint64_t> selection() const override { return s_.scene_.selection; }

 private:
  void reshadow() {
    shadow_ = s_.scene_.entities;
    shadow_env_ = s_.scene_.environment;
    shadow_physics_ = s_.scene_.physics;
  }

  Editor3DScreen& s_;
  std::vector<scene3d::Entity3D> shadow_;
  render3d::Environment shadow_env_;
  scene3d::PhysicsSettings3D shadow_physics_;
  std::uint64_t lo_ = 0, hi_ = 0;
};

std::unique_ptr<TeamBinding> make_team_binding(Editor3DScreen& screen) {
  return std::make_unique<Team3DBinding>(screen);
}

TeamSession& Editor3DScreen::team() {
  if (!team_) team_ = std::make_unique<TeamSession>(make_team_binding(*this), project_);
  return *team_;
}

void Editor3DScreen::tick_team(float dt) {
  if (!team_) return;
  team_->tick(dt);
  for (const std::string& n : team_->take_notes()) note("Team: " + n);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
