#include "plugins/shipyard/ShipSim.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace shipyard {

namespace {

constexpr float kDt = static_cast<float>(runtime::kTickSeconds);
constexpr float kPi = 3.14159265358979f;

float wrap(float a) {
  while (a > kPi) a -= 2.0f * kPi;
  while (a <= -kPi) a += 2.0f * kPi;
  return a;
}

// --- Byte wire ----------------------------------------------------------------------------
struct Writer {
  std::vector<std::uint8_t> b;
  void u8(std::uint8_t v) { b.push_back(v); }
  void u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v & 0xFF));
    u8(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v & 0xFFFF));
    u16(static_cast<std::uint16_t>(v >> 16));
  }
  void f32(float f) {
    std::uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    u32(v);
  }
  void str(const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), 255);
    u8(static_cast<std::uint8_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
  }
};

struct Reader {
  const std::vector<std::uint8_t>& b;
  std::size_t i = 0;
  bool bad = false;
  explicit Reader(const std::vector<std::uint8_t>& bytes) : b(bytes) {}
  std::uint8_t u8() {
    if (i >= b.size()) {
      bad = true;
      return 0;
    }
    return b[i++];
  }
  std::uint16_t u16() {
    const std::uint16_t lo = u8();
    return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(u8()) << 8));
  }
  std::uint32_t u32() {
    const std::uint32_t lo = u16();
    return lo | (static_cast<std::uint32_t>(u16()) << 16);
  }
  float f32() {
    const std::uint32_t v = u32();
    float f = 0;
    std::memcpy(&f, &v, 4);
    return f;
  }
  std::string str() {
    const std::size_t n = u8();
    if (i + n > b.size()) {
      bad = true;
      return {};
    }
    std::string s(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(i + n));
    i += n;
    return s;
  }
};

void fnv(std::uint64_t* h, const void* p, std::size_t n) {
  const auto* c = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) *h = (*h ^ c[i]) * 1099511628211ull;
}

bool hostile(Team a, Team b) {
  return a != b && a != Team::Derelict && b != Team::Derelict;
}

}  // namespace

const char* team_name(Team t) {
  switch (t) {
    case Team::Posse: return "posse";
    case Team::Pirates: return "pirates";
    default: return "derelict";
  }
}

// --- Settings -------------------------------------------------------------------------------

bool SkySettings::parse(const std::string& text, std::string* error) {
  json::Value root;
  std::string err;
  if (!json::parse(text, &root, &err)) {
    if (error) *error = "sky.json: " + err;
    return false;
  }
  player_design = root.str("player_design", player_design);
  crew = root.flag("crew", crew);
  respawn = std::max(0.5f, root.flt("respawn_seconds", respawn));
  reinforce = std::max(0.0f, root.flt("reinforce_seconds", reinforce));
  enemies.clear();
  for (const json::Value& e : root["enemies"].items()) {
    EnemySpawn s;
    s.design = e.str("design");
    s.x = e.flt("x", 0.0f);
    s.y = e.flt("y", 0.0f);
    if (!s.design.empty()) enemies.push_back(std::move(s));
  }
  return true;
}

// --- Ship -------------------------------------------------------------------------------------

int Ship::part_at_cell(int x, int y) const {
  if (x < gx0 || y < gy0 || x >= gx0 + gw || y >= gy0 + gh) return -1;
  return cells[static_cast<std::size_t>((y - gy0) * gw + (x - gx0))];
}

std::vector<PartPose> Ship::poses() const {
  std::vector<PartPose> out;
  out.reserve(parts.size());
  for (const LivePart& p : parts) out.push_back(PartPose{p.def, p.x, p.y, p.rot});
  return out;
}

void Ship::to_world(float gx, float gy, float* wx, float* wy) const {
  const float c = std::cos(angle), s = std::sin(angle);
  const float lx = gx - cx, ly = gy - cy;
  *wx = x + c * lx - s * ly;
  *wy = y + s * lx + c * ly;
}

void Ship::to_grid(float wx, float wy, float* gx, float* gy) const {
  const float c = std::cos(angle), s = std::sin(angle);
  const float dx = wx - x, dy = wy - y;
  *gx = c * dx + s * dy + cx;
  *gy = -s * dx + c * dy + cy;
}

// --- Built-in designs -------------------------------------------------------------------

