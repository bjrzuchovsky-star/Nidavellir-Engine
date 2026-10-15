#include "plugins/ai/AiPlugin.h"

#include "ai/AiJson.h"
#include "plugin/Catalog.h"
#include "plugins/survival/FrontierSim.h"
#include "plugins/survival/Tiles.h"
#include "runtime/Collision.h"
#include "runtime/World.h"

// Lua is built as C++ (cmake/FetchLua.cmake): no extern "C".
#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace plugins {

using runtime::World;
using ai::V3;

namespace {

AiPlugin*& instance_ref() {
  static AiPlugin* p = nullptr;
  return p;
}

// Every live ts.ai instance (one per plugin manager); the Frontier hook asks
// whichever owns the world.
std::vector<AiPlugin*>& live() {
  static std::vector<AiPlugin*> v;
  return v;
}

void install_mob_steer() {
  survival::FrontierSim::set_mob_steer(
      [](const runtime::World& w, std::uint32_t mob, float x, float y, float tx, float ty, float /*half*/,
         const std::function<bool(float, float)>& walkable, float* dx, float* dy) {
        for (AiPlugin* p : live()) {
          if (p->active(w)) return p->steer_around(w, mob, x, y, tx, ty, survival::kTile, walkable, dx, dy);
        }
        return false;
      });
}

V3 to3(float x, float y) { return {x, 0.0f, y}; }

std::int64_t key_of(int x, int y) {
  return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
}

}  // namespace

// ---- Per-world state ---------------------------------------------------------------

struct AiPlugin::WorldAi {
  World* w = nullptr;
  float cell = 32.0f;
  std::vector<runtime::Aabb> solids;  // static solid colliders
  struct Map {
    const Entity2D* e = nullptr;
    SolidTable table;
  };
  std::vector<Map> maps;
  bool scanned = false;
  std::unordered_map<std::int64_t, char> memo;
  std::unique_ptr<ai::HierGrid> hier;
  std::map<std::uint64_t, Agent> agents;
  std::map<std::pair<int, int>, std::pair<std::uint64_t, ai::FlowField>> flows;
  std::uint64_t sounds_seen = 0;
  std::uint64_t seed = 0x9E3779B97F4A7C15ull;

  void scan() {
    solids.clear();
    maps.clear();
    for (const runtime::Actor& a : w->actors()) {
      if (a.data.player || a.dead) continue;
      if (agents.count(a.data.id)) continue;
      if (a.data.tilemap) {
        maps.push_back({&a.data, w->tile_solidity().table(a.data.tilemap->tileset)});
      }
      if (a.data.collider && !a.data.collider->trigger && !a.data.collider->dynamic) {
        solids.push_back(runtime::collider_box(a.data));
      }
    }
    scanned = true;
  }
  bool blocked_px(float x0, float y0, float x1, float y1) {
    if (!scanned) scan();
    const runtime::Aabb box{x0, y0, x1, y1};
    for (const runtime::Aabb& s : solids) {
      if (runtime::overlaps(s, box)) return true;
    }
    for (const Map& m : maps) {
      const TileMapData& t = *m.e->tilemap;
      const float ts = static_cast<float>(t.tile_size);
      const int c0 = static_cast<int>(std::floor((x0 - m.e->x) / ts));
      const int c1 = static_cast<int>(std::floor((x1 - 0.01f - m.e->x) / ts));
      const int r0 = static_cast<int>(std::floor((y0 - m.e->y) / ts));
      const int r1 = static_cast<int>(std::floor((y1 - 0.01f - m.e->y) / ts));
      for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
          const int id = t.at(c, r);
          if (id > 0 && m.table(id)) return true;
        }
      }
    }
    return false;
  }
  bool walk(int cx, int cy) {
    const std::int64_t k = key_of(cx, cy);
    auto it = memo.find(k);
    if (it != memo.end()) return it->second != 0;
    const float inset = 1.0f;
    const bool ok = !blocked_px(cx * cell + inset, cy * cell + inset, (cx + 1) * cell - inset, (cy + 1) * cell - inset);
    memo.emplace(k, ok ? 1 : 0);
    return ok;
  }
  ai::WalkFn walk_fn() {
    return [this](int x, int y) { return walk(x, y); };
  }
  ai::Cell cell_of(float x, float y) const {
    return {static_cast<int>(std::floor(x / cell)), static_cast<int>(std::floor(y / cell))};
  }
  V3 centre(ai::Cell c) const { return to3((c.x + 0.5f) * cell, (c.y + 0.5f) * cell); }
  void reset_cache() {
    memo.clear();
    scanned = false;
    hier.reset();
    flows.clear();
  }
  // Nearest walkable cell within a couple of rings (agents hugging a wall).
  bool nearest_walk(ai::Cell* c) {
    if (walk(c->x, c->y)) return true;
    for (int r = 1; r <= 2; ++r) {
      for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
          if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
          if (walk(c->x + dx, c->y + dy)) {
            c->x += dx;
            c->y += dy;
            return true;
          }
        }
      }
    }
    return false;
  }
  bool plan(const V3& from, const V3& to, std::vector<V3>* out) {
    ai::Cell a = cell_of(from.x, from.z), b = cell_of(to.x, to.z);
    if (!nearest_walk(&a) || !walk(b.x, b.y)) return false;
    ai::GridPath p;
    const int span = std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
    if (span <= 96) {
      ai::GridOptions opt;
      opt.bounds = ai::GridBounds::around(a, b, 24);
      p = ai::jps(walk_fn(), a, b, opt);
    } else {
      if (!hier) hier = std::make_unique<ai::HierGrid>(walk_fn(), 16);
      p = hier->find(a, b);
    }
    if (!p.found) return false;
    const std::vector<ai::Cell> cells = ai::smooth_path(walk_fn(), p.cells);
    out->clear();
    out->push_back(from);
    for (std::size_t i = 1; i + 1 < cells.size(); ++i) out->push_back(centre(cells[i]));
    out->push_back(to);
    return true;
  }
  bool line_clear(const V3& p, const V3& q) {
    return ai::line_walkable(walk_fn(), cell_of(p.x, p.z), cell_of(q.x, q.z));
  }
};

