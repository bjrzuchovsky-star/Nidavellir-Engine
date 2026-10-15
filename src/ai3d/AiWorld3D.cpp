#include "ai3d/AiWorld3D.h"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace ai3d {

using ai::V3;
using world3d::World3D;
using world3d::World3DExtension;

namespace {

V3 v3(const render3d::Vec3& v) { return {v.x, v.y, v.z}; }
render3d::Vec3 rv(const V3& v) { return {v.x, v.y, v.z}; }

std::map<const World3D*, AiWorld3D*>& registry() {
  static std::map<const World3D*, AiWorld3D*> r;
  return r;
}

}  // namespace

struct AiWorld3D::Agent : ai::AgentBody {
  AiWorld3D* owner = nullptr;
  std::uint64_t id = 0;
  scene3d::Agent3D cfg;
  int crowd = -1;
  std::string bt_path;
  std::unique_ptr<ai::AgentBrain> brain;
  V3 fwd{0, 0, 1};
  V3 goal;
  bool has_goal = false;

  V3 position() const override {
    const world3d::Entity3D* e = owner->world_->find(id);
    return e ? v3(e->transform.position) : V3{};
  }
  V3 forward() const override { return fwd; }
  bool plan(const V3& from, const V3& to, std::vector<V3>* points) override {
    const ai::NavPath p = owner->mesh_->find_path(from, to);
    if (!p.found || p.partial) return false;
    *points = p.points;
    return true;
  }
  bool random_point(const V3& around, float radius, float u, float v, V3* out) override {
    std::uint64_t s = static_cast<std::uint64_t>(u * 4294967295.0) * 2654435761ull ^
                      static_cast<std::uint64_t>(v * 4294967295.0) ^ 1;
    return owner->mesh_->random_point(around, radius, &s, out);
  }
  bool entity_pos(std::uint64_t e, V3* out) const override {
    const world3d::Entity3D* x = owner->world_->find(e);
    if (!x) return false;
    *out = v3(x->transform.position);
    return true;
  }
  std::uint64_t find(const std::string& name) const override {
    if (name == "player") return owner->world_->player_id();
    return owner->world_->script_find(name);
  }
  void desire(const V3& vel) override {
    // The crowd steers; the brain's path tells it where to (see on_tick).
    if (ai::length_xz(vel) < 1e-5f && crowd >= 0 && !brain->moving()) {
      if (has_goal) owner->crowd_->stop(crowd);
      has_goal = false;
    }
  }
  bool play(const std::string& clip, float fade) override { return owner->world_->anim_play(id, clip, fade, false); }
  bool attack(std::uint64_t target, double damage) override {
    const ScriptValue* hp = owner->world_->script_get(target, "hp");
    if (hp && hp->type == ScriptValue::Type::Number) {
      const ScriptValue nv = ScriptValue::of_number(hp->number - damage);
      owner->world_->script_set(target, "hp", &nv);
    }
    return true;
  }
  void log(const std::string& text) override {
    runtime::LogEntry e;
    e.channel = "ai";
    e.text = text;
    e.entity = id;
    owner->world_->script_log(e);
  }
};

AiWorld3D::AiWorld3D() : mesh_(std::make_unique<ai::NavMesh>()) {}

AiWorld3D::~AiWorld3D() {
  for (auto it = registry().begin(); it != registry().end();) {
    if (it->second == this) it = registry().erase(it);
    else ++it;
  }
}

AiWorld3D* AiWorld3D::of(const World3D& w) {
  auto it = registry().find(&w);
  return it == registry().end() ? nullptr : it->second;
}

AiWorld3D* AiWorld3D::latest() {
  AiWorld3D* best = nullptr;
  for (const auto& [w, a] : registry()) {
    if (w->running()) best = a;
  }
  return best;
}

std::shared_ptr<AiWorld3D> attach(World3D& w, bool enabled) {
  if (!enabled) return nullptr;
  if (auto* existing = dynamic_cast<AiWorld3D*>(w.extension(AiWorld3D::kName))) {
    (void)existing;
    return nullptr;
  }
  auto ext = std::make_shared<AiWorld3D>();
  w.add_extension(ext);
  return ext;
}

