#include "plugins/survival/SurvivalPlugin.h"

#include "plugin/Catalog.h"
#include "runtime/Collision.h"
#include "runtime/World.h"

// Lua is built as C++ (cmake/FetchLua.cmake): no extern "C".
#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace survival {

namespace {

SurvivalPlugin*& current_ref() {
  static SurvivalPlugin* p = nullptr;
  return p;
}

SurvivalPlugin::EditorHook& hook_ref() {
  static SurvivalPlugin::EditorHook h;
  return h;
}

// --- Lua: the `frontier` table ----------------------------------------------------

SurvivalPlugin* self(lua_State* L) {
  return static_cast<SurvivalPlugin*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int arg_slot(lua_State* L, int idx) {
  const lua_Integer s = luaL_checkinteger(L, idx) - 1;  // Lua slots are 1-based
  if (s < 0 || s >= FrontierSim::kSlots) {
    luaL_error(L, "frontier: rider slot %d out of range (1..%d)", static_cast<int>(s + 1),
               FrontierSim::kSlots);
  }
  return static_cast<int>(s);
}

int arg_item(lua_State* L, SurvivalPlugin* p, int idx) {
  const char* id = luaL_checkstring(L, idx);
  const int i = p->sim().data().item(id);
  if (i < 0) luaL_error(L, "frontier: no item '%s' (see survival/items.json)", id);
  return i;
}

bool sim_ready(SurvivalPlugin* p) { return p->sim().active() && !p->sim().mirror(); }

// frontier.give(slot, item_id, n=1) -> leftover
int l_give(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  const int item = arg_item(L, p, 2);
  const int n = static_cast<int>(luaL_optinteger(L, 3, 1));
  lua_pushinteger(L, sim_ready(p) ? p->sim().give(s, item, n) : n);
  return 1;
}

// frontier.take(slot, item_id, n=1) -> taken
int l_take(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  const int item = arg_item(L, p, 2);
  const int n = static_cast<int>(luaL_optinteger(L, 3, 1));
  lua_pushinteger(L, sim_ready(p) ? p->sim().take(s, item, n) : 0);
  return 1;
}

// frontier.count(slot, item_id) -> n
int l_count(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  const int item = arg_item(L, p, 2);
  lua_pushinteger(L, p->sim().player(s).inv.count(item));
  return 1;
}

// frontier.health(slot) -> hp, max
int l_health(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  lua_pushinteger(L, p->sim().player(s).hp);
  lua_pushinteger(L, p->sim().player(s).max_hp);
  return 2;
}

// frontier.hurt(slot, n, by="a stray bullet")
int l_hurt(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  const int n = static_cast<int>(luaL_checkinteger(L, 2));
  const char* by = luaL_optstring(L, 3, "a stray bullet");
  if (sim_ready(p) && p->world()) p->sim().hurt_player(*p->world(), s, n, by);
  return 0;
}

// frontier.heal(slot, n)
int l_heal(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  if (sim_ready(p)) p->sim().heal_player(s, static_cast<int>(luaL_checkinteger(L, 2)));
  return 0;
}

// frontier.tile(tx, ty) -> ground_id, object_id|nil
int l_tile(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const Tile t = p->sim().tile(static_cast<int>(luaL_checkinteger(L, 1)),
                               static_cast<int>(luaL_checkinteger(L, 2)));
  lua_pushstring(L, ground_def(t.ground).id);
  if (t.object == kObjNone) lua_pushnil(L);
  else lua_pushstring(L, object_def(t.object).id);
  return 2;
}

// frontier.set_tile(tx, ty, ground_id|nil, object_id|nil|"") -> changed
int l_set_tile(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int tx = static_cast<int>(luaL_checkinteger(L, 1));
  const int ty = static_cast<int>(luaL_checkinteger(L, 2));
  Tile t = p->sim().tile(tx, ty);
  if (!lua_isnoneornil(L, 3)) {
    const char* g = luaL_checkstring(L, 3);
    if (!ground_by_id(g, &t.ground)) return luaL_error(L, "frontier: no ground '%s'", g);
  }
  if (!lua_isnone(L, 4)) {
    if (lua_isnil(L, 4)) {
      t.object = kObjNone;
    } else {
      const char* o = luaL_checkstring(L, 4);
      if (*o == '\0') t.object = kObjNone;
      else if (!object_by_id(o, &t.object)) return luaL_error(L, "frontier: no object '%s'", o);
    }
    t.damage = 0;
  }
  lua_pushboolean(L, sim_ready(p) && p->sim().set_tile(tx, ty, t));
  return 1;
}

// frontier.time() -> phase 0..1, is_night
int l_time(lua_State* L) {
  SurvivalPlugin* p = self(L);
  lua_pushnumber(L, p->sim().phase());
  lua_pushboolean(L, p->sim().night());
  return 2;
}

// frontier.spawn(creature_id, x, y) -> id (0 when unknown)
int l_spawn(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const char* id = luaL_checkstring(L, 1);
  const int c = p->sim().data().creature(id);
  if (c < 0) return luaL_error(L, "frontier: no creature '%s'", id);
  const float x = static_cast<float>(luaL_checknumber(L, 2));
  const float y = static_cast<float>(luaL_checknumber(L, 3));
  lua_pushinteger(L, sim_ready(p) ? static_cast<lua_Integer>(p->sim().spawn(c, x, y)) : 0);
  return 1;
}

// frontier.craft(slot, recipe_id) -> ok, why
int l_craft(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  const char* id = luaL_checkstring(L, 2);
  const int r = p->sim().data().recipe(id);
  std::string why = "no such recipe";
  const bool ok = r >= 0 && sim_ready(p) && p->world() && p->sim().craft(*p->world(), s, r, &why);
  lua_pushboolean(L, ok);
  if (ok) lua_pushnil(L);
  else lua_pushstring(L, why.c_str());
  return 2;
}

// frontier.seed() -> world seed
int l_seed(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(self(L)->sim().chunks().seed()));
  return 1;
}

// frontier.selected(slot) -> item_id|nil, count
int l_selected(lua_State* L) {
  SurvivalPlugin* p = self(L);
  const int s = arg_slot(L, 1);
  const FrontierSim::Player& pl = p->sim().player(s);
  const Stack& st = pl.inv.at(pl.hotbar);
  const ItemDef* d = st.empty() ? nullptr : p->sim().data().item_def(st.item);
  if (d) lua_pushstring(L, d->id.c_str());
  else lua_pushnil(L);
  lua_pushinteger(L, d ? st.count : 0);
  return 2;
}

// --- Drawing helpers --------------------------------------------------------------------

void quad(std::vector<runtime::DrawQuad>* out, float x0, float y0, float x1, float y1, float r,
          float g, float b, float a = 1.0f) {
  runtime::DrawQuad q;
  q.x0 = x0;
  q.y0 = y0;
  q.x1 = x1;
  q.y1 = y1;
  q.rgba[0] = r;
  q.rgba[1] = g;
  q.rgba[2] = b;
  q.rgba[3] = a;
  out->push_back(q);
}

void quadc(std::vector<runtime::DrawQuad>* out, float x0, float y0, float x1, float y1,
           const float* c, float shade = 1.0f, float a = 1.0f) {
  quad(out, x0, y0, x1, y1, c[0] * shade, c[1] * shade, c[2] * shade, a);
}

// Cheap per-tile variation so the ground doesn't look like a spreadsheet.
float speckle(int tx, int ty) {
  std::uint32_t h = static_cast<std::uint32_t>(tx) * 73856093u ^ static_cast<std::uint32_t>(ty) * 19349663u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return 0.94f + static_cast<float>(h % 100u) / 100.0f * 0.1f;
}

void draw_object(std::vector<runtime::DrawQuad>* out, std::uint8_t obj, float x, float y,
                 int damage) {
  const ObjectDef& d = object_def(obj);
  const float t = kTile;
  const float* c = d.color;
  switch (obj) {
    case kTree:
      quad(out, x + 13, y + 18, x + 19, y + 31, 0.42f, 0.28f, 0.16f);
      quadc(out, x + 3, y + 1, x + 29, y + 21, c);
      quadc(out, x + 8, y + 4, x + 20, y + 12, c, 1.18f);
      break;
    case kCactus:
      quadc(out, x + 12, y + 3, x + 20, y + 31, c);
      quadc(out, x + 5, y + 10, x + 12, y + 14, c);
      quadc(out, x + 5, y + 6, x + 9, y + 14, c);
      quadc(out, x + 20, y + 14, x + 27, y + 18, c);
      quadc(out, x + 23, y + 9, x + 27, y + 18, c);
      break;
    case kBush:
      quadc(out, x + 6, y + 12, x + 26, y + 28, c);
      quad(out, x + 11, y + 16, x + 14, y + 19, 0.8f, 0.2f, 0.2f);
      quad(out, x + 18, y + 20, x + 21, y + 23, 0.8f, 0.2f, 0.2f);
      break;
    case kCopperOre:
    case kIronOre:
    case kGoldOre: {
      const float rock[3] = {0.45f, 0.38f, 0.34f};
      quadc(out, x + 1, y + 1, x + t - 1, y + t - 1, rock);
      quadc(out, x + 6, y + 7, x + 12, y + 12, c);
      quadc(out, x + 18, y + 5, x + 24, y + 10, c);
      quadc(out, x + 12, y + 19, x + 19, y + 25, c);
      break;
    }
    case kTorch:
      quad(out, x + 14, y + 12, x + 18, y + 28, 0.45f, 0.3f, 0.15f);
      quadc(out, x + 12, y + 6, x + 20, y + 13, c);
      break;
    case kCampfire:
      quad(out, x + 5, y + 22, x + 27, y + 27, 0.4f, 0.26f, 0.14f);
      quadc(out, x + 10, y + 10, x + 22, y + 23, c);
      quad(out, x + 13, y + 14, x + 19, y + 22, 1.0f, 0.9f, 0.4f);
      break;
    case kDoor:
      quadc(out, x + 2, y + 0, x + t - 2, y + t, c);
      quad(out, x + 22, y + 15, x + 25, y + 18, 0.2f, 0.15f, 0.1f);
      break;
    case kBed:
      quadc(out, x + 3, y + 4, x + t - 3, y + t - 2, c);
      quad(out, x + 5, y + 6, x + t - 5, y + 13, 0.92f, 0.88f, 0.8f);
      break;
    case kWorkbench:
      quadc(out, x + 2, y + 8, x + t - 2, y + 18, c);
      quadc(out, x + 4, y + 18, x + 8, y + 30, c, 0.7f);
      quadc(out, x + t - 8, y + 18, x + t - 4, y + 30, c, 0.7f);
      break;
    case kForge:
      quadc(out, x + 2, y + 4, x + t - 2, y + t - 2, c);
      quad(out, x + 9, y + 14, x + 23, y + 24, 1.0f, 0.5f, 0.15f);
      break;
    default:
      quadc(out, x + 1, y + 1, x + t - 1, y + t - 1, c);
      quadc(out, x + 4, y + 3, x + t - 8, y + 9, c, 1.15f);
      break;
  }
  if (damage > 0 && d.hardness > 0) {
    const float f = std::min(1.0f, static_cast<float>(damage) / static_cast<float>(d.hardness));
    quad(out, x + 3, y + t - 5, x + 3 + (t - 6) * (1.0f - f), y + t - 2, 0.95f, 0.85f, 0.4f);
  }
}

}  // namespace