struct AiPlugin::Body2D : ai::AgentBody {
  WorldAi* wa = nullptr;
  std::uint64_t id = 0;
  V3 vel;
  V3 fwd{1.0f, 0.0f, 0.0f};
  const runtime::Actor* actor() const { return wa->w->find(id); }
  V3 position() const override {
    const runtime::Actor* a = actor();
    return a ? to3(a->data.x + a->data.w * 0.5f, a->data.y + a->data.h * 0.5f) : V3{};
  }
  V3 forward() const override { return fwd; }
  bool plan(const V3& from, const V3& to, std::vector<V3>* points) override { return wa->plan(from, to, points); }
  bool random_point(const V3& around, float radius, float u, float v, V3* out) override {
    const float ang = u * 6.2831853f, r = std::sqrt(v) * radius;
    const V3 p = around + V3{std::cos(ang) * r, 0.0f, std::sin(ang) * r};
    const ai::Cell c = wa->cell_of(p.x, p.z);
    if (!wa->walk(c.x, c.y)) return false;
    *out = wa->centre(c);
    return true;
  }
  bool entity_pos(std::uint64_t e, V3* out) const override {
    const runtime::Actor* a = wa->w->find(e);
    if (!a || a->dead) return false;
    *out = to3(a->data.x + a->data.w * 0.5f, a->data.y + a->data.h * 0.5f);
    return true;
  }
  std::uint64_t find(const std::string& name) const override {
    if (name == "player") return wa->w->script_player(0);
    return wa->w->script_find(name);
  }
  void desire(const V3& v) override {
    vel = v;
    if (ai::length_xz(v) > 1e-6f) fwd = ai::normalized(ai::flat(v));
  }
  bool attack(std::uint64_t target, double damage) override {
    const ScriptValue* hp = wa->w->script_get(target, "hp");
    if (hp && hp->type == ScriptValue::Type::Number) {
      const ScriptValue nv = ScriptValue::of_number(hp->number - damage);
      wa->w->script_set(target, "hp", &nv);
    }
    const ScriptValue hit = ScriptValue::of_number(static_cast<double>(id));
    wa->w->script_set(target, "ai_hit_by", &hit);
    return true;
  }
  bool play(const std::string& clip, float) override { return wa->w->script_play(id, clip, false); }
  void log(const std::string& text) override {
    runtime::LogEntry e;
    e.channel = "ai";
    e.text = text;
    e.entity = id;
    e.tick = wa->w->tick();
    wa->w->script_log(e);
  }
};

AiPlugin::~AiPlugin() {
  if (instance_ref() == this) instance_ref() = nullptr;
  auto& v = live();
  v.erase(std::remove(v.begin(), v.end(), this), v.end());
  if (v.empty()) survival::FrontierSim::set_mob_steer(nullptr);
}

