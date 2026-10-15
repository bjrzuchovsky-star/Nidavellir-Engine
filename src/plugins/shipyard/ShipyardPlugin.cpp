#include "plugins/shipyard/ShipyardPlugin.h"

#include "plugin/Catalog.h"
#include "runtime/World.h"

// Lua is built as C++ (cmake/FetchLua.cmake): no extern "C".
#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace shipyard {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.0f / kPi;

ShipyardPlugin*& current_ref() {
  static ShipyardPlugin* p = nullptr;
  return p;
}

ShipyardPlugin::EditorHook& hook_ref() {
  static ShipyardPlugin::EditorHook h;
  return h;
}

// --- Lua: the `ships` table ----------------------------------------------------------

ShipyardPlugin* self(lua_State* L) {
  return static_cast<ShipyardPlugin*>(lua_touserdata(L, lua_upvalueindex(1)));
}

bool sim_ready(ShipyardPlugin* p) { return p->sim().active() && !p->sim().mirror(); }

std::uint32_t arg_ship(lua_State* L, int idx) {
  return static_cast<std::uint32_t>(luaL_checkinteger(L, idx));
}

// ships.of(slot) -> id | nil (slots are 1-based like frontier's)
int l_of(lua_State* L) {
  const lua_Integer slot = luaL_checkinteger(L, 1) - 1;
  const std::uint32_t id = self(L)->sim().ship_of(static_cast<int>(slot));
  if (id) lua_pushinteger(L, id);
  else lua_pushnil(L);
  return 1;
}