void AiWorld3D::rebuild() {
  ai::NavGeometry geo;
  std::vector<ai::OffMeshLink> links;
  float radius = 0.0f, height = 0.0f;
  for (const world3d::Entity3D& e : world_->entities()) {
    if (e.agent) {
      radius = std::max(radius, e.agent->radius);
      height = std::max(height, e.agent->height);
    }
    if (e.nav_link) {
      ai::OffMeshLink l;
      l.a = v3(e.transform.position);
      l.b = v3(e.nav_link->to);
      l.radius = e.nav_link->radius;
      l.bidirectional = e.nav_link->bidirectional;
      l.user_id = static_cast<std::uint32_t>(e.id);
      links.push_back(l);
    }
    if (!e.collider || e.collider->trigger || e.player || e.agent || e.grabbable || e.vr_rig || !e.visible) continue;
    const scene3d::Collider3D& c = *e.collider;
    render3d::Aabb local;
    if (c.shape == scene3d::Collider3D::Shape::Capsule) {
      const float r = c.size.x, h = std::max(c.size.y, 2.0f * c.size.x);
      local = {{-r, -h * 0.5f, -r}, {r, h * 0.5f, r}};
    } else {
      local = {c.size * -0.5f, c.size * 0.5f};
    }
    local.min += c.offset;
    local.max += c.offset;
    const render3d::Aabb b = local.transformed(e.transform.matrix());
    geo.add_box(v3((b.min + b.max) * 0.5f), v3((b.max - b.min) * 0.5f));
  }
  ai::NavBuildConfig cfg;
  if (radius > 0.0f) cfg.agent_radius = radius;
  if (height > 0.0f) cfg.agent_height = height;
  const auto t0 = std::chrono::steady_clock::now();
  build_error_.clear();
  mesh_->clear();
  if (geo.tri_count() == 0 || !mesh_->build(geo, cfg, links, &build_error_)) {
    if (build_error_.empty()) build_error_ = "no static colliders to walk on";
  }
  build_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  edges_.clear();
  if (mesh_->ready()) mesh_->debug_edges(&edges_);
}

void AiWorld3D::on_start(World3D& w) {
  world_ = &w;
  registry()[&w] = this;
  agents_.clear();
  crowd_.reset();
  if (!authority_) return;
  rebuild();
  if (!mesh_->ready()) {
    runtime::LogEntry e;
    e.level = runtime::LogLevel::Warn;
    e.channel = "ai";
    e.text = "navmesh: " + build_error_;
    w.script_log(e);
    return;
  }
  crowd_ = std::make_unique<ai::NavCrowd>(*mesh_, 128, 2.0f);
  for (const world3d::Entity3D& e : w.entities()) {
    if (!e.agent) continue;
    if (add_agent(e.id, *e.agent) && !e.agent->bt.empty()) {
      std::string err;
      if (!load_tree(e.id, e.agent->bt, &err)) {
        runtime::LogEntry le;
        le.level = runtime::LogLevel::Warn;
        le.channel = "ai";
        le.text = err;
        le.entity = e.id;
        w.script_log(le);
      }
    }
  }
  runtime::LogEntry e;
  e.channel = "ai";
  e.text = "navmesh baked: " + std::to_string(mesh_->stats().polys) + " polys, " +
           std::to_string(mesh_->stats().offmesh) + " links, " + std::to_string(agents_.size()) + " agent(s)";
  w.script_log(e);
}

void AiWorld3D::on_stop(World3D& w) {
  agents_.clear();
  crowd_.reset();
  registry().erase(&w);
  world_ = nullptr;
}