AiPlugin* AiPlugin::instance() { return instance_ref(); }

const plugin::PluginInfo& AiPlugin::static_info() {
  static const plugin::PluginInfo info = [] {
    plugin::PluginInfo i;
    i.id = "ts.ai";
    i.name = "Pathfinder (AI Toolkit)";
    i.description =
        "Behaviour trees (*.bt.json) with utility scoring, 2D grid A* / jump point / hierarchical "
        "paths and flow fields, 3D Recast/Detour navmesh + crowds, perception (sight + hearing), "
        "a state machine helper, debug draw and the `ai` Lua / graph / C++ / C# API. "
        "Frontier coyotes path around walls and Old Hank wanders town when it is on.";
    i.author = "TombStone";
    i.version = plugin::Version{1, 0, 0};
    i.category = plugin::PluginCategory::Gameplay;
    i.project_types = plugin::kType2D | plugin::kType3D | plugin::kTypeVR;
    i.default_enabled = false;
    return i;
  }();
  return info;
}

AiPlugin::WorldAi& AiPlugin::state(World& w) {
  auto& slot = worlds_[&w];
  if (!slot) {
    slot = std::make_unique<WorldAi>();
    slot->w = &w;
  }
  return *slot;
}

const AiPlugin::WorldAi* AiPlugin::find_state(const World& w) const {
  auto it = worlds_.find(&w);
  return it == worlds_.end() ? nullptr : it->second.get();
}

bool AiPlugin::active(const World& w) const { return worlds_.count(&w) != 0; }

std::size_t AiPlugin::agent_count(const World& w) const {
  const WorldAi* s = find_state(w);
  return s ? s->agents.size() : 0;
}

bool AiPlugin::add_agent(World& w, std::uint64_t id, const ai::AgentParams& p, const ai::PerceptionConfig& senses) {
  if (w.net_role() == runtime::NetRole::Client || !w.find(id)) return false;
  WorldAi& s = state(w);
  Agent& a = s.agents[id];
  a.id = id;
  a.body = std::make_unique<Body2D>();
  a.body->wa = &s;
  a.body->id = id;
  a.brain = std::make_unique<ai::AgentBrain>(a.body.get(), p, senses);
  a.brain->set_home(a.body->position());
  s.scanned = false;  // the agent's own collider no longer counts as a wall
  s.memo.clear();
  s.hier.reset();
  return true;
}

void AiPlugin::remove_agent(World& w, std::uint64_t id) {
  if (WorldAi* s = const_cast<WorldAi*>(find_state(w))) s->agents.erase(id);
}

AiPlugin::Agent* AiPlugin::agent(const World& w, std::uint64_t id) {
  auto it = worlds_.find(&w);
  if (it == worlds_.end()) return nullptr;
  auto a = it->second->agents.find(id);
  return a == it->second->agents.end() ? nullptr : &a->second;
}

std::vector<const AiPlugin::Agent*> AiPlugin::agents(const World& w) const {
  std::vector<const Agent*> out;
  if (const WorldAi* s = find_state(w)) {
    for (const auto& [id, a] : s->agents) out.push_back(&a);
  }
  return out;
}

bool AiPlugin::load_tree(World& w, std::uint64_t id, const std::string& rel, std::string* error) {
  Agent* a = agent(w, id);
  if (!a) {
    if (error) *error = "not an agent";
    return false;
  }
  std::ifstream f(w.project_dir() + "/" + rel, std::ios::binary);
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
  a->brain->set_tree(tree, state(w).seed ^ (id * 0x9E3779B97F4A7C15ull));
  return true;
}

bool AiPlugin::move_to(World& w, std::uint64_t id, float x, float y) {
  Agent* a = agent(w, id);
  return a && a->brain->move_to(to3(x, y));
}

bool AiPlugin::find_path(World& w, float x0, float y0, float x1, float y1, std::vector<V3>* out) {
  return state(w).plan(to3(x0, y0), to3(x1, y1), out);
}

bool AiPlugin::flow(World& w, float gx, float gy, float x, float y, float* dx, float* dy) {
  WorldAi& s = state(w);
  const ai::Cell g = s.cell_of(gx, gy);
  auto& slot = s.flows[{g.x, g.y}];
  // Rebuilt at most once a second per goal cell (cheap for crowds).
  if (slot.first == 0 || w.tick() >= slot.first + 60) {
    ai::GridBounds b = ai::GridBounds::around(g, g, 64);
    slot.second.build(s.walk_fn(), b, {g});
    slot.first = w.tick() + 1;
  }
  const ai::Cell c = s.cell_of(x, y);
  if (!slot.second.reachable(c.x, c.y)) return false;
  slot.second.direction(c.x, c.y, dx, dy);
  return true;
}