Design ShipSim::sloop_design() {
  Design d;
  d.name = "Dust Devil (sloop)";
  d.parts = {
      {"gatling", 3, 0, 0},   {"armor", 2, 0, 0},       {"armor", 4, 0, 0},
      {"hull", 2, 1, 0},      {"hull", 3, 1, 0},        {"hull", 4, 1, 0},
      {"boiler", 2, 2, 0},    {"shield_vane", 4, 2, 0}, {"gatling", 1, 2, 0},
      {"gatling", 5, 2, 0},   {"bunkhouse", 2, 4, 0},   {"gangway", 4, 4, 0},
      {"trim_jet", 1, 1, 1},  {"trim_jet", 5, 1, 3},    {"trim_jet", 1, 4, 1},
      {"trim_jet", 5, 4, 3},  {"steam_engine", 2, 5, 0}, {"steam_engine", 4, 5, 0},
      {"trim_jet", 3, 5, 2},
  };
  return d;
}

Design ShipSim::marauder_design() {
  Design d;
  d.name = "Black Kettle (marauder)";
  d.parts = {
      {"cannon", 2, 0, 0},    {"armor", 1, 0, 0},       {"armor", 4, 0, 0},
      {"armor", 1, 1, 0},     {"armor", 4, 1, 0},       {"boiler", 2, 2, 0},
      {"hull", 1, 2, 0},      {"hull", 4, 2, 0},        {"hull", 1, 3, 0},
      {"hull", 4, 3, 0},      {"gatling", 0, 2, 0},     {"gatling", 5, 2, 0},
      {"bunkhouse", 2, 4, 0}, {"gangway", 1, 4, 0},     {"gangway", 4, 4, 0},
      {"trim_jet", 0, 1, 1},  {"trim_jet", 5, 1, 3},    {"trim_jet", 0, 4, 1},
      {"trim_jet", 5, 4, 3},  {"steam_engine", 1, 5, 0}, {"steam_engine", 4, 5, 0},
      {"trim_jet", 2, 5, 2},
  };
  return d;
}

// --- Lifecycle ------------------------------------------------------------------------------

void ShipSim::begin(runtime::World* world, const std::string& project_dir, const PartLibrary& lib,
                    const SkySettings& sky) {
  *this = ShipSim();
  lib_ = lib;
  sky_ = sky;
  project_dir_ = project_dir;
  active_ = true;
  player_design_ = sloop_design();
  if (!project_dir.empty()) {
    Design d;
    std::string err;
    if (load_design((std::filesystem::path(project_dir) / sky.player_design).string(), &d, &err)) {
      player_design_ = std::move(d);
    } else {
      warnings_.push_back("Airship Works: " + err + " (flying the stock sloop)");
    }
  }
  const Validation v = validate(lib_, player_design_);
  if (!v.ok && !v.errors.empty()) {
    warnings_.push_back("Airship Works: your design won't fly (" + v.errors.front() +
                        "); flying the stock sloop.");
    player_design_ = sloop_design();
  }
  for (const EnemySpawn& e : sky_.enemies) {
    std::uint32_t id = 0;
    std::string err;
    if (!spawn_file(e.design, e.x, e.y, Team::Pirates, true, &id, &err)) {
      spawn(marauder_design(), e.x, e.y, Team::Pirates, true);
      warnings_.push_back("Airship Works: " + err + " (stock marauder instead)");
    }
  }
  (void)world;
}

void ShipSim::begin_client(const PartLibrary& lib) {
  *this = ShipSim();
  lib_ = lib;
  active_ = true;
  mirror_ = true;
}

void ShipSim::end() {
  ships_.clear();
  shots_.clear();
  active_ = false;
  mirror_ = false;
}

// --- Spawning ---------------------------------------------------------------------------------

void ShipSim::relayout(Ship& s) const {
  const ShipStats st = compute_stats(lib_, s.poses());
  s.mass = std::max(0.05f, st.mass);
  s.inertia = std::max(1.0f, st.inertia);
  s.cx = st.cx;
  s.cy = st.cy;
  s.radius = std::max(8.0f, st.radius);
  s.shield_max = st.shield;
  s.shield = std::min(s.shield, s.shield_max);
  s.gun_range = 0.0f;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool any = false;
  for (const LivePart& p : s.parts) {
    const PartDef* d = lib_.get(p.def);
    if (!d) continue;
    if (d->kind == PartKind::Gun) s.gun_range = std::max(s.gun_range, d->range);
    int w = 1, h = 1;
    footprint(*d, p.rot, &w, &h);
    if (!any) {
      x0 = p.x;
      y0 = p.y;
      x1 = p.x + w;
      y1 = p.y + h;
      any = true;
    }
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x + w);
    y1 = std::max(y1, p.y + h);
  }
  s.gx0 = x0;
  s.gy0 = y0;
  s.gw = any ? x1 - x0 : 0;
  s.gh = any ? y1 - y0 : 0;
  s.cells.assign(static_cast<std::size_t>(s.gw * s.gh), -1);
  for (std::size_t i = 0; i < s.parts.size(); ++i) {
    const LivePart& p = s.parts[i];
    const PartDef* d = lib_.get(p.def);
    if (!d) continue;
    int w = 1, h = 1;
    footprint(*d, p.rot, &w, &h);
    for (int yy = 0; yy < h; ++yy) {
      for (int xx = 0; xx < w; ++xx) {
        s.cells[static_cast<std::size_t>((p.y + yy - y0) * s.gw + (p.x + xx - x0))] = static_cast<int>(i);
      }
    }
  }
  s.layout_dirty = true;
}