// ships.list([team]) -> { id, ... }
int l_list(lua_State* L) {
  const char* team = luaL_optstring(L, 1, nullptr);
  lua_newtable(L);
  lua_Integer n = 0;
  for (const Ship& s : self(L)->sim().ships()) {
    if (team && std::string(team) != team_name(s.team)) continue;
    lua_pushinteger(L, s.id);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// ships.info(id) -> { name, team, x, y, vx, vy, angle (deg), parts, hp, max_hp,
//                     mass, shield, ai, slot } | nil
int l_info(lua_State* L) {
  ShipyardPlugin* p = self(L);
  const Ship* s = p->sim().find(arg_ship(L, 1));
  if (!s) {
    lua_pushnil(L);
    return 1;
  }
  int hp = 0, max_hp = 0;
  for (const LivePart& lp : s->parts) {
    hp += std::max(0, lp.hp);
    if (const PartDef* d = p->lib().get(lp.def)) max_hp += d->hp;
  }
  lua_newtable(L);
  auto num = [L](const char* k, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
  };
  lua_pushstring(L, s->name.c_str());
  lua_setfield(L, -2, "name");
  lua_pushstring(L, team_name(s->team));
  lua_setfield(L, -2, "team");
  num("x", s->x);
  num("y", s->y);
  num("vx", s->vx);
  num("vy", s->vy);
  num("angle", s->angle * kDeg);
  num("mass", s->mass);
  num("shield", s->shield);
  lua_pushinteger(L, static_cast<lua_Integer>(s->parts.size()));
  lua_setfield(L, -2, "parts");
  lua_pushinteger(L, hp);
  lua_setfield(L, -2, "hp");
  lua_pushinteger(L, max_hp);
  lua_setfield(L, -2, "max_hp");
  lua_pushboolean(L, s->ai);
  lua_setfield(L, -2, "ai");
  if (s->owner >= 0) {
    lua_pushinteger(L, s->owner + 1);
    lua_setfield(L, -2, "slot");
  }
  return 1;
}

// ships.spawn(design_path | "stock:sloop" | "stock:marauder", x, y, team="pirates") -> id | nil, why
int l_spawn(lua_State* L) {
  ShipyardPlugin* p = self(L);
  const char* path = luaL_checkstring(L, 1);
  const float x = static_cast<float>(luaL_checknumber(L, 2));
  const float y = static_cast<float>(luaL_checknumber(L, 3));
  const std::string team = luaL_optstring(L, 4, "pirates");
  Team t = Team::Pirates;
  if (team == "posse") t = Team::Posse;
  else if (team == "derelict") t = Team::Derelict;
  else if (team != "pirates") return luaL_error(L, "ships.spawn: team is pirates, posse or derelict");
  if (!sim_ready(p)) {
    lua_pushnil(L);
    lua_pushstring(L, "no ride in progress (or this is a client)");
    return 2;
  }
  std::uint32_t id = 0;
  std::string err;
  if (!p->sim().spawn_file(path, x, y, t, t == Team::Pirates, &id, &err)) {
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  lua_pushinteger(L, id);
  return 1;
}

// ships.damage(id, part (1-based), n) -> destroyed
int l_damage(lua_State* L) {
  ShipyardPlugin* p = self(L);
  const std::uint32_t id = arg_ship(L, 1);
  const int part = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
  const int n = static_cast<int>(luaL_checkinteger(L, 3));
  lua_pushboolean(L, sim_ready(p) && p->sim().damage_part(id, part, n));
  return 1;
}

// ships.remove(id) -> removed
int l_remove(lua_State* L) {
  ShipyardPlugin* p = self(L);
  lua_pushboolean(L, sim_ready(p) && p->sim().remove_ship(arg_ship(L, 1)));
  return 1;
}

// ships.set_ai(id, on)
int l_set_ai(lua_State* L) {
  ShipyardPlugin* p = self(L);
  if (Ship* s = sim_ready(p) ? p->sim().find(arg_ship(L, 1)) : nullptr) {
    s->ai = lua_toboolean(L, 2) != 0;
    if (!s->ai) p->sim().set_helm(s->id, 0, 0, 0, false);
  }
  return 0;
}

// ships.helm(id, strafe, throttle, turn, fire) -- for ships without a captain
int l_helm(lua_State* L) {
  ShipyardPlugin* p = self(L);
  if (!sim_ready(p)) return 0;
  p->sim().set_helm(arg_ship(L, 1), static_cast<float>(luaL_optnumber(L, 2, 0)),
                    static_cast<float>(luaL_optnumber(L, 3, 0)),
                    static_cast<int>(luaL_optinteger(L, 4, 0)), lua_toboolean(L, 5) != 0);
  return 0;
}

// ships.stats() -> parts_destroyed, splits, shots_fired
int l_stats(lua_State* L) {
  const ShipSim& s = self(L)->sim();
  lua_pushinteger(L, static_cast<lua_Integer>(s.parts_destroyed()));
  lua_pushinteger(L, static_cast<lua_Integer>(s.splits()));
  lua_pushinteger(L, static_cast<lua_Integer>(s.shots_fired()));
  return 3;
}

// --- Drawing --------------------------------------------------------------------------------

void quad(std::vector<runtime::DrawQuad>* out, float cx, float cy, float w, float h, float deg,
          float r, float g, float b, float a = 1.0f) {
  runtime::DrawQuad q;
  q.x0 = cx - w * 0.5f;
  q.y0 = cy - h * 0.5f;
  q.x1 = cx + w * 0.5f;
  q.y1 = cy + h * 0.5f;
  q.rgba[0] = std::min(1.0f, r);
  q.rgba[1] = std::min(1.0f, g);
  q.rgba[2] = std::min(1.0f, b);
  q.rgba[3] = a;
  q.rotation = deg;
  out->push_back(q);
}

std::uint32_t mix(int x, int y) {
  std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return h;
}

}  // namespace

// --- Plugin ---------------------------------------------------------------------------------

const plugin::PluginInfo& ShipyardPlugin::static_info() {
  static const plugin::PluginInfo info = [] {
    plugin::PluginInfo i;
    i.id = "ts.shipyard";
    i.name = "Airship Works";
    i.description =
        "Cosmoteer-style airships for 2D projects: design ships part by part "
        "(hull, armor, engines, boilers, bunks, guns, shields, cargo), then fly "
        "them with thrust physics, trade shot with sky pirates and watch "
        "wrecks break away. Posse-ready.";
    i.author = "TombStone";
    i.version = plugin::Version{1, 0, 0};
    i.category = plugin::PluginCategory::Gameplay;
    i.project_types = plugin::kType2D;
    i.default_enabled = false;
    return i;
  }();
  return info;
}

void ShipyardPlugin::set_editor_hook(EditorHook hook) { hook_ref() = std::move(hook); }

ShipyardPlugin* ShipyardPlugin::current() { return current_ref(); }

bool ShipyardPlugin::reload_data(std::string* error) {
  PartLibrary lib;
  std::vector<std::string> warnings;
  if (!lib.load(project_dir_, error, &warnings)) return false;
  SkySettings sky;
  const std::filesystem::path p = std::filesystem::path(shipyard_dir(project_dir_)) / "sky.json";
  std::ifstream in(p, std::ios::binary);
  if (in && !project_dir_.empty()) {
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string err;
    if (!sky.parse(ss.str(), &err)) warnings.push_back(err + " (default encounter)");
  } else {
    sky.enemies.push_back(EnemySpawn{"stock:marauder", 520.0f, -420.0f});
  }
  lib_ = std::move(lib);
  sky_ = std::move(sky);
  warnings_ = std::move(warnings);
  return true;
}

bool ShipyardPlugin::on_load(const plugin::PluginContext& ctx, std::string* error) {
  project_dir_ = ctx.project_dir;
  host_ = ctx.host;
  if (!reload_data(error)) return false;
  current_ref() = this;
  return true;
}

void ShipyardPlugin::on_register(plugin::Registrar& reg) {
  reg.add_component("Airship", "Airship Works: every rider takes the helm of a ship");
  reg.add_system("shipyard.sim", [this](runtime::World& w, double) {
    if (w.net_role() != runtime::NetRole::Client) sim_.step(w);
  });
  // Thruster plumes: every peer puffs its own from the (mirrored) engines.
  reg.add_system("shipyard.thrusters", [this](runtime::World& w, double) {
    if (w.tick() % 3 != 0) return;
    for (const Ship& s : sim_.ships()) {
      const float c = std::cos(s.angle), sn = std::sin(s.angle);
      for (const LivePart& p : s.parts) {
        if (!p.firing) continue;
        const PartDef* d = sim_.lib().get(p.def);
        if (!d || d->kind != PartKind::Engine) continue;
        int fw = 1, fh = 1;
        footprint(*d, p.rot, &fw, &fh);
        float gx = 0, gy = 0, wx = 0, wy = 0;
        part_center(*d, PartPose{p.def, p.x, p.y, p.rot}, &gx, &gy);
        s.to_world(gx, gy, &wx, &wy);
        float ex = 0, ey = 0;
        engine_dir(p.rot, &ex, &ey);
        const float bx = c * -ex - sn * -ey, by = sn * -ex + c * -ey;
        const float len = (ex != 0.0f ? fw * kCell : fh * kCell) * 0.5f;
        runtime::FxEvent ev;
        ev.preset = "thruster";
        ev.local = true;
        ev.count = 2;
        ev.x = wx + bx * (len + 4);
        ev.y = wy + by * (len + 4);
        // The preset points down (+y on screen); turn it onto the nozzle.
        ev.dir = std::atan2(by, bx) * 57.2957795f - 90.0f;
        w.fx(std::move(ev));
      }
    }
  });
  reg.add_draw_layer("shipyard.sky", false,
                     [this](const runtime::World& w, const runtime::WorldRect& view, float,
                            std::vector<runtime::DrawQuad>* out) { draw_sky(w, view, out); });
  reg.add_draw_layer("shipyard.ships", true,
                     [this](const runtime::World& w, const runtime::WorldRect& view, float alpha,
                            std::vector<runtime::DrawQuad>* out) { draw_ships(w, view, alpha, out); });
  reg.add_net_channel(kChannel, [this](runtime::World& w, int from, const std::vector<std::uint8_t>& b) {
    if (w.net_role() == runtime::NetRole::Client && !sim_.mirror()) {
      sim_.begin_client(lib_);
      world_ = &w;
    }
    sim_.receive(w, from, b);
  });
  reg.add_lua_api("ships", [this](lua_State* L) {
    lua_newtable(L);
    const luaL_Reg fns[] = {{"of", &l_of},         {"list", &l_list},     {"info", &l_info},
                            {"spawn", &l_spawn},   {"damage", &l_damage}, {"remove", &l_remove},
                            {"set_ai", &l_set_ai}, {"helm", &l_helm},     {"stats", &l_stats},
                            {nullptr, nullptr}};
    lua_pushlightuserdata(L, this);
    luaL_setfuncs(L, fns, 1);
  });
  reg.add_asset_type({".ship.json", "Airship design"});
  if (hook_ref() && host_ == "editor2d") hook_ref()(*this, reg);
}

void ShipyardPlugin::on_play_begin(runtime::World& world) {
  world_ = &world;
  if (world.net_role() == runtime::NetRole::Client) {
    sim_.begin_client(lib_);
    return;
  }
  sim_.begin(&world, project_dir_, lib_, sky_);
  for (const std::string& w : warnings_) world.script_toast(0, w, 6.0, 0);
  for (const std::string& w : sim_.warnings()) world.script_toast(0, w, 6.0, 0);
}

void ShipyardPlugin::on_play_end() {
  sim_.end();
  world_ = nullptr;
}

void ShipyardPlugin::on_unload() {
  if (sim_.active()) sim_.end();
  world_ = nullptr;
  if (current_ref() == this) current_ref() = nullptr;
}

void ShipyardPlugin::draw_sky(const runtime::World&, const runtime::WorldRect& view,
                              std::vector<runtime::DrawQuad>* out) const {
  if (!sim_.active()) return;
  const float vw = view.x1 - view.x0, vh = view.y1 - view.y0;
  // Dusk sky: two bands.
  quad(out, view.x0 + vw * 0.5f, view.y0 + vh * 0.5f, vw + 4, vh + 4, 0, 0.36f, 0.30f, 0.42f);
  quad(out, view.x0 + vw * 0.5f, view.y0 + vh * 0.8f, vw + 4, vh * 0.4f + 4, 0, 0.62f, 0.42f, 0.36f, 0.35f);
  // Clouds on a 320 px lattice (fixed in the world, so motion reads).
  constexpr float kLattice = 320.0f;
  const int x0 = static_cast<int>(std::floor(view.x0 / kLattice)) - 1;
  const int y0 = static_cast<int>(std::floor(view.y0 / kLattice)) - 1;
  const int x1 = static_cast<int>(std::floor(view.x1 / kLattice)) + 1;
  const int y1 = static_cast<int>(std::floor(view.y1 / kLattice)) + 1;
  if ((x1 - x0) * (y1 - y0) > 900) return;
  for (int gy = y0; gy <= y1; ++gy) {
    for (int gx = x0; gx <= x1; ++gx) {
      const std::uint32_t h = mix(gx, gy);
      if (h % 3u != 0u) continue;
      const float cx = (static_cast<float>(gx) + static_cast<float>(h % 97u) / 97.0f) * kLattice;
      const float cy = (static_cast<float>(gy) + static_cast<float>((h >> 8) % 89u) / 89.0f) * kLattice;
      const float s = 50.0f + static_cast<float>((h >> 16) % 60u);
      quad(out, cx, cy, s * 2.2f, s * 0.7f, 0, 0.93f, 0.80f, 0.74f, 0.22f);
      quad(out, cx - s * 0.4f, cy - s * 0.25f, s * 1.1f, s * 0.6f, 0, 0.98f, 0.88f, 0.80f, 0.25f);
      quad(out, cx + s * 0.5f, cy - s * 0.1f, s * 0.9f, s * 0.5f, 0, 0.98f, 0.86f, 0.78f, 0.2f);
    }
  }
}

void ShipyardPlugin::draw_ships(const runtime::World& w, const runtime::WorldRect& view, float alpha,
                                std::vector<runtime::DrawQuad>* out) const {
  if (!sim_.active()) return;
  const bool client = w.net_role() == runtime::NetRole::Client;
  const float a = client ? 1.0f : std::clamp(alpha, 0.0f, 1.0f);
  for (const Ship& real : sim_.ships()) {
    Ship s = real;  // interpolated pose
    s.x = real.prev_x + (real.x - real.prev_x) * a;
    s.y = real.prev_y + (real.y - real.prev_y) * a;
    const float r = s.radius + 24.0f;
    if (s.x + r < view.x0 || s.x - r > view.x1 || s.y + r < view.y0 || s.y - r > view.y1) continue;
    const float deg = s.angle * kDeg;
    float trim[3] = {0.35f, 0.33f, 0.32f};
    if (s.team == Team::Posse) {
      trim[0] = 0.95f; trim[1] = 0.72f; trim[2] = 0.32f;
    } else if (s.team == Team::Pirates) {
      trim[0] = 0.62f; trim[1] = 0.12f; trim[2] = 0.12f;
    }
    // Shadow on the clouds.
    for (const LivePart& p : s.parts) {
      const PartDef* d = sim_.lib().get(p.def);
      if (!d) continue;
      int fw = 1, fh = 1;
      footprint(*d, p.rot, &fw, &fh);
      float gx = 0, gy = 0, wx = 0, wy = 0;
      part_center(*d, PartPose{p.def, p.x, p.y, p.rot}, &gx, &gy);
      s.to_world(gx, gy, &wx, &wy);
      quad(out, wx + 10, wy + 14, fw * kCell, fh * kCell, deg, 0.1f, 0.06f, 0.12f, 0.25f);
    }
    // Trim (a team-coloured rim) then the parts.
    for (const LivePart& p : s.parts) {
      const PartDef* d = sim_.lib().get(p.def);
      if (!d) continue;
      int fw = 1, fh = 1;
      footprint(*d, p.rot, &fw, &fh);
      float gx = 0, gy = 0, wx = 0, wy = 0;
      part_center(*d, PartPose{p.def, p.x, p.y, p.rot}, &gx, &gy);
      s.to_world(gx, gy, &wx, &wy);
      quad(out, wx, wy, fw * kCell + 3, fh * kCell + 3, deg, trim[0], trim[1], trim[2]);
    }
    for (const LivePart& p : s.parts) {
      const PartDef* d = sim_.lib().get(p.def);
      if (!d) continue;
      int fw = 1, fh = 1;
      footprint(*d, p.rot, &fw, &fh);
      float gx = 0, gy = 0, wx = 0, wy = 0;
      part_center(*d, PartPose{p.def, p.x, p.y, p.rot}, &gx, &gy);
      s.to_world(gx, gy, &wx, &wy);
      const float health = std::clamp(static_cast<float>(p.hp) / static_cast<float>(d->hp), 0.0f, 1.0f);
      float shade = (0.55f + 0.45f * health) * (p.powered ? 1.0f : 0.6f);
      if (s.hit_flash > 0.0f) shade *= 1.25f;
      const float pw = fw * kCell - 1.0f, ph = fh * kCell - 1.0f;
      quad(out, wx, wy, pw, ph, deg, d->color[0] * shade, d->color[1] * shade, d->color[2] * shade);
      const float c = std::cos(s.angle), sn = std::sin(s.angle);
      switch (d->kind) {
        case PartKind::Engine: {
          float ex = 0, ey = 0;
          engine_dir(p.rot, &ex, &ey);
          // Nozzle at the back (opposite the push).
          const float bx = c * -ex - sn * -ey, by = sn * -ex + c * -ey;
          const float len = (ex != 0.0f ? pw : ph) * 0.5f;
          quad(out, wx + bx * (len - 3), wy + by * (len - 3), 8, 8, deg, 0.18f, 0.16f, 0.16f);
          if (p.firing) {
            quad(out, wx + bx * (len + 7), wy + by * (len + 7), 9, 9, deg + 45, 1.0f, 0.62f, 0.2f, 0.9f);
            quad(out, wx + bx * (len + 14), wy + by * (len + 14), 6, 6, deg + 45, 1.0f, 0.9f, 0.5f, 0.7f);
          }
          break;
        }
        case PartKind::Reactor:
          quad(out, wx, wy, pw * 0.45f, ph * 0.45f, deg, 1.0f, 0.55f, 0.18f);
          quad(out, wx, wy, pw * 0.22f, ph * 0.22f, deg + 45, 1.0f, 0.9f, 0.45f);
          break;
        case PartKind::Shield:
          quad(out, wx, wy, pw * 0.4f, ph * 0.7f, deg, 0.55f, 0.85f, 1.0f, p.powered ? 1.0f : 0.4f);
          break;
        case PartKind::Quarters:
          quad(out, wx, wy, pw * 0.7f, ph * 0.35f, deg, 0.85f, 0.75f, 0.6f);
          break;
        case PartKind::Storage:
          quad(out, wx, wy, pw * 0.75f, ph * 0.75f, deg, d->color[0] * 0.75f, d->color[1] * 0.75f, d->color[2] * 0.75f);
          quad(out, wx, wy, pw * 0.75f, 2, deg + 45, 0.3f, 0.22f, 0.12f);
          break;
        case PartKind::Armor:
          quad(out, wx, wy, 3, 3, deg, 0.75f, 0.75f, 0.78f);
          break;
        case PartKind::Gun: {
          const float len = std::max(pw, ph) * 0.8f;
          const float bw = d->damage >= 30 ? 6.0f : 4.0f;
          const float ax = std::sin(p.aim), ay = -std::cos(p.aim);
          quad(out, wx, wy, pw * 0.6f, ph * 0.6f, deg, 0.42f, 0.40f, 0.42f);
          quad(out, wx + ax * len * 0.5f, wy + ay * len * 0.5f, bw, len, p.aim * kDeg, 0.12f, 0.12f, 0.13f);
          if (p.firing) quad(out, wx + ax * len, wy + ay * len, 8, 8, p.aim * kDeg + 45, 1.0f, 0.85f, 0.4f);
          break;
        }
        default:
          break;
      }
    }
    if (s.shield > 0.0f && s.shield_max > 0.0f) {
      const float f = s.shield / s.shield_max;
      const float d = (s.radius + 10.0f) * 2.0f;
      quad(out, s.x, s.y, d, d, deg, 0.45f, 0.8f, 1.0f, 0.05f + 0.07f * f);
      quad(out, s.x, s.y, d, d, deg + 45, 0.45f, 0.8f, 1.0f, 0.05f + 0.07f * f);
    }
    if (s.team != Team::Derelict) {
      int hp = 0, max_hp = 0;
      for (const LivePart& p : s.parts) {
        hp += std::max(0, p.hp);
        if (const PartDef* d = sim_.lib().get(p.def)) max_hp += d->hp;
      }
      const float f = max_hp > 0 ? static_cast<float>(hp) / static_cast<float>(max_hp) : 0.0f;
      const float top = s.y - s.radius - 14.0f;
      quad(out, s.x, top, 48, 5, 0, 0.12f, 0.07f, 0.06f, 0.85f);
      quad(out, s.x - 24 + 24 * f, top, 48 * f, 5, 0, trim[0], trim[1], trim[2], 0.95f);
      if (s.shield_max > 0.0f) {
        const float g = s.shield / s.shield_max;
        quad(out, s.x - 24 + 24 * g, top + 5, 48 * g, 3, 0, 0.5f, 0.85f, 1.0f, 0.9f);
      }
    }
  }
  for (const Shot& sh : sim_.shots()) {
    if (sh.x < view.x0 || sh.x > view.x1 || sh.y < view.y0 || sh.y > view.y1) continue;
    const float deg = std::atan2(sh.vx, -sh.vy) * kDeg;
    if (sh.team == Team::Posse) quad(out, sh.x, sh.y, 3, 9, deg, 1.0f, 0.88f, 0.45f);
    else quad(out, sh.x, sh.y, 4, 9, deg, 1.0f, 0.4f, 0.3f);
  }
  // Helm gauges for this host's own ship (bottom left of the view).
  const int me = client ? sim_.my_slot() : 0;
  const Ship* mine = sim_.find(sim_.ship_of(me));
  const float vw = view.x1 - view.x0, vh = view.y1 - view.y0;
  const float u = std::clamp(vw / 60.0f, 8.0f, 24.0f);
  const float gx = view.x0 + u * 6.5f, gy = view.y0 + vh - u * 2.0f;
  quad(out, gx, gy, u * 12.0f, u * 2.4f, 0, 0.10f, 0.07f, 0.09f, 0.8f);
  if (!mine) {
    quad(out, gx, gy, u * 11.0f, u * 0.5f, 0, 0.6f, 0.2f, 0.15f, 0.9f);
    return;
  }
  int hp = 0, max_hp = 0;
  float made = 0, used = 0;
  for (const LivePart& p : mine->parts) {
    const PartDef* d = sim_.lib().get(p.def);
    if (!d) continue;
    hp += std::max(0, p.hp);
    max_hp += d->hp;
    if (d->power > 0) made += d->power;
    else if (p.powered) used -= d->power;
  }
  const float bars[3] = {max_hp ? static_cast<float>(hp) / static_cast<float>(max_hp) : 0.0f,
                         mine->shield_max > 0 ? mine->shield / mine->shield_max : 0.0f,
                         made > 0 ? std::min(1.0f, used / made) : 0.0f};
  const float colors[3][3] = {{0.95f, 0.72f, 0.32f}, {0.5f, 0.85f, 1.0f}, {0.9f, 0.5f, 0.25f}};
  for (int i = 0; i < 3; ++i) {
    const float y = gy - u * 0.75f + static_cast<float>(i) * u * 0.75f;
    quad(out, gx, y, u * 11.0f, u * 0.45f, 0, 0.2f, 0.15f, 0.16f, 0.9f);
    quad(out, gx - u * 5.5f + u * 5.5f * bars[i], y, u * 11.0f * bars[i], u * 0.45f, 0,
         colors[i][0], colors[i][1], colors[i][2], 0.95f);
  }
}

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(shipyard, ::ts::tombstone::shipyard::ShipyardPlugin)