void AiPlugin::noise(World& w, float x, float y, float loudness, const std::string& what) {
  if (w.net_role() == runtime::NetRole::Client) return;
  WorldAi& s = state(w);
  for (auto& [id, a] : s.agents) a.brain->hear(0, to3(x, y), loudness, what, w.time_seconds());
}

void AiPlugin::invalidate(World& w) { state(w).reset_cache(); }

void AiPlugin::set_cell(World& w, float px) {
  WorldAi& s = state(w);
  s.cell = std::clamp(px, 4.0f, 512.0f);
  s.reset_cache();
}

// ---- Per tick ------------------------------------------------------------------------

void AiPlugin::run(World& w, double dt) {
  current_ = &w;
  if (w.net_role() == runtime::NetRole::Client) return;  // server-side only
  auto it = worlds_.find(&w);
  if (it == worlds_.end()) return;
  WorldAi& s = *it->second;
  const double now = w.time_seconds();
  // Drop agents whose entity is gone.
  for (auto a = s.agents.begin(); a != s.agents.end();) {
    const runtime::Actor* act = w.find(a->first);
    if (!act || act->dead) a = s.agents.erase(a);
    else ++a;
  }
  // Hearing: new positional sound events.
  const std::uint64_t total = w.sound_total();
  if (total > s.sounds_seen) {
    const auto& pending = w.pending_sounds();
    const std::size_t fresh = static_cast<std::size_t>(std::min<std::uint64_t>(total - s.sounds_seen, pending.size()));
    for (std::size_t k = pending.size() - fresh; k < pending.size(); ++k) {
      const runtime::SoundEvent& ev = pending[k];
      if (ev.kind != runtime::SoundEvent::Kind::Play || !ev.positional || ev.three_d) continue;
      for (auto& [id, a] : s.agents) {
        if (ev.entity == id) continue;
        a.brain->hear(ev.entity, to3(ev.x, ev.y), ev.volume, ev.clip, now);
      }
    }
    s.sounds_seen = total;
  }
  // Sight: riders are the candidates.
  std::vector<ai::Perception::Candidate> riders;
  for (int slot = 0; slot < kMaxPlayers; ++slot) {
    if (const runtime::Actor* p = w.player(slot)) {
      riders.push_back({p->data.id, to3(p->data.x + p->data.w * 0.5f, p->data.y + p->data.h * 0.5f)});
    }
  }
  const ai::LineClearFn clear = [&s](const V3& a, const V3& b) { return s.line_clear(a, b); };
  for (auto& [id, a] : s.agents) {
    a.brain->see(riders, clear, now);
    a.brain->tick(static_cast<float>(dt), now);
  }
  // Integrate: desired velocity, separation (avoidance), walls.
  std::vector<std::pair<Agent*, V3>> moves;
  for (auto& [id, a] : s.agents) moves.push_back({&a, a.body->position()});
  const float fdt = static_cast<float>(dt);
  for (auto& [a, pos] : moves) {
    V3 v = a->body->vel;
    const ai::AgentParams& p = a->brain->params();
    if (p.avoidance > 0.0f) {
      V3 push;
      for (auto& [b, q] : moves) {
        if (b == a) continue;
        const float rr = p.radius + b->brain->params().radius;
        const V3 d = ai::flat(pos - q);
        const float len = ai::length_xz(d);
        if (len < rr && len > 1e-4f) push = push + d * ((rr - len) / len);
        else if (len <= 1e-4f) push = push + V3{static_cast<float>((a->id % 7)) - 3.0f, 0, 1};
      }
      v = v + push * (p.avoidance * 6.0f);
      v = ai::truncate(v, p.speed * 1.25f);
    }
    V3 next = pos + v * fdt;
    ai::Cell c = s.cell_of(next.x, next.z);
    if (!s.walk(c.x, c.y)) {
      // Slide along the blocked axis.
      const ai::Cell cx = s.cell_of(next.x, pos.z), cz = s.cell_of(pos.x, next.z);
      if (s.walk(cx.x, cx.y)) next.z = pos.z;
      else if (s.walk(cz.x, cz.y)) next.x = pos.x;
      else next = pos;
    }
    pos = next;
  }
  for (auto& [a, pos] : moves) {
    const runtime::Actor* act = w.find(a->id);
    if (!act) continue;
    if (ai::distance_xz(pos, a->body->position()) < 1e-5f) continue;
    w.script_move(a->id, pos.x - act->data.w * 0.5f, pos.z - act->data.h * 0.5f);
  }
}

