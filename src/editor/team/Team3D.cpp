// Live Team Claims: the 3D / VR editor binding (Scene3D <-> team doc).
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/team/TeamApply.h"
#include "editor/team/TeamSession.h"
#include "render3d/Scene3D.h"
#include "terrain/TerrainView.h"

#include <cstring>
#include <set>

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

// ---- Terrains (docs/team-claims.md, 3.6) ----

team::TerrainState terrain_state(const terrain::TerrainData& d) {
  team::TerrainState t;
  t.res = d.res;
  t.size = d.size;
  t.heights.resize(d.heights.size() * 4);
  if (!d.heights.empty()) std::memcpy(t.heights.data(), d.heights.data(), t.heights.size());
  t.splat.assign(d.splat.begin(), d.splat.end());
  t.foliage = json::Value::array();
  for (const terrain::FoliageInstance& f : d.foliage) {
    json::Value a = json::Value::array();
    a.push(json::Value::number(f.type));
    a.push(json::Value::number(f.x));
    a.push(json::Value::number(f.y));
    a.push(json::Value::number(f.z));
    a.push(json::Value::number(f.yaw));
    a.push(json::Value::number(f.scale));
    t.foliage.push(std::move(a));
  }
  return t;
}

// Write a patch into the editor's terrain data (and tell the caches).
bool write_terrain_patch(terrain::TerrainData& d, const json::Value& patch) {
  team::TerrainState t = terrain_state(d);
  const int old_res = t.res;
  if (!team::apply_terrain_patch(t, patch) || !t.valid()) return false;
  team::TerrainRect r;
  bool h = false, sp = false, f = false;
  team::terrain_patch_rect(patch, &r, &h, &sp, &f);
  d.res = t.res;
  d.size = t.size;
  d.heights.resize(static_cast<std::size_t>(t.res) * t.res);
  std::memcpy(d.heights.data(), t.heights.data(), t.heights.size());
  d.splat.assign(t.splat.begin(), t.splat.end());
  if (f) {
    d.foliage.clear();
    for (const json::Value& a : t.foliage.items()) {
      if (a.size() < 6) continue;
      terrain::FoliageInstance fi;
      fi.type = static_cast<std::uint16_t>(a[std::size_t{0}].as_number(0));
      fi.x = static_cast<float>(a[std::size_t{1}].as_number(0));
      fi.y = static_cast<float>(a[std::size_t{2}].as_number(0));
      fi.z = static_cast<float>(a[std::size_t{3}].as_number(0));
      fi.yaw = static_cast<float>(a[std::size_t{4}].as_number(0));
      fi.scale = static_cast<float>(a[std::size_t{5}].as_number(1));
      d.foliage.push_back(fi);
    }
  }
  if (old_res != t.res) {
    d.edits.clear();
    terrain::note_edit(d, terrain::full_rect(d), true, true, true);
  } else {
    terrain::Rect tr{r.i0, r.j0, r.i1, r.j1};
    if (!(h || sp)) tr = terrain::Rect{};
    terrain::note_edit(d, tr, h, sp, f);
  }
  return true;
}

// Bounding rect of the cells that differ between two same-sized grids
// (4 bytes per cell).
bool changed_rect(const std::string& a, const std::string& b, int res, team::TerrainRect* r) {
  bool any = false;
  for (int j = 0; j < res; ++j) {
    const std::size_t row = static_cast<std::size_t>(j) * res * 4;
    if (std::memcmp(a.data() + row, b.data() + row, static_cast<std::size_t>(res) * 4) == 0) continue;
    for (int i = 0; i < res; ++i) {
      if (std::memcmp(a.data() + row + i * 4, b.data() + row + i * 4, 4) == 0) continue;
      if (!any) *r = team::TerrainRect{i, j, i, j};
      any = true;
      r->i0 = std::min(r->i0, i);
      r->i1 = std::max(r->i1, i);
      r->j0 = std::min(r->j0, j);
      r->j1 = std::max(r->j1, j);
    }
  }
  return any;
}

}  // namespace

class Team3DBinding final : public TeamBinding {
 public:
  explicit Team3DBinding(Editor3DScreen& s) : s_(s) { reshadow(); }

  const char* space() const override { return "3d"; }

  bool busy() const override {
    return s_.modal_.before.has_value() || s_.inspector_before_.has_value() ||
           s_.stroke_.has_value() || s_.terrain_edit_before_.has_value() || s_.playing();
  }