std::uint32_t ShipSim::spawn(const Design& d, float x, float y, Team team, bool ai, int owner,
                             float angle) {
  Ship s;
  s.id = next_id_++;
  s.name = d.name;
  s.team = team;
  s.ai = ai;
  s.owner = owner;
  for (const PartPose& p : resolve(lib_, d)) {
    const PartDef* def = lib_.get(p.def);
    if (!def) continue;
    LivePart lp;
    lp.def = p.def;
    lp.x = p.x;
    lp.y = p.y;
    lp.rot = p.rot;
    lp.hp = def->hp;
    s.parts.push_back(lp);
  }
  if (s.parts.empty()) return 0;
  relayout(s);
  s.shield = s.shield_max;
  s.x = s.prev_x = x;
  s.y = s.prev_y = y;
  s.angle = angle;
  s.orbit = (s.id & 1u) ? 1.0f : -1.0f;
  ships_.push_back(std::move(s));
  return ships_.back().id;
}

bool ShipSim::spawn_file(const std::string& rel_path, float x, float y, Team team, bool ai,
                         std::uint32_t* id, std::string* error) {
  Design d;
  if (rel_path == "stock:sloop") d = sloop_design();
  else if (rel_path == "stock:marauder") d = marauder_design();
  else if (!load_design((std::filesystem::path(project_dir_) / rel_path).string(), &d, error)) return false;
  const std::uint32_t got = spawn(d, x, y, team, ai);
  if (!got) {
    if (error) *error = rel_path + " has no parts this library knows";
    return false;
  }
  if (id) *id = got;
  return true;
}