bool AiWorld3D::add_agent(std::uint64_t id, const scene3d::Agent3D& cfg) {
  if (!world_ || !crowd_ || !authority_) return false;
  const world3d::Entity3D* e = world_->find(id);
  if (!e) return false;
  remove_agent(id);
  auto a = std::make_unique<Agent>();
  a->owner = this;
  a->id = id;
  a->cfg = cfg;
  ai::CrowdAgentParams cp;
  cp.radius = cfg.radius;
  cp.height = cfg.height;
  cp.max_speed = cfg.speed;
  cp.max_accel = cfg.speed * 4.0f;
  cp.avoidance = cfg.avoidance;
  cp.separation = cfg.avoidance ? 2.0f : 0.0f;
  a->crowd = crowd_->add(v3(e->transform.position), cp);
  if (a->crowd < 0) return false;
  ai::AgentParams p;
  p.speed = cfg.speed;
  p.radius = cfg.radius;
  p.avoidance = cfg.avoidance ? 1.0f : 0.0f;
  p.arrive = std::max(cfg.radius, 0.35f);
  ai::PerceptionConfig senses;
  senses.sight_range = 12.0f;
  senses.fov_deg = 110.0f;
  a->brain = std::make_unique<ai::AgentBrain>(a.get(), p, senses);
  a->brain->set_home(v3(e->transform.position));
  const render3d::Vec3 eu = e->transform.rotation.to_euler_deg();
  const float yaw = render3d::radians(eu.y);
  a->fwd = {std::sin(yaw), 0.0f, std::cos(yaw)};
  a->brain->blackboard().set_number("wander_radius", cfg.wander_radius);
  agents_[id] = std::move(a);
  return true;
}

void AiWorld3D::remove_agent(std::uint64_t id) {
  auto it = agents_.find(id);
  if (it == agents_.end()) return;
  if (crowd_ && it->second->crowd >= 0) crowd_->remove(it->second->crowd);
  agents_.erase(it);
}