// --- Plugin ------------------------------------------------------------------------------

const plugin::PluginInfo& SurvivalPlugin::static_info() {
  static const plugin::PluginInfo info = [] {
    plugin::PluginInfo i;
    i.id = "ts.frontier";
    i.name = "Frontier Survival";
    i.description =
        "Necesse-style survival for 2D projects: an endless seeded frontier "
        "(prairie, desert, badlands, caves), mining and building, saddlebags "
        "and crafting, critters and bandits by night, settlers.";
    i.author = "TombStone";
    i.version = plugin::Version{1, 0, 0};
    i.category = plugin::PluginCategory::Gameplay;
    i.project_types = plugin::kType2D;
    i.default_enabled = false;
    return i;
  }();
  return info;
}

void SurvivalPlugin::set_editor_hook(EditorHook hook) { hook_ref() = std::move(hook); }

SurvivalPlugin* SurvivalPlugin::current() { return current_ref(); }

bool SurvivalPlugin::reload_data(std::string* error) {
  GameData d;
  std::vector<std::string> warnings;
  if (!d.load(project_dir_, error, &warnings)) return false;
  data_ = std::move(d);
  data_warnings_ = std::move(warnings);
  return true;
}

bool SurvivalPlugin::on_load(const plugin::PluginContext& ctx, std::string* error) {
  project_dir_ = ctx.project_dir;
  host_ = ctx.host;
  saving_ = ctx.host != "smoke" && !project_dir_.empty();
  if (!reload_data(error)) return false;
  current_ref() = this;
  return true;
}