// ---- Debug draw ----------------------------------------------------------------------

void AiPlugin::draw(const World& w, const runtime::WorldRect& view, std::vector<runtime::DrawQuad>* out) {
  if (!debug_) return;
  const WorldAi* s = find_state(w);
  if (!s) return;
  auto quad = [&](float x, float y, float h, const float c[4]) {
    if (x + h < view.x0 || x - h > view.x1 || y + h < view.y0 || y - h > view.y1) return;
    runtime::DrawQuad q;
    q.x0 = x - h;
    q.y0 = y - h;
    q.x1 = x + h;
    q.y1 = y + h;
    for (int k = 0; k < 4; ++k) q.rgba[k] = c[k];
    out->push_back(q);
  };
  // Blocked cells around agents (navigation grid).
  const float wall[4] = {0.9f, 0.2f, 0.2f, 0.18f};
  const float path[4] = {1.0f, 0.85f, 0.2f, 0.9f};
  for (const auto& [id, a] : s->agents) {
    const V3 at = a.body->position();
    const ai::Cell c0 = s->cell_of(at.x, at.z);
    for (int dy = -6; dy <= 6; ++dy) {
      for (int dx = -6; dx <= 6; ++dx) {
        auto m = s->memo.find(key_of(c0.x + dx, c0.y + dy));
        if (m != s->memo.end() && !m->second) {
          quad((c0.x + dx + 0.5f) * s->cell, (c0.y + dy + 0.5f) * s->cell, s->cell * 0.5f, wall);
        }
      }
    }
    // Path: dotted line through the waypoints.
    const auto& pts = a.brain->path();
    V3 prev = at;
    for (std::size_t k = a.brain->next_waypoint(); k < pts.size(); ++k) {
      const V3 d = pts[k] - prev;
      const int steps = std::max(1, static_cast<int>(ai::length_xz(d) / 6.0f));
      for (int t = 0; t <= steps; ++t) {
        const V3 p = prev + d * (static_cast<float>(t) / steps);
        quad(p.x, p.z, 1.5f, path);
      }
      quad(pts[k].x, pts[k].z, 3.5f, path);
      prev = pts[k];
    }
    // BT live state: marker colour (running green, success blue, failure red).
    float st[4] = {0.6f, 0.6f, 0.6f, 0.9f};
    if (const ai::BtInstance* bt = a.brain->tree()) {
      switch (bt->last_status()) {
        case ai::BtStatus::Running: st[0] = 0.3f, st[1] = 0.9f, st[2] = 0.3f; break;
        case ai::BtStatus::Success: st[0] = 0.3f, st[1] = 0.6f, st[2] = 1.0f; break;
        case ai::BtStatus::Failure: st[0] = 1.0f, st[1] = 0.3f, st[2] = 0.3f; break;
        default: break;
      }
    }
    quad(at.x, at.z - a.brain->params().radius - 8.0f, 4.0f, st);
    // Facing (sight cone axis).
    const V3 f = a.body->forward();
    for (int t = 1; t <= 6; ++t) quad(at.x + f.x * t * 6.0f, at.z + f.z * t * 6.0f, 1.2f, st);
  }
}

// ---- Frontier coyotes ----------------------------------------------------------------