Ship* ShipSim::find(std::uint32_t id) {
  for (Ship& s : ships_) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

const Ship* ShipSim::find(std::uint32_t id) const {
  return const_cast<ShipSim*>(this)->find(id);
}

std::uint32_t ShipSim::ship_of(int slot) const {
  for (const Ship& s : ships_) {
    if (s.owner == slot) return s.id;
  }
  return 0;
}

void ShipSim::set_helm(std::uint32_t id, float in_x, float in_y, int turn, bool fire) {
  if (Ship* s = find(id)) {
    s->in_x = std::clamp(in_x, -1.0f, 1.0f);
    s->in_y = std::clamp(in_y, -1.0f, 1.0f);
    s->turn = turn > 0 ? 1 : (turn < 0 ? -1 : 0);
    s->fire = fire;
  }
}

bool ShipSim::remove_ship(std::uint32_t id) {
  for (std::size_t i = 0; i < ships_.size(); ++i) {
    if (ships_[i].id == id) {
      const int owner = ships_[i].owner;
      if (owner >= 0 && owner < kSlots) {
        respawn_at_[static_cast<std::size_t>(owner)] =
            static_cast<double>(ticks_) * runtime::kTickSeconds + sky_.respawn;
      }
      ships_.erase(ships_.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}

bool ShipSim::damage_part(std::uint32_t id, int index, int amount) {
  if (mirror_) return false;
  for (std::size_t i = 0; i < ships_.size(); ++i) {
    Ship& s = ships_[i];
    if (s.id != id) continue;
    if (index < 0 || index >= static_cast<int>(s.parts.size())) return false;
    LivePart& p = s.parts[static_cast<std::size_t>(index)];
    p.hp -= std::max(0, amount);
    if (p.hp > 0) return false;
    break_up(i);
    return true;
  }
  return false;
}

// --- Physics ---------------------------------------------------------------------------------

void ShipSim::apply_power(Ship& s) const {
  float steam = 0.0f;
  int hands = 0;
  for (const LivePart& p : s.parts) {
    const PartDef* d = lib_.get(p.def);
    if (!d) continue;
    if (d->power > 0) steam += d->power;
    if (d->crew > 0) hands += d->crew;
  }
  // Engines first, then guns, shields, the rest; part order inside each.
  static constexpr PartKind kOrder[] = {PartKind::Engine, PartKind::Gun, PartKind::Shield};
  std::vector<bool> done(s.parts.size(), false);
  auto feed = [&](std::size_t i) {
    LivePart& p = s.parts[i];
    const PartDef* d = lib_.get(p.def);
    done[i] = true;
    if (!d) {
      p.powered = false;
      return;
    }
    const float need = d->power < 0 ? -d->power : 0.0f;
    const int crew = sky_.crew && d->crew < 0 ? -d->crew : 0;
    if (need <= steam + 1e-4f && crew <= hands) {
      steam -= need;
      hands -= crew;
      p.powered = true;
    } else {
      p.powered = need <= 0.0f && crew == 0;
    }
  };
  for (PartKind k : kOrder) {
    for (std::size_t i = 0; i < s.parts.size(); ++i) {
      const PartDef* d = lib_.get(s.parts[i].def);
      if (d && d->kind == k) feed(i);
    }
  }
  for (std::size_t i = 0; i < s.parts.size(); ++i) {
    if (!done[i]) feed(i);
  }
}

void ShipSim::allocate_thrust(Ship& s, float* fx, float* fy, float* torque) const {
  float dx = s.in_x, dy = s.in_y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len > 1.0f) {
    dx /= len;
    dy /= len;
  }
  *fx = *fy = *torque = 0.0f;
  for (LivePart& p : s.parts) {
    p.firing = false;
    const PartDef* d = lib_.get(p.def);
    if (!d || d->kind != PartKind::Engine || !p.powered || d->thrust <= 0.0f) continue;
    float ex = 0, ey = 0;
    engine_dir(p.rot, &ex, &ey);
    float px = 0, py = 0;
    part_center(*d, PartPose{p.def, p.x, p.y, p.rot}, &px, &py);
    const float rx = px - s.cx, ry = py - s.cy;
    const float arm = rx * ey - ry * ex;  // torque per unit thrust (+ = clockwise)
    const int tsign = arm > 2.0f ? 1 : (arm < -2.0f ? -1 : 0);
    float score = dx * ex + dy * ey;
    if (s.turn != 0) score += static_cast<float>(s.turn * tsign);
    if (score <= 0.5f) continue;
    p.firing = true;
    *fx += ex * d->thrust;
    *fy += ey * d->thrust;
    *torque += arm * d->thrust;
  }
}

void ShipSim::physics() {
  for (Ship& s : ships_) {
    apply_power(s);
    float fx = 0, fy = 0, tq = 0;
    allocate_thrust(s, &fx, &fy, &tq);
    const float c = std::cos(s.angle), sn = std::sin(s.angle);
    const float wx = c * fx - sn * fy, wy = sn * fx + c * fy;
    s.prev_x = s.x;
    s.prev_y = s.y;
    s.vx += wx / s.mass * kDt;
    s.vy += wy / s.mass * kDt;
    s.vx *= 1.0f - kLinearDamping * kDt;
    s.vy *= 1.0f - kLinearDamping * kDt;
    s.x += s.vx * kDt;
    s.y += s.vy * kDt;
    s.spin += tq / s.inertia * kDt;
    s.spin *= 1.0f - kAngularDamping * kDt;
    s.angle = wrap(s.angle + s.spin * kDt);
    // Shields recharge from powered vanes.
    float regen = 0.0f;
    for (const LivePart& p : s.parts) {
      const PartDef* d = lib_.get(p.def);
      if (d && p.powered) regen += d->shield_regen;
    }
    s.shield = std::min(s.shield_max, s.shield + regen * kDt);
    s.hit_flash = std::max(0.0f, s.hit_flash - kDt);
  }
}

// --- Captains, guns, shot ----------------------------------------------------------------

std::uint32_t ShipSim::pick_target(const Ship& s, float range) const {
  std::uint32_t best = 0;
  float best_d = range * range;
  for (const Ship& o : ships_) {
    if (!hostile(s.team, o.team) || o.parts.empty()) continue;
    const float dx = o.x - s.x, dy = o.y - s.y;
    const float d = dx * dx + dy * dy;
    if (d < best_d) {
      best_d = d;
      best = o.id;
    }
  }
  return best;
}

void ShipSim::captains() {
  for (Ship& s : ships_) {
    if (!s.ai) continue;
    s.target = pick_target(s, 4000.0f);
    const Ship* t = find(s.target);
    if (!t) {
      s.in_x = s.in_y = 0.0f;
      s.turn = 0;
      s.fire = false;
      continue;
    }
    const float dx = t->x - s.x, dy = t->y - s.y;
    const float dist = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
    const float ux = dx / dist, uy = dy / dist;
    const float want = std::clamp(s.gun_range * 0.7f, 160.0f, 420.0f);
    float mx = 0, my = 0;
    if (dist > want + 60.0f) {
      mx = ux;
      my = uy;
    } else if (dist < want - 60.0f) {
      mx = -ux;
      my = -uy;
    } else {
      // Circle the prey, drifting back toward the ring.
      const float k = (dist - want) / 60.0f * 0.5f;
      mx = -uy * s.orbit + ux * k;
      my = ux * s.orbit + uy * k;
    }
    // World wish -> ship frame (bow = -y).
    const float c = std::cos(s.angle), sn = std::sin(s.angle);
    s.in_x = c * mx + sn * my;
    s.in_y = -sn * mx + c * my;
    // Keep the bow on the prey.
    const float heading = std::atan2(ux, -uy);
    const float err = wrap(heading - s.angle);
    const float cmd = err * 2.0f - s.spin * 0.9f;
    s.turn = cmd > 0.12f ? 1 : (cmd < -0.12f ? -1 : 0);
    s.fire = dist <= s.gun_range + 20.0f;
  }
}

void ShipSim::guns() {
  for (Ship& s : ships_) {
    const std::uint32_t tid = pick_target(s, std::max(1.0f, s.gun_range + 40.0f));
    const Ship* t = find(tid);
    for (LivePart& p : s.parts) {
      const PartDef* d = lib_.get(p.def);
      if (!d || d->kind != PartKind::Gun) continue;
      p.firing = false;
      p.cooldown = std::max(0.0f, p.cooldown - kDt);
      float gx = 0, gy = 0;
      part_center(*d, PartPose{p.def, p.x, p.y, p.rot}, &gx, &gy);
      float wx = 0, wy = 0;
      s.to_world(gx, gy, &wx, &wy);
      bool in_range = false;
      if (t) {
        // Lead the target a little.
        const float dx0 = t->x - wx, dy0 = t->y - wy;
        const float tt = std::sqrt(dx0 * dx0 + dy0 * dy0) / d->shot_speed;
        const float dx = dx0 + (t->vx - s.vx) * tt, dy = dy0 + (t->vy - s.vy) * tt;
        p.aim = std::atan2(dx, -dy);
        in_range = dx0 * dx0 + dy0 * dy0 <= d->range * d->range;
      } else {
        p.aim = s.angle;
      }
      const bool want = s.fire && (in_range || (!s.ai && s.owner >= 0));
      if (!want || !p.powered || p.cooldown > 0.0f || d->fire_rate <= 0.0f) continue;
      p.cooldown = 1.0f / d->fire_rate;
      p.firing = true;
      Shot sh;
      const float ax = std::sin(p.aim), ay = -std::cos(p.aim);
      const float muzzle = static_cast<float>(std::max(d->w, d->h)) * kCell * 0.5f;
      sh.x = wx + ax * muzzle;
      sh.y = wy + ay * muzzle;
      sh.vx = ax * d->shot_speed + s.vx;
      sh.vy = ay * d->shot_speed + s.vy;
      sh.damage = d->damage;
      sh.life = d->range / d->shot_speed * 1.15f;
      sh.ship = s.id;
      sh.team = s.team;
      shots_.push_back(sh);
      ++shots_fired_;
    }
  }
}

void ShipSim::step_shots() {
  std::vector<std::uint32_t> hurt;
  for (std::size_t k = 0; k < shots_.size();) {
    Shot& sh = shots_[k];
    sh.x += sh.vx * kDt;
    sh.y += sh.vy * kDt;
    sh.life -= kDt;
    bool gone = sh.life <= 0.0f;
    for (Ship& s : ships_) {
      if (gone) break;
      if (s.id == sh.ship || s.parts.empty()) continue;
      if (s.team == sh.team && s.team != Team::Derelict) continue;
      const float dx = sh.x - s.x, dy = sh.y - s.y;
      const float r = s.radius + (s.shield > 0.0f ? 10.0f : 2.0f);
      if (dx * dx + dy * dy > r * r) continue;
      if (s.shield > 0.0f) {
        s.shield = std::max(0.0f, s.shield - static_cast<float>(sh.damage));
        s.hit_flash = 0.15f;
        gone = true;
        break;
      }
      float gx = 0, gy = 0;
      s.to_grid(sh.x, sh.y, &gx, &gy);
      const int i = s.part_at_cell(static_cast<int>(std::floor(gx / kCell)),
                                   static_cast<int>(std::floor(gy / kCell)));
      if (i < 0) continue;
      s.parts[static_cast<std::size_t>(i)].hp -= sh.damage;
      s.hit_flash = 0.1f;
      if (s.parts[static_cast<std::size_t>(i)].hp <= 0 &&
          std::find(hurt.begin(), hurt.end(), s.id) == hurt.end()) {
        hurt.push_back(s.id);
      }
      gone = true;
    }
    if (gone) shots_.erase(shots_.begin() + static_cast<std::ptrdiff_t>(k));
    else ++k;
  }
  for (std::uint32_t id : hurt) {
    for (std::size_t i = 0; i < ships_.size(); ++i) {
      if (ships_[i].id == id) {
        break_up(i);
        break;
      }
    }
  }
}

void ShipSim::break_up(std::size_t index) {
  // Copy: the vector grows below.
  Ship old = ships_[index];
  std::vector<LivePart> alive;
  for (const LivePart& p : old.parts) {
    if (p.hp > 0) alive.push_back(p);
    else ++parts_destroyed_;
  }
  if (alive.empty()) {
    if (old.owner >= 0 && old.owner < kSlots) {
      respawn_at_[static_cast<std::size_t>(old.owner)] =
          static_cast<double>(ticks_) * runtime::kTickSeconds + sky_.respawn;
    }
    ships_.erase(ships_.begin() + static_cast<std::ptrdiff_t>(index));
    return;
  }
  std::vector<PartPose> poses;
  for (const LivePart& p : alive) poses.push_back(PartPose{p.def, p.x, p.y, p.rot});
  const auto groups = components(lib_, poses);
  auto carve = [&](Ship& t, const std::vector<int>& group) {
    t.parts.clear();
    for (int i : group) t.parts.push_back(alive[static_cast<std::size_t>(i)]);
    relayout(t);
    // Same parts, same place: move the body to the new centre of mass and
    // give it the velocity that point had.
    float wx = 0, wy = 0;
    old.to_world(t.cx, t.cy, &wx, &wy);
    const float rx = wx - old.x, ry = wy - old.y;
    t.x = t.prev_x = wx;
    t.y = t.prev_y = wy;
    t.vx = old.vx - old.spin * ry;
    t.vy = old.vy + old.spin * rx;
    t.angle = old.angle;
    t.spin = old.spin;
  };
  Ship& keep = ships_[index];
  carve(keep, groups.front());
  for (std::size_t g = 1; g < groups.size(); ++g) {
    Ship wreck;
    wreck.id = next_id_++;
    wreck.name = "Wreckage of " + old.name;
    wreck.team = Team::Derelict;
    wreck.ai = false;
    wreck.owner = -1;
    carve(wreck, groups[g]);
    wreck.shield = 0.0f;
    ships_.push_back(std::move(wreck));
    ++splits_;
  }
}

void ShipSim::reinforce() {
  if (sky_.reinforce <= 0.0f || sky_.enemies.empty()) return;
  bool pirates = false;
  for (const Ship& s : ships_) pirates |= s.team == Team::Pirates;
  const double now = static_cast<double>(ticks_) * runtime::kTickSeconds;
  if (pirates) {
    reinforce_at_ = -1.0;
    return;
  }
  if (reinforce_at_ < 0.0) {
    reinforce_at_ = now + sky_.reinforce;
    return;
  }
  if (now < reinforce_at_) return;
  reinforce_at_ = -1.0;
  // Fresh pirates arrive around the posse's centre.
  float cx = 0, cy = 0;
  int n = 0;
  for (const Ship& s : ships_) {
    if (s.team == Team::Posse) {
      cx += s.x;
      cy += s.y;
      ++n;
    }
  }
  if (n) {
    cx /= static_cast<float>(n);
    cy /= static_cast<float>(n);
  }
  for (const EnemySpawn& e : sky_.enemies) {
    std::uint32_t id = 0;
    if (!spawn_file(e.design, cx + e.x, cy + e.y, Team::Pirates, true, &id, nullptr)) {
      spawn(marauder_design(), cx + e.x, cy + e.y, Team::Pirates, true);
    }
  }
}

// --- Tick ------------------------------------------------------------------------------------

void ShipSim::riders(runtime::World& w) {
  const double now = static_cast<double>(ticks_) * runtime::kTickSeconds;
  const runtime::InputFrame& in = w.last_input();
  const bool server = w.net_role() == runtime::NetRole::Server;
  for (int slot = 0; slot < kSlots; ++slot) {
    const std::size_t si = static_cast<std::size_t>(slot);
    const runtime::Actor* a = w.player(slot);
    if (!a) {
      if (present_[si]) {
        present_[si] = false;
        if (const std::uint32_t id = ship_of(slot)) remove_ship(id);
      }
      continue;
    }
    if (!present_[si]) {
      present_[si] = true;
      spawn_at_[si] = {a->data.center_x(), a->data.center_y()};
      respawn_at_[si] = now;
      if (server) {
        Writer wr;
        wr.u8('W');
        wr.u8(static_cast<std::uint8_t>(slot));
        post(w, slot, std::move(wr.b));
        full_all_ = true;
      }
    }
    std::uint32_t id = ship_of(slot);
    if (!id && now >= respawn_at_[si]) {
      id = spawn(player_design_, spawn_at_[si].first, spawn_at_[si].second, Team::Posse, false, slot);
      if (id) w.script_toast(slot, "The " + player_design_.name + " is yours. W/S throttle, A/D strafe, Q/E turn, F fire.", 4.0, 0);
    }
    Ship* s = find(id);
    if (s) {
      const runtime::PlayerInput& pi = in.slot(slot);
      int turn = 0;
      if (pi.buttons & runtime::kButtonPrev) --turn;
      if (pi.buttons & runtime::kButtonAction) ++turn;
      s->in_x = pi.move_x;
      s->in_y = pi.move_y;
      s->turn = turn;
      s->fire = (pi.buttons & (runtime::kButtonUse | runtime::kButtonFire)) != 0;
    }
  }
}

void ShipSim::tick(runtime::World* w) {
  ++ticks_;
  if (w) riders(*w);
  captains();
  physics();
  guns();
  step_shots();
  reinforce();
  if (!w) return;
  // Pin each rider to its ship (the camera follows riders).
  for (int slot = 0; slot < kSlots; ++slot) {
    const runtime::Actor* cur = w->player(slot);
    if (!cur) continue;
    runtime::Actor* a = w->find(cur->data.id);
    const Ship* s = find(ship_of(slot));
    if (!a) continue;
    a->hidden = true;
    if (!s) continue;
    a->prev_x = s->prev_x - a->data.w * 0.5f;
    a->prev_y = s->prev_y - a->data.h * 0.5f;
    a->data.x = s->x - a->data.w * 0.5f;
    a->data.y = s->y - a->data.h * 0.5f;
    a->vx = s->vx;
    a->vy = s->vy;
  }
  if (w->net_role() != runtime::NetRole::Server) {
    for (Ship& s : ships_) s.layout_dirty = false;
    return;
  }
  for (Ship& s : ships_) {
    if (s.layout_dirty || full_all_ || ticks_ % 300 == 0) post(*w, -1, encode_full(s));
    s.layout_dirty = false;
  }
  full_all_ = false;
  if (ticks_ % kStateEvery == 0) post(*w, -1, encode_state());
}

void ShipSim::step(runtime::World& world) {
  if (!active_ || mirror_) return;
  tick(&world);
}

void ShipSim::step_bare() {
  if (!active_ || mirror_) return;
  tick(nullptr);
  for (Ship& s : ships_) s.layout_dirty = false;
}

std::uint64_t ShipSim::hash() const {
  std::uint64_t h = 1469598103934665603ull;
  for (const Ship& s : ships_) {
    fnv(&h, &s.id, sizeof(s.id));
    const float f[] = {s.x, s.y, s.vx, s.vy, s.angle, s.spin, s.shield};
    fnv(&h, f, sizeof(f));
    for (const LivePart& p : s.parts) {
      fnv(&h, &p.hp, sizeof(p.hp));
      fnv(&h, &p.aim, sizeof(p.aim));
    }
  }
  for (const Shot& sh : shots_) {
    const float f[] = {sh.x, sh.y, sh.vx, sh.vy};
    fnv(&h, f, sizeof(f));
  }
  return h;
}

// --- Wire ---------------------------------------------------------------------------------------

void ShipSim::post(runtime::World& w, int slot, std::vector<std::uint8_t> bytes) {
  w.net_post(kChannel, slot, std::move(bytes));
}

std::vector<std::uint8_t> ShipSim::encode_full(const Ship& s) const {
  Writer w;
  w.u8('F');
  w.u32(s.id);
  w.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(s.owner)));
  w.u8(static_cast<std::uint8_t>(s.team));
  w.u8(s.ai ? 1 : 0);
  w.str(s.name);
  w.u16(static_cast<std::uint16_t>(s.parts.size()));
  for (const LivePart& p : s.parts) {
    const PartDef* d = lib_.get(p.def);
    w.str(d ? d->id : std::string());
    w.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(p.x)));
    w.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(p.y)));
    w.u8(static_cast<std::uint8_t>(p.rot));
    w.u16(static_cast<std::uint16_t>(std::clamp(p.hp, 0, 65535)));
  }
  const float f[] = {s.x, s.y, s.vx, s.vy, s.angle, s.spin, s.shield};
  for (float v : f) w.f32(v);
  return std::move(w.b);
}