void SurvivalPlugin::on_register(plugin::Registrar& reg) {
  reg.add_component("FrontierRider", "Frontier Survival: every rider gets saddlebags and grit");
  reg.add_system("frontier.sim", [this](runtime::World& w, double) {
    if (w.net_role() != runtime::NetRole::Client) sim_.step(w);
  });
  reg.add_draw_layer("frontier.ground", false,
                     [this](const runtime::World& w, const runtime::WorldRect& view, float,
                            std::vector<runtime::DrawQuad>* out) { draw_under(w, view, out); });
  reg.add_draw_layer("frontier.sky", true,
                     [this](const runtime::World& w, const runtime::WorldRect& view, float,
                            std::vector<runtime::DrawQuad>* out) { draw_over(w, view, out); });
  reg.add_blocker("frontier.tiles", [this](const runtime::World&, const runtime::Aabb& area,
                                           std::vector<runtime::Aabb>* out) {
    if (!sim_.has_seed()) return;
    const int tx0 = static_cast<int>(std::floor(area.x0 / kTile));
    const int ty0 = static_cast<int>(std::floor(area.y0 / kTile));
    const int tx1 = static_cast<int>(std::floor(area.x1 / kTile));
    const int ty1 = static_cast<int>(std::floor(area.y1 / kTile));
    for (int ty = ty0; ty <= ty1; ++ty) {
      for (int tx = tx0; tx <= tx1; ++tx) {
        const Tile* t = sim_.chunks().peek(tx, ty);
        if (t && tile_solid(*t)) {
          out->push_back({tx * kTile, ty * kTile, (tx + 1) * kTile, (ty + 1) * kTile});
        }
      }
    }
  });
  reg.add_net_channel(kChannel, [this](runtime::World& w, int from, const std::vector<std::uint8_t>& b) {
    if (w.net_role() == runtime::NetRole::Client) ensure_client(w);
    sim_.receive(w, from, b);
  });
  reg.add_lua_api("frontier", [this](lua_State* L) {
    lua_newtable(L);
    const luaL_Reg fns[] = {{"give", &l_give},         {"take", &l_take},
                            {"count", &l_count},       {"health", &l_health},
                            {"hurt", &l_hurt},         {"heal", &l_heal},
                            {"tile", &l_tile},         {"set_tile", &l_set_tile},
                            {"time", &l_time},         {"spawn", &l_spawn},
                            {"craft", &l_craft},       {"seed", &l_seed},
                            {"selected", &l_selected}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, this);
    luaL_setfuncs(L, fns, 1);
  });
  reg.add_asset_type({".frontier.json", "Frontier Survival data"});
  if (hook_ref() && host_ == "editor2d") hook_ref()(*this, reg);
}