  team::SceneDoc capture() const override {
    team::SceneDoc d;
    for (const scene3d::Entity3D& e : s_.scene_.entities) d.entities.emplace_back(e.id, entity_json(e));
    d.props = props_of(s_.scene_);
    for (const scene3d::Entity3D& e : s_.scene_.entities) {
      if (!e.terrain || e.terrain->data.empty() || d.terrains.count(e.terrain->data)) continue;
      std::shared_ptr<terrain::TerrainData> td = terrain::acquire(s_.project_.path, *e.terrain);
      if (td && td->valid()) d.terrains[e.terrain->data] = terrain_state(*td);
    }
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
    tshadow_.clear();
    trev_.clear();
    for (const auto& [path, t] : doc.terrains) write_terrain(path, 0, team::terrain_full(t));
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
    team_apply::drop_previewed(ops, previewed_);
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
    take_terrain_ops(ops);
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
      if (s_.modal_.before) fn(*s_.modal_.before);
      if (s_.inspector_before_) fn(*s_.inspector_before_);
      if (s_.terrain_edit_before_) fn(*s_.terrain_edit_before_);
    };
    for (const team::Op& op : ops) {
      if (op.kind == team::Op::Kind::Asset) continue;  // the Vault fetch (later)
      if (op.kind == team::Op::Kind::Terrain) {
        if (op.v.is_null()) {
          tshadow_.erase(op.key);
          trev_.erase(op.key);
        } else {
          write_terrain(op.key, op.id, op.v);
        }
        continue;
      }
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
      if (op.kind == team::Op::Kind::Put || op.kind == team::Op::Kind::Del) previewed_.erase(op.id);
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

  std::vector<team::Op> preview_ops() const override {
    return team_apply::preview_puts(shadow_, s_.scene_.entities, entity_json, previewed_);
  }
  void apply_preview(const std::vector<team::Op>& puts) override {
    team_apply::apply_preview(s_.scene_.entities, puts, entity_from, previewed_);
  }
  void end_preview(const std::vector<std::uint64_t>& ids) override {
    team_apply::end_preview(s_.scene_.entities, shadow_, ids, previewed_);
  }

 private:
  const scene3d::Entity3D* terrain_owner(const std::string& path, std::uint64_t id) const {
    if (const scene3d::Entity3D* e = s_.scene_.find(id); e && e->terrain && e->terrain->data == path) return e;
    for (const scene3d::Entity3D& e : s_.scene_.entities) {
      if (e.terrain && e.terrain->data == path) return &e;
    }
    return nullptr;
  }

  // A remote (or welcome) terrain patch into the shared data.
  void write_terrain(const std::string& path, std::uint64_t id, const json::Value& patch) {
    const scene3d::Entity3D* e = terrain_owner(path, id);
    if (!e) {
      // No entity for it (yet): keep it so the shadow knows the doc has it.
      team::TerrainState t = tshadow_.count(path) ? tshadow_[path] : team::TerrainState{};
      if (team::apply_terrain_patch(t, patch)) tshadow_[path] = std::move(t);
      return;
    }
    std::shared_ptr<terrain::TerrainData> d = terrain::acquire(s_.project_.path, *e->terrain);
    if (!d) return;
    if (!d->valid()) terrain::reset(*d, std::max(e->terrain->resolution, 2), e->terrain->size);
    if (!write_terrain_patch(*d, patch)) return;
    tshadow_[path] = terrain_state(*d);
    trev_[path] = d->revision;
  }

  // Local terrain edits since the last send: the changed rect of heights
  // and splat, plus the foliage list when it changed.
  void take_terrain_ops(std::vector<team::Op>& ops) {
    std::set<std::string> seen;
    for (const scene3d::Entity3D& e : s_.scene_.entities) {
      if (!e.terrain || e.terrain->data.empty() || !seen.insert(e.terrain->data).second) continue;
      const std::string& path = e.terrain->data;
      std::shared_ptr<terrain::TerrainData> d = terrain::find_loaded(s_.project_.path, *e.terrain);
      if (!d || !d->valid()) continue;
      auto sh = tshadow_.find(path);
      auto rv = trev_.find(path);
      if (sh != tshadow_.end() && rv != trev_.end() && rv->second == d->revision) continue;
      team::TerrainState now = terrain_state(*d);
      trev_[path] = d->revision;
      if (sh == tshadow_.end() || sh->second.res != now.res || sh->second.size != now.size ||
          !sh->second.valid()) {
        ops.push_back(team::Op::terrain(path, e.id, team::terrain_full(now)));
        tshadow_[path] = std::move(now);
        continue;
      }
      team::TerrainRect rh, rs;
      const bool h = changed_rect(sh->second.heights, now.heights, now.res, &rh);
      const bool sp = changed_rect(sh->second.splat, now.splat, now.res, &rs);
      const bool f = !team::same_json(sh->second.foliage, now.foliage);
      if (!h && !sp && !f) continue;
      team::TerrainRect r = h ? rh : rs;
      if (h && sp) {
        r.i0 = std::min(rh.i0, rs.i0);
        r.j0 = std::min(rh.j0, rs.j0);
        r.i1 = std::max(rh.i1, rs.i1);
        r.j1 = std::max(rh.j1, rs.j1);
      }
      ops.push_back(team::Op::terrain(path, e.id, team::terrain_patch(now, r, h, sp, f)));
      sh->second = std::move(now);
    }
    // Terrains no entity uses any more leave the doc.
    for (auto it = tshadow_.begin(); it != tshadow_.end();) {
      if (!seen.count(it->first) && !terrain_owner(it->first, 0)) {
        ops.push_back(team::Op::terrain(it->first, 0, json::Value::null()));
        trev_.erase(it->first);
        it = tshadow_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void reshadow() {
    shadow_ = s_.scene_.entities;
    shadow_env_ = s_.scene_.environment;
    shadow_physics_ = s_.scene_.physics;
  }

  Editor3DScreen& s_;
  std::vector<scene3d::Entity3D> shadow_;
  std::set<std::uint64_t> previewed_;  // under someone else's drag
  std::map<std::string, team::TerrainState> tshadow_;  // terrain data as last sent / applied
  std::map<std::string, std::uint64_t> trev_;          // its TerrainData revision then
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
  // Files the posse sent (or removed): models / textures reload on the next
  // draw and the Asset Browser shows them.
  if (!team_->take_arrived().empty()) {
    if (view_) view_->clear();
    asset_browser_.rescan();
    rescan_assets();
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