std::vector<std::uint8_t> ShipSim::encode_state() const {
  Writer w;
  w.u8('S');
  w.u32(static_cast<std::uint32_t>(ticks_));
  w.u16(static_cast<std::uint16_t>(ships_.size()));
  for (const Ship& s : ships_) {
    w.u32(s.id);
    const float f[] = {s.x, s.y, s.vx, s.vy, s.angle, s.spin, s.shield};
    for (float v : f) w.f32(v);
    w.u16(static_cast<std::uint16_t>(s.parts.size()));
    for (const LivePart& p : s.parts) {
      w.u16(static_cast<std::uint16_t>(std::clamp(p.hp, 0, 65535)));
      w.u8(static_cast<std::uint8_t>((p.powered ? 1 : 0) | (p.firing ? 2 : 0)));
      w.u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(wrap(p.aim) * 10000.0f)));
    }
  }
  w.u16(static_cast<std::uint16_t>(std::min<std::size_t>(shots_.size(), 65535)));
  for (std::size_t i = 0; i < shots_.size() && i < 65535; ++i) {
    const Shot& sh = shots_[i];
    w.f32(sh.x);
    w.f32(sh.y);
    w.f32(sh.vx);
    w.f32(sh.vy);
    w.u8(static_cast<std::uint8_t>(sh.team));
  }
  return std::move(w.b);
}