void SurvivalPlugin::on_play_begin(runtime::World& world) {
  world_ = &world;
  if (world.net_role() == runtime::NetRole::Client) {
    sim_.begin_client(data_);
    return;
  }
  std::string err;
  if (!sim_.begin(world, project_dir_, data_, saving_, &err)) {
    world.script_toast(0, "Frontier Survival: " + err, 6.0, 0);
    // Ride on unsaved rather than not at all.
    sim_.begin(world, project_dir_, data_, false, nullptr);
  }
  for (const std::string& w : sim_.warnings()) world.script_toast(0, w, 6.0, 0);
}

void SurvivalPlugin::on_play_end() {
  sim_.end();
  world_ = nullptr;
}

void SurvivalPlugin::on_unload() {
  if (sim_.active()) sim_.end();
  world_ = nullptr;
  if (current_ref() == this) current_ref() = nullptr;
}

void SurvivalPlugin::ensure_client(const runtime::World& w) {
  if (!sim_.active()) {
    sim_.begin_client(data_);
    world_ = const_cast<runtime::World*>(&w);
  }
}

void SurvivalPlugin::draw_under(const runtime::World& w, const runtime::WorldRect& view,
                                std::vector<runtime::DrawQuad>* out) {
  if (w.net_role() == runtime::NetRole::Client) {
    ensure_client(w);
    sim_.client_frame(w);
  }
  if (!sim_.has_seed()) return;
  const int tx0 = static_cast<int>(std::floor(view.x0 / kTile)) - 1;
  const int ty0 = static_cast<int>(std::floor(view.y0 / kTile)) - 1;
  const int tx1 = static_cast<int>(std::floor(view.x1 / kTile)) + 1;
  const int ty1 = static_cast<int>(std::floor(view.y1 / kTile)) + 1;
  if ((tx1 - tx0) * (ty1 - ty0) > 200 * 200) return;  // zoomed way out
  const WorldGen& gen = sim_.chunks().gen();
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      const Tile* p = sim_.chunks().peek(tx, ty);
      const Tile t = p ? *p : gen.tile_at(tx, ty);
      const float x = tx * kTile, y = ty * kTile;
      quadc(out, x, y, x + kTile, y + kTile, ground_def(t.ground).color, speckle(tx, ty));
      if (t.object != kObjNone) draw_object(out, t.object, x, y, t.damage);
    }
  }
}