bool AiPlugin::steer_around(const World& w, std::uint32_t key, float x, float y, float tx, float ty, float tile,
                            const WalkPx& walk_px, float* dx, float* dy) {
  if (!active(w)) return false;
  auto cell = [tile](float v) { return static_cast<int>(std::floor(v / tile)); };
  const ai::WalkFn walk = [&](int cx, int cy) { return walk_px((cx + 0.5f) * tile, (cy + 0.5f) * tile); };
  const ai::Cell a{cell(x), cell(y)}, b{cell(tx), cell(ty)};
  CoyotePlan& plan = coyotes_[{&w, key}];
  ++coyote_tick_;
  const bool stale = plan.cells.empty() || plan.goal_x != b.x || plan.goal_y != b.y || coyote_tick_ > plan.made + 30;
  if (stale) {
    // Straight line clear: no search needed.
    if (ai::line_walkable(walk, a, b)) {
      plan.cells = {a, b};
    } else {
      ai::GridOptions opt;
      opt.bounds = ai::GridBounds::around(a, b, 10);
      const ai::GridPath p = ai::jps(walk, a, b, opt);
      if (!p.found) {
        plan.cells.clear();
        return false;
      }
      plan.cells = ai::smooth_path(walk, p.cells);
      ++coyote_plans_;
    }
    plan.next = 1;
    plan.goal_x = b.x;
    plan.goal_y = b.y;
    plan.made = coyote_tick_;
  }
  while (plan.next < plan.cells.size()) {
    const ai::Cell c = plan.cells[plan.next];
    const float cx = (c.x + 0.5f) * tile, cy = (c.y + 0.5f) * tile;
    if (std::hypot(cx - x, cy - y) > tile * 0.35f || plan.next + 1 == plan.cells.size()) break;
    ++plan.next;
  }
  float gx = tx, gy = ty;
  if (plan.next + 1 < plan.cells.size()) {
    gx = (plan.cells[plan.next].x + 0.5f) * tile;
    gy = (plan.cells[plan.next].y + 0.5f) * tile;
  }
  const float d = std::hypot(gx - x, gy - y);
  if (d < 1e-3f) return false;
  *dx = (gx - x) / d;
  *dy = (gy - y) / d;
  return true;
}

// ---- Lua -----------------------------------------------------------------------------