void ShipSim::receive(runtime::World& world, int from_slot, const std::vector<std::uint8_t>& bytes) {
  (void)from_slot;
  if (world.net_role() == runtime::NetRole::Client) {
    if (!mirror_) begin_client(lib_);
    apply(bytes);
  }
  // The server takes nothing from clients: helms ride on PlayerInput.
}

void ShipSim::apply(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes);
  const std::uint8_t kind = r.u8();
  if (kind == 'W') {
    my_slot_ = r.u8();
    return;
  }
  if (kind == 'F') {
    Ship s;
    s.id = r.u32();
    s.owner = static_cast<std::int8_t>(r.u8());
    s.team = static_cast<Team>(std::min<std::uint8_t>(r.u8(), 2));
    s.ai = r.u8() != 0;
    s.name = r.str();
    const std::uint16_t n = r.u16();
    for (std::uint16_t i = 0; i < n && !r.bad; ++i) {
      LivePart p;
      p.def = lib_.find(r.str());
      p.x = static_cast<std::int8_t>(r.u8());
      p.y = static_cast<std::int8_t>(r.u8());
      p.rot = r.u8() & 3;
      p.hp = r.u16();
      s.parts.push_back(p);
    }
    float* f[] = {&s.x, &s.y, &s.vx, &s.vy, &s.angle, &s.spin, &s.shield};
    for (float* v : f) *v = r.f32();
    if (r.bad) return;
    relayout(s);
    s.prev_x = s.x;
    s.prev_y = s.y;
    if (Ship* cur = find(s.id)) *cur = std::move(s);
    else ships_.push_back(std::move(s));
    return;
  }
  if (kind == 'S') {
    ticks_ = r.u32();
    const std::uint16_t n = r.u16();
    std::vector<std::uint32_t> seen;
    for (std::uint16_t k = 0; k < n && !r.bad; ++k) {
      const std::uint32_t id = r.u32();
      seen.push_back(id);
      Ship* s = find(id);
      Ship scratch;
      Ship& t = s ? *s : scratch;
      t.prev_x = t.x;
      t.prev_y = t.y;
      float* f[] = {&t.x, &t.y, &t.vx, &t.vy, &t.angle, &t.spin, &t.shield};
      for (float* v : f) *v = r.f32();
      const std::uint16_t np = r.u16();
      const bool match = s && np == s->parts.size();
      for (std::uint16_t i = 0; i < np && !r.bad; ++i) {
        const int hp = r.u16();
        const std::uint8_t fl = r.u8();
        const float aim = static_cast<float>(static_cast<std::int16_t>(r.u16())) / 10000.0f;
        if (!match) continue;
        LivePart& p = s->parts[i];
        p.hp = hp;
        p.powered = (fl & 1) != 0;
        p.firing = (fl & 2) != 0;
        p.aim = aim;
      }
    }
    std::vector<Shot> shots;
    const std::uint16_t ns = r.u16();
    for (std::uint16_t i = 0; i < ns && !r.bad; ++i) {
      Shot sh;
      sh.x = r.f32();
      sh.y = r.f32();
      sh.vx = r.f32();
      sh.vy = r.f32();
      sh.team = static_cast<Team>(std::min<std::uint8_t>(r.u8(), 2));
      shots.push_back(sh);
    }
    if (r.bad) return;
    shots_ = std::move(shots);
    ships_.erase(std::remove_if(ships_.begin(), ships_.end(),
                                [&](const Ship& s) {
                                  return std::find(seen.begin(), seen.end(), s.id) == seen.end();
                                }),
                 ships_.end());
    ++states_received_;
  }
}

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