void SurvivalPlugin::draw_over(const runtime::World& w, const runtime::WorldRect& view,
                               std::vector<runtime::DrawQuad>* out) {
  if (!sim_.has_seed()) return;
  const GameData& d = sim_.data();
  // Creatures.
  for (const FrontierSim::Mob& m : sim_.mobs()) {
    if (m.def < 0 || m.def >= static_cast<int>(d.creatures.size())) continue;
    const CreatureDef& c = d.creatures[static_cast<std::size_t>(m.def)];
    const float h = c.size * kTile * 0.5f;
    if (m.x + h < view.x0 || m.x - h > view.x1 || m.y + h < view.y0 || m.y - h > view.y1) continue;
    quad(out, m.x - h * 0.9f, m.y + h * 0.6f, m.x + h * 0.9f, m.y + h * 0.9f, 0, 0, 0, 0.25f);
    if (m.hurt > 0.0f) quad(out, m.x - h, m.y - h, m.x + h, m.y + h * 0.7f, 1.0f, 0.9f, 0.9f);
    else quadc(out, m.x - h, m.y - h, m.x + h, m.y + h * 0.7f, c.color);
    quad(out, m.x - h * 0.6f, m.y - h * 0.6f, m.x - h * 0.3f, m.y - h * 0.3f,
         m.state == 1 ? 0.95f : 0.1f, 0.1f, 0.1f);
    quad(out, m.x + h * 0.3f, m.y - h * 0.6f, m.x + h * 0.6f, m.y - h * 0.3f,
         m.state == 1 ? 0.95f : 0.1f, 0.1f, 0.1f);
    if (!c.friendly && m.hp < c.hp) {
      const float f = std::clamp(static_cast<float>(m.hp) / static_cast<float>(c.hp), 0.0f, 1.0f);
      quad(out, m.x - h, m.y - h - 7, m.x + h, m.y - h - 3, 0.15f, 0.08f, 0.06f);
      quad(out, m.x - h, m.y - h - 7, m.x - h + 2 * h * f, m.y - h - 3, 0.85f, 0.25f, 0.18f);
    }
  }
  // Shots.
  for (const FrontierSim::Shot& s : sim_.shots()) {
    quad(out, s.x - 3, s.y - 3, s.x + 3, s.y + 3, 1.0f, 0.92f, 0.55f);
  }
  // Riders' grit bars and target outline.
  const int me = w.net_role() == runtime::NetRole::Client ? sim_.my_slot() : 0;
  for (int s = 0; s < FrontierSim::kSlots; ++s) {
    const runtime::Actor* a = w.player(s);
    if (!a || a->hidden) continue;
    const FrontierSim::Player& p = sim_.player(s);
    const runtime::Aabb b = runtime::collider_box(a->data);
    const float f = std::clamp(static_cast<float>(p.hp) / static_cast<float>(p.max_hp), 0.0f, 1.0f);
    const float cx = (b.x0 + b.x1) * 0.5f;
    const float top = a->data.y - 8.0f;
    quad(out, cx - 16, top - 6, cx + 16, top - 2, 0.15f, 0.08f, 0.06f);
    quad(out, cx - 16, top - 6, cx - 16 + 32 * f, top - 2,
         p.hurt > 0.0 ? 1.0f : 0.78f, 0.24f, 0.17f);
    if (s == me) {
      const float x = p.target_tx * kTile, y = p.target_ty * kTile;
      const float k = 2.0f;
      quad(out, x, y, x + kTile, y + k, 0.96f, 0.85f, 0.55f, 0.8f);
      quad(out, x, y + kTile - k, x + kTile, y + kTile, 0.96f, 0.85f, 0.55f, 0.8f);
      quad(out, x, y, x + k, y + kTile, 0.96f, 0.85f, 0.55f, 0.8f);
      quad(out, x + kTile - k, y, x + kTile, y + kTile, 0.96f, 0.85f, 0.55f, 0.8f);
    }
  }
  // Night: a dusk wash over everything, holes punched by light sources
  // approximated with warm glows on top.
  const float dark = sim_.darkness();
  if (dark > 0.01f) {
    quad(out, view.x0, view.y0, view.x1, view.y1, 0.04f, 0.03f, 0.12f, dark);
    const int tx0 = static_cast<int>(std::floor(view.x0 / kTile)) - 4;
    const int ty0 = static_cast<int>(std::floor(view.y0 / kTile)) - 4;
    const int tx1 = static_cast<int>(std::floor(view.x1 / kTile)) + 4;
    const int ty1 = static_cast<int>(std::floor(view.y1 / kTile)) + 4;
    if ((tx1 - tx0) * (ty1 - ty0) <= 120 * 120) {
      for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
          const Tile* t = sim_.chunks().peek(tx, ty);
          if (!t || t->object == kObjNone) continue;
          const float r = object_def(t->object).light * kTile;
          if (r <= 0.0f) continue;
          const float cx = (tx + 0.5f) * kTile, cy = (ty + 0.5f) * kTile;
          for (int ring = 3; ring >= 1; --ring) {
            const float rr = r * static_cast<float>(ring) / 3.0f;
            quad(out, cx - rr, cy - rr, cx + rr, cy + rr, 1.0f, 0.72f, 0.35f, dark * 0.16f);
          }
        }
      }
    }
  }
  // Hotbar (view space: bottom centre of the view).
  const FrontierSim::Player& p = sim_.player(me);
  const float vw = view.x1 - view.x0, vh = view.y1 - view.y0;
  const float cell = std::clamp(vw / 26.0f, 18.0f, 44.0f);
  const float gap = cell * 0.12f;
  const float total = Inventory::kHotbar * cell + (Inventory::kHotbar - 1) * gap;
  const float x0 = view.x0 + (vw - total) * 0.5f;
  const float y0 = view.y0 + vh - cell - cell * 0.4f;
  quad(out, x0 - gap * 2, y0 - gap * 2, x0 + total + gap * 2, y0 + cell + gap * 2, 0.10f, 0.07f,
       0.09f, 0.82f);
  for (int i = 0; i < Inventory::kHotbar; ++i) {
    const float x = x0 + i * (cell + gap);
    const bool sel = i == p.hotbar;
    if (sel) quad(out, x - gap, y0 - gap, x + cell + gap, y0 + cell + gap, 0.93f, 0.62f, 0.28f);
    quad(out, x, y0, x + cell, y0 + cell, 0.22f, 0.16f, 0.17f);
    const Stack& st = p.inv.at(i);
    const ItemDef* it = st.empty() ? nullptr : d.item_def(st.item);
    if (!it) continue;
    const float m = cell * 0.2f;
    quadc(out, x + m, y0 + m, x + cell - m, y0 + cell - m, it->color);
    // Count as pips (no font in the runtime draw list): one per 10, max 9.
    const int pips = std::min(9, (st.count + 9) / 10);
    for (int k = 0; k < pips && st.count > 1; ++k) {
      const float px = x + 2 + k * (cell - 4) / 9.0f;
      quad(out, px, y0 + cell - 4, px + (cell - 4) / 9.0f - 1, y0 + cell - 2, 0.96f, 0.9f, 0.7f);
    }
  }
  // Grit bar above the hotbar.
  const float f = std::clamp(static_cast<float>(p.hp) / static_cast<float>(p.max_hp), 0.0f, 1.0f);
  quad(out, x0, y0 - cell * 0.35f, x0 + total, y0 - cell * 0.2f, 0.15f, 0.08f, 0.06f, 0.9f);
  quad(out, x0, y0 - cell * 0.35f, x0 + total * f, y0 - cell * 0.2f, 0.78f, 0.24f, 0.17f, 0.95f);
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(frontier, ::ts::tombstone::survival::SurvivalPlugin)