namespace {

AiPlugin* self(lua_State* L) { return static_cast<AiPlugin*>(lua_touserdata(L, lua_upvalueindex(1))); }
World** world_slot(lua_State* L) { return static_cast<World**>(lua_touserdata(L, lua_upvalueindex(2))); }

World* world_of(lua_State* L) {
  World* w = *world_slot(L);
  return (w && w->net_role() != runtime::NetRole::Client) ? w : nullptr;
}

std::uint64_t entity_arg(lua_State* L, int idx) {
  if (void* u = luaL_testudata(L, idx, "TombStone.Entity")) return *static_cast<std::uint64_t*>(u);
  if (lua_isnumber(L, idx)) return static_cast<std::uint64_t>(lua_tointeger(L, idx));
  if (lua_isstring(L, idx)) {
    World* w = *world_slot(L);
    return w ? w->script_find(lua_tostring(L, idx)) : 0;
  }
  luaL_argerror(L, idx, "entity, id or name expected");
  return 0;
}

float opt_num(lua_State* L, int t, const char* k, float d) {
  lua_getfield(L, t, k);
  const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : d;
  lua_pop(L, 1);
  return v;
}

int l_agent(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  if (!w) return lua_pushboolean(L, 0), 1;
  ai::AgentParams p;
  p.speed = 90.0f;
  p.radius = 12.0f;
  ai::PerceptionConfig senses;
  senses.sight_range = 320.0f;
  senses.near_sense = 40.0f;
  senses.hearing_range = 480.0f;
  std::string bt;
  if (lua_istable(L, 2)) {
    p.speed = opt_num(L, 2, "speed", p.speed);
    p.radius = opt_num(L, 2, "radius", p.radius);
    p.avoidance = opt_num(L, 2, "avoidance", p.avoidance);
    senses.sight_range = opt_num(L, 2, "sight", senses.sight_range);
    senses.fov_deg = opt_num(L, 2, "fov", senses.fov_deg);
    senses.hearing_range = opt_num(L, 2, "hearing", senses.hearing_range);
    senses.memory = opt_num(L, 2, "memory", senses.memory);
    lua_getfield(L, 2, "bt");
    if (lua_isstring(L, -1)) bt = lua_tostring(L, -1);
    lua_pop(L, 1);
  } else if (lua_isnumber(L, 2)) {
    p.speed = static_cast<float>(lua_tonumber(L, 2));
  }
  bool ok = self(L)->add_agent(*w, id, p, senses);
  std::string err;
  if (ok && !bt.empty() && !self(L)->load_tree(*w, id, bt, &err)) {
    return luaL_error(L, "ai.agent: %s", err.c_str());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_remove(lua_State* L) {
  if (World* w = world_of(L)) self(L)->remove_agent(*w, entity_arg(L, 1));
  return 0;
}

int l_bt(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  const char* path = luaL_checkstring(L, 2);
  if (!w) return lua_pushboolean(L, 0), 1;
  std::string err;
  if (!self(L)->load_tree(*w, id, path, &err)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int l_move_to(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  const float x = static_cast<float>(luaL_checknumber(L, 2)), y = static_cast<float>(luaL_checknumber(L, 3));
  lua_pushboolean(L, w && self(L)->move_to(*w, id, x, y) ? 1 : 0);
  return 1;
}

int l_stop(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  if (w) {
    if (AiPlugin::Agent* a = self(L)->agent(*w, id)) a->brain->stop();
  }
  return 0;
}

int l_moving(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  AiPlugin::Agent* a = w ? self(L)->agent(*w, id) : nullptr;
  lua_pushboolean(L, a && a->brain->moving() ? 1 : 0);
  return 1;
}

int l_path(lua_State* L) {
  World* w = world_of(L);
  const float x0 = static_cast<float>(luaL_checknumber(L, 1)), y0 = static_cast<float>(luaL_checknumber(L, 2));
  const float x1 = static_cast<float>(luaL_checknumber(L, 3)), y1 = static_cast<float>(luaL_checknumber(L, 4));
  std::vector<V3> pts;
  if (!w || !self(L)->find_path(*w, x0, y0, x1, y1, &pts)) return lua_pushnil(L), 1;
  lua_createtable(L, static_cast<int>(pts.size()), 0);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, pts[i].x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, pts[i].z);
    lua_setfield(L, -2, "y");
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

// Scalar version for graphs / C++ / C# (the bridge carries numbers): length or -1.
int l_path_length(lua_State* L) {
  World* w = world_of(L);
  const float x0 = static_cast<float>(luaL_checknumber(L, 1)), y0 = static_cast<float>(luaL_checknumber(L, 2));
  const float x1 = static_cast<float>(luaL_checknumber(L, 3)), y1 = static_cast<float>(luaL_checknumber(L, 4));
  std::vector<V3> pts;
  if (!w || !self(L)->find_path(*w, x0, y0, x1, y1, &pts)) return lua_pushnumber(L, -1), 1;
  float len = 0.0f;
  for (std::size_t i = 1; i < pts.size(); ++i) len += ai::distance_xz(pts[i - 1], pts[i]);
  lua_pushnumber(L, len);
  return 1;
}

int l_flow(lua_State* L) {
  World* w = world_of(L);
  float dx = 0, dy = 0;
  const bool ok = w && self(L)->flow(*w, static_cast<float>(luaL_checknumber(L, 1)),
                                     static_cast<float>(luaL_checknumber(L, 2)),
                                     static_cast<float>(luaL_checknumber(L, 3)),
                                     static_cast<float>(luaL_checknumber(L, 4)), &dx, &dy);
  if (!ok) return lua_pushnil(L), 1;
  lua_pushnumber(L, dx);
  lua_pushnumber(L, dy);
  return 2;
}

void push_bb(lua_State* L, const ai::BbValue* v) {
  if (!v) return lua_pushnil(L);
  switch (v->type) {
    case ai::BbValue::Type::Number: lua_pushnumber(L, v->num); break;
    case ai::BbValue::Type::Bool: lua_pushboolean(L, v->flag ? 1 : 0); break;
    case ai::BbValue::Type::Text: lua_pushlstring(L, v->text.data(), v->text.size()); break;
    case ai::BbValue::Type::Vec:
      lua_createtable(L, 0, 2);
      lua_pushnumber(L, v->vec.x);
      lua_setfield(L, -2, "x");
      lua_pushnumber(L, v->vec.z);
      lua_setfield(L, -2, "y");
      break;
    default: lua_pushnil(L);
  }
}

int l_set_bb(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  const std::string k = luaL_checkstring(L, 2);
  AiPlugin::Agent* a = w ? self(L)->agent(*w, id) : nullptr;
  if (!a) return 0;
  ai::Blackboard& bb = a->brain->blackboard();
  if (luaL_testudata(L, 3, "TombStone.Entity")) {
    bb.set_number(k, static_cast<double>(entity_arg(L, 3)));
  } else if (lua_isboolean(L, 3)) {
    bb.set_bool(k, lua_toboolean(L, 3) != 0);
  } else if (lua_isnumber(L, 3) && lua_isnumber(L, 4)) {
    bb.set_vec(k, to3(static_cast<float>(lua_tonumber(L, 3)), static_cast<float>(lua_tonumber(L, 4))));
  } else if (lua_isnumber(L, 3)) {
    bb.set_number(k, lua_tonumber(L, 3));
  } else if (lua_isstring(L, 3)) {
    bb.set_text(k, lua_tostring(L, 3));
  } else if (lua_istable(L, 3)) {
    bb.set_vec(k, to3(opt_num(L, 3, "x", 0), opt_num(L, 3, "y", 0)));
  } else {
    bb.erase(k);
  }
  return 0;
}

int l_get_bb(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  const std::string k = luaL_checkstring(L, 2);
  AiPlugin::Agent* a = w ? self(L)->agent(*w, id) : nullptr;
  push_bb(L, a ? a->brain->blackboard().get(k) : nullptr);
  return 1;
}

int l_state(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  AiPlugin::Agent* a = w ? self(L)->agent(*w, id) : nullptr;
  if (!a || !a->brain->tree()) return lua_pushstring(L, ""), 1;
  lua_pushstring(L, a->brain->tree()->running_path().c_str());
  return 1;
}

int l_noise(lua_State* L) {
  World* w = world_of(L);
  const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
  const float loud = static_cast<float>(luaL_optnumber(L, 3, 1.0));
  const char* what = luaL_optstring(L, 4, "noise");
  if (w) self(L)->noise(*w, x, y, loud, what);
  return 0;
}

int l_can_see(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  const std::uint64_t t = entity_arg(L, 2);
  AiPlugin::Agent* a = w ? self(L)->agent(*w, id) : nullptr;
  lua_pushboolean(L, a && a->brain->perception().sees(t) ? 1 : 0);
  return 1;
}

int l_heard(lua_State* L) {
  World* w = world_of(L);
  const std::uint64_t id = entity_arg(L, 1);
  const double within = luaL_optnumber(L, 2, 2.0);
  AiPlugin::Agent* a = w ? self(L)->agent(*w, id) : nullptr;
  const ai::Percept* p = a ? a->brain->perception().last_heard(w->time_seconds(), within) : nullptr;
  if (!p) return lua_pushnil(L), 1;
  lua_pushnumber(L, p->pos.x);
  lua_pushnumber(L, p->pos.z);
  return 2;
}

int l_debug(lua_State* L) {
  if (lua_gettop(L) >= 1) self(L)->set_debug(lua_toboolean(L, 1) != 0);
  lua_pushboolean(L, self(L)->debug() ? 1 : 0);
  return 1;
}

int l_grid(lua_State* L) {
  World* w = world_of(L);
  if (w) self(L)->set_cell(*w, static_cast<float>(luaL_checknumber(L, 1)));
  return 0;
}

int l_invalidate(lua_State* L) {
  if (World* w = world_of(L)) self(L)->invalidate(*w);
  return 0;
}

int l_count(lua_State* L) {
  World* w = *world_slot(L);
  lua_pushinteger(L, w ? static_cast<lua_Integer>(self(L)->agent_count(*w)) : 0);
  return 1;
}

}  // namespace

void AiPlugin::on_register(plugin::Registrar& reg) {
  instance_ref() = this;
  if (std::find(live().begin(), live().end(), this) == live().end()) live().push_back(this);
  install_mob_steer();
  reg.add_component("Agent", "AI agent: speed, radius, avoidance, behaviour tree (ai.agent from a script)");
  reg.add_asset_type({".bt.json", "Behaviour tree"});
  reg.add_system("ai.agents", [this](World& w, double dt) { run(w, dt); });
  reg.add_draw_layer("ai.debug", true,
                     [this](const World& w, const runtime::WorldRect& view, float, std::vector<runtime::DrawQuad>* out) {
                       draw(w, view, out);
                     });
  reg.add_lua_api("ai", [this](lua_State* L) {
    lua_newtable(L);
    const luaL_Reg fns[] = {{"agent", &l_agent},       {"remove", &l_remove},
                            {"bt", &l_bt},             {"move_to", &l_move_to},
                            {"stop", &l_stop},         {"moving", &l_moving},
                            {"path", &l_path},         {"path_length", &l_path_length},
                            {"flow", &l_flow},         {"set_bb", &l_set_bb},
                            {"get_bb", &l_get_bb},     {"state", &l_state},
                            {"noise", &l_noise},       {"can_see", &l_can_see},
                            {"heard", &l_heard},       {"debug", &l_debug},
                            {"grid", &l_grid},         {"invalidate", &l_invalidate},
                            {"count", &l_count},       {nullptr, nullptr}};
    lua_pushlightuserdata(L, this);
    lua_pushlightuserdata(L, &current_);
    luaL_setfuncs(L, fns, 2);
  });
}

void AiPlugin::on_play_begin(World& world) {
  current_ = &world;
  if (world.net_role() != runtime::NetRole::Client) state(world);
}

void AiPlugin::on_play_end() {
  worlds_.clear();
  coyotes_.clear();
  current_ = nullptr;
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(ai, ::ts::tombstone::plugins::AiPlugin)