bool AiWorld3D::load_tree(std::uint64_t id, const std::string& rel, std::string* error) {
  Agent* a = agent(id);
  if (!a || !world_) {
    if (error) *error = "not an agent";
    return false;
  }
  std::ifstream f(world_->project_dir() + "/" + rel, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot open " + rel;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  auto tree = std::make_shared<ai::BtTree>();
  std::string err;
  if (!tree->parse(ss.str(), &err)) {
    if (error) *error = rel + ": " + err;
    return false;
  }
  a->bt_path = rel;
  a->brain->set_tree(tree, seed_ ^ (id * 0x9E3779B97F4A7C15ull));
  a->brain->blackboard().set_number("wander_radius", a->cfg.wander_radius);
  return true;
}

AiWorld3D::Agent* AiWorld3D::agent(std::uint64_t id) {
  auto it = agents_.find(id);
  return it == agents_.end() ? nullptr : it->second.get();
}

ai::AgentBrain* AiWorld3D::brain(std::uint64_t id) {
  Agent* a = agent(id);
  return a ? a->brain.get() : nullptr;
}

std::vector<std::uint64_t> AiWorld3D::agent_ids() const {
  std::vector<std::uint64_t> out;
  for (const auto& [id, a] : agents_) out.push_back(id);
  return out;
}

bool AiWorld3D::move_to(std::uint64_t id, const V3& goal) {
  Agent* a = agent(id);
  return a && a->brain->move_to(goal);
}

void AiWorld3D::stop(std::uint64_t id) {
  if (Agent* a = agent(id)) a->brain->stop();
}

ai::NavPath AiWorld3D::find_path(const V3& a, const V3& b) const {
  if (!mesh_->ready()) return {};
  return mesh_->find_path(a, b);
}

void AiWorld3D::noise(const V3& at, float loudness, const std::string& what) {
  if (!world_) return;
  const double now = static_cast<double>(world_->tick()) * World3D::kTickSeconds;
  for (auto& [id, a] : agents_) a->brain->hear(0, at, loudness, what, now);
}

void AiWorld3D::on_sound(World3D& w, const runtime::SoundEvent& ev) {
  if (ev.kind != runtime::SoundEvent::Kind::Play || !ev.positional) return;
  const double now = static_cast<double>(w.tick()) * World3D::kTickSeconds;
  for (auto& [id, a] : agents_) {
    if (ev.entity == id) continue;
    a->brain->hear(ev.entity, {ev.x, ev.y, ev.z}, ev.volume, ev.clip, now);
  }
}

void AiWorld3D::on_tick(World3D& w, double dt) {
  if (!authority_ || !crowd_) return;
  const double now = static_cast<double>(w.tick()) * World3D::kTickSeconds;
  // Drop agents whose entity is gone.
  for (auto it = agents_.begin(); it != agents_.end();) {
    if (!w.find(it->first)) {
      if (it->second->crowd >= 0) crowd_->remove(it->second->crowd);
      it = agents_.erase(it);
    } else {
      ++it;
    }
  }
  std::vector<ai::Perception::Candidate> riders;
  if (const world3d::Entity3D* p = w.find(w.player_id())) riders.push_back({p->id, v3(p->transform.position)});
  const ai::LineClearFn clear = [this](const V3& a, const V3& b) { return !mesh_->raycast(a, b); };
  for (auto& [id, a] : agents_) {
    a->brain->see(riders, clear, now);
    a->brain->tick(static_cast<float>(dt), now);
    // Hand the crowd the end of the brain's path.
    if (a->brain->moving()) {
      const V3 goal = a->brain->path().back();
      if (!a->has_goal || ai::distance(goal, a->goal) > 0.05f) {
        crowd_->set_target(a->crowd, goal);
        a->goal = goal;
        a->has_goal = true;
      }
    } else if (a->has_goal) {
      crowd_->stop(a->crowd);
      a->has_goal = false;
    }
  }
  crowd_->update(static_cast<float>(dt));
  for (auto& [id, a] : agents_) {
    world3d::Entity3D* e = w.live(id);
    if (!e) continue;
    const V3 p = crowd_->position(a->crowd);
    const V3 v = crowd_->velocity(a->crowd);
    e->transform.position = rv(p);
    const float speed = ai::length_xz(v);
    if (speed > 0.05f) {
      a->fwd = ai::normalized(ai::flat(v));
      render3d::Vec3 eu = e->transform.rotation.to_euler_deg();
      eu.y = render3d::degrees(std::atan2(v.x, v.z));
      e->transform.rotation = render3d::Quat::from_euler_deg(eu);
    }
    if (render3d::AnimatorRuntime* an = w.animator(id)) an->set_param("speed", speed);
  }
}

void AiWorld3D::debug_lines(const World3D& w, std::vector<Line>* out) const {
  if (!debug_) return;
  auto line = [out](const V3& a, const V3& b, float r, float g, float bl, float al) {
    Line l;
    l.a = rv(a);
    l.b = rv(b);
    l.rgba[0] = r;
    l.rgba[1] = g;
    l.rgba[2] = bl;
    l.rgba[3] = al;
    out->push_back(l);
  };
  const V3 lift{0, 0.05f, 0};
  for (std::size_t i = 0; i + 1 < edges_.size(); i += 2) line(edges_[i] + lift, edges_[i + 1] + lift, 0.2f, 0.8f, 0.9f, 0.6f);
  for (const ai::OffMeshLink& l : mesh_->links()) line(l.a + V3{0, 0.3f, 0}, l.b + V3{0, 0.3f, 0}, 0.9f, 0.4f, 1.0f, 1.0f);
  for (const auto& [id, a] : agents_) {
    const world3d::Entity3D* e = w.find(id);
    if (!e) continue;
    const V3 at = v3(e->transform.position);
    // Path.
    V3 prev = at + V3{0, 0.1f, 0};
    const auto& pts = a->brain->path();
    for (std::size_t k = a->brain->next_waypoint(); k < pts.size(); ++k) {
      line(prev, pts[k] + V3{0, 0.1f, 0}, 1.0f, 0.85f, 0.2f, 1.0f);
      prev = pts[k] + V3{0, 0.1f, 0};
    }
    // BT state post over the head: running green, success blue, failure red.
    float r = 0.6f, g = 0.6f, b = 0.6f;
    if (const ai::BtInstance* bt = a->brain->tree()) {
      if (bt->last_status() == ai::BtStatus::Running) r = 0.3f, g = 0.9f, b = 0.3f;
      else if (bt->last_status() == ai::BtStatus::Success) r = 0.3f, g = 0.6f, b = 1.0f;
      else if (bt->last_status() == ai::BtStatus::Failure) r = 1.0f, g = 0.3f, b = 0.3f;
    }
    line(at + V3{0, a->cfg.height, 0}, at + V3{0, a->cfg.height + 0.5f, 0}, r, g, b, 1.0f);
    // Sight cone edges.
    const float range = a->brain->senses().sight_range;
    const float half = render3d::radians(a->brain->senses().fov_deg * 0.5f);
    const float base = std::atan2(a->fwd.x, a->fwd.z);
    const V3 eye = at + V3{0, 1.5f, 0};
    for (float s : {-1.0f, 1.0f}) {
      const float ang = base + s * half;
      line(eye, eye + V3{std::sin(ang) * range, 0, std::cos(ang) * range}, 1.0f, 1.0f, 0.4f, 0.35f);
    }
  }
}

// ---- Lua -----------------------------------------------------------------------------

namespace {

AiWorld3D* self(lua_State* L) { return static_cast<AiWorld3D*>(lua_touserdata(L, lua_upvalueindex(1))); }

std::uint64_t entity_arg(lua_State* L, int idx) {
  if (void* u = luaL_testudata(L, idx, "TombStone.Entity")) return *static_cast<std::uint64_t*>(u);
  if (lua_isnumber(L, idx)) return static_cast<std::uint64_t>(lua_tointeger(L, idx));
  if (lua_isstring(L, idx) && self(L)->world()) return self(L)->world()->script_find(lua_tostring(L, idx));
  luaL_argerror(L, idx, "entity, id or name expected");
  return 0;
}

float fnum(lua_State* L, int i) { return static_cast<float>(luaL_checknumber(L, i)); }

float opt_field(lua_State* L, int t, const char* k, float d) {
  lua_getfield(L, t, k);
  const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : d;
  lua_pop(L, 1);
  return v;
}

int l_agent(lua_State* L) {
  const std::uint64_t id = entity_arg(L, 1);
  scene3d::Agent3D cfg;
  if (lua_istable(L, 2)) {
    cfg.speed = opt_field(L, 2, "speed", cfg.speed);
    cfg.radius = opt_field(L, 2, "radius", cfg.radius);
    cfg.height = opt_field(L, 2, "height", cfg.height);
    cfg.wander_radius = opt_field(L, 2, "wander", cfg.wander_radius);
    lua_getfield(L, 2, "avoidance");
    if (lua_isboolean(L, -1)) cfg.avoidance = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 2, "bt");
    if (lua_isstring(L, -1)) cfg.bt = lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  bool ok = self(L)->add_agent(id, cfg);
  if (ok && !cfg.bt.empty()) ok = self(L)->load_tree(id, cfg.bt);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_bt(lua_State* L) {
  const std::uint64_t id = entity_arg(L, 1);
  std::string err;
  const bool ok = self(L)->load_tree(id, luaL_checkstring(L, 2), &err);
  lua_pushboolean(L, ok ? 1 : 0);
  if (!ok) {
    lua_pushstring(L, err.c_str());
    return 2;
  }
  return 1;
}

int l_move_to(lua_State* L) {
  const std::uint64_t id = entity_arg(L, 1);
  lua_pushboolean(L, self(L)->move_to(id, {fnum(L, 2), fnum(L, 3), fnum(L, 4)}) ? 1 : 0);
  return 1;
}

int l_stop(lua_State* L) {
  self(L)->stop(entity_arg(L, 1));
  return 0;
}

int l_moving(lua_State* L) {
  ai::AgentBrain* b = self(L)->brain(entity_arg(L, 1));
  lua_pushboolean(L, b && b->moving() ? 1 : 0);
  return 1;
}

int l_path_length(lua_State* L) {
  const ai::NavPath p = self(L)->find_path({fnum(L, 1), fnum(L, 2), fnum(L, 3)}, {fnum(L, 4), fnum(L, 5), fnum(L, 6)});
  lua_pushnumber(L, p.found && !p.partial ? p.length() : -1.0);
  return 1;
}

int l_state(lua_State* L) {
  ai::AgentBrain* b = self(L)->brain(entity_arg(L, 1));
  lua_pushstring(L, b && b->tree() ? b->tree()->running_path().c_str() : "");
  return 1;
}

int l_set_bb(lua_State* L) {
  ai::AgentBrain* b = self(L)->brain(entity_arg(L, 1));
  const std::string k = luaL_checkstring(L, 2);
  if (!b) return 0;
  ai::Blackboard& bb = b->blackboard();
  if (luaL_testudata(L, 3, "TombStone.Entity")) bb.set_number(k, static_cast<double>(entity_arg(L, 3)));
  else if (lua_isboolean(L, 3)) bb.set_bool(k, lua_toboolean(L, 3) != 0);
  else if (lua_isnumber(L, 3) && lua_isnumber(L, 5)) bb.set_vec(k, {fnum(L, 3), fnum(L, 4), fnum(L, 5)});
  else if (lua_isnumber(L, 3)) bb.set_number(k, lua_tonumber(L, 3));
  else if (lua_isstring(L, 3)) bb.set_text(k, lua_tostring(L, 3));
  else bb.erase(k);
  return 0;
}

int l_get_bb(lua_State* L) {
  ai::AgentBrain* b = self(L)->brain(entity_arg(L, 1));
  const ai::BbValue* v = b ? b->blackboard().get(luaL_checkstring(L, 2)) : nullptr;
  if (!v) return lua_pushnil(L), 1;
  switch (v->type) {
    case ai::BbValue::Type::Number: lua_pushnumber(L, v->num); return 1;
    case ai::BbValue::Type::Bool: lua_pushboolean(L, v->flag ? 1 : 0); return 1;
    case ai::BbValue::Type::Text: lua_pushstring(L, v->text.c_str()); return 1;
    case ai::BbValue::Type::Vec:
      lua_pushnumber(L, v->vec.x);
      lua_pushnumber(L, v->vec.y);
      lua_pushnumber(L, v->vec.z);
      return 3;
    default: lua_pushnil(L); return 1;
  }
}

int l_noise(lua_State* L) {
  self(L)->noise({fnum(L, 1), fnum(L, 2), fnum(L, 3)}, static_cast<float>(luaL_optnumber(L, 4, 1.0)),
                 luaL_optstring(L, 5, "noise"));
  return 0;
}

int l_can_see(lua_State* L) {
  ai::AgentBrain* b = self(L)->brain(entity_arg(L, 1));
  lua_pushboolean(L, b && b->perception().sees(entity_arg(L, 2)) ? 1 : 0);
  return 1;
}

int l_debug(lua_State* L) {
  if (lua_gettop(L) >= 1) self(L)->set_debug(lua_toboolean(L, 1) != 0);
  lua_pushboolean(L, self(L)->debug() ? 1 : 0);
  return 1;
}

int l_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(self(L)->agent_ids().size()));
  return 1;
}

}  // namespace

void AiWorld3D::open_lua(World3D& /*w*/, lua_State* L, int sandbox) {
  const luaL_Reg fns[] = {{"agent", &l_agent},   {"bt", &l_bt},
                          {"move_to", &l_move_to}, {"stop", &l_stop},
                          {"moving", &l_moving},   {"path_length", &l_path_length},
                          {"state", &l_state},     {"set_bb", &l_set_bb},
                          {"get_bb", &l_get_bb},   {"noise", &l_noise},
                          {"can_see", &l_can_see}, {"debug", &l_debug},
                          {"count", &l_count},     {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, fns, 1);
  lua_setfield(L, sandbox, "ai");
}

}  // namespace ai3d
}  // namespace tombstone
}  // namespace ts
