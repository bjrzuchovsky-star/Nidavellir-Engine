// Terrain queries for a running 3D world and the Lua `terrain` table:
//
//   terrain.height(x, z) -> y (or nil off the terrain)
//   terrain.normal(x, z) -> nx, ny, nz
//   terrain.layer(x, z)  -> dominant splat layer (1..4), weight 0..1
//                           (footstep sounds, slowing down in sand)
//   terrain.foliage(x, z, radius) -> foliage instances within radius
//
// Heights come from the shared terrain store (terrain/TerrainView.h), so
// edits made in the editor show up in a play-in-editor ride at once.

#include "world3d/World3D.h"

#include "terrain/TerrainView.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace world3d {

namespace {

struct Hit {
  std::shared_ptr<terrain::TerrainData> data;
  Vec3 origin;
};

bool terrain_under(const World3D& w, float x, float z, Hit* out) {
  for (const Entity3D& e : w.entities()) {
    if (!e.terrain || !e.visible) continue;
    std::shared_ptr<terrain::TerrainData> t = terrain::acquire(w.project_dir(), *e.terrain);
    if (!t || !t->valid()) continue;
    const Vec3 o = e.transform.position;
    if (!terrain::inside(*t, x - o.x, z - o.z)) continue;
    out->data = std::move(t);
    out->origin = o;
    return true;
  }
  return false;
}

World3D& W(lua_State* L) { return *static_cast<World3D*>(lua_touserdata(L, lua_upvalueindex(1))); }

float fnum(lua_State* L, int idx) {
  const lua_Number n = luaL_checknumber(L, idx);
  luaL_argcheck(L, std::isfinite(n) && std::fabs(n) < 1e7, idx, "number out of range");
  return static_cast<float>(n);
}

int l_height(lua_State* L) {
  float y = 0;
  if (!W(L).terrain_height(fnum(L, 1), fnum(L, 2), &y)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, y);
  return 1;
}

int l_normal(lua_State* L) {
  float y = 0;
  Vec3 n{0, 1, 0};
  W(L).terrain_height(fnum(L, 1), fnum(L, 2), &y, &n);
  lua_pushnumber(L, n.x);
  lua_pushnumber(L, n.y);
  lua_pushnumber(L, n.z);
  return 3;
}

int l_layer(lua_State* L) {
  float w = 0;
  const int layer = W(L).terrain_layer(fnum(L, 1), fnum(L, 2), &w);
  if (layer < 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, layer + 1);
  lua_pushnumber(L, w);
  return 2;
}

int l_foliage(lua_State* L) {
  const float x = fnum(L, 1), z = fnum(L, 2), r = std::fabs(fnum(L, 3));
  Hit h;
  int n = 0;
  if (terrain_under(W(L), x, z, &h)) {
    const float lx = x - h.origin.x, lz = z - h.origin.z;
    for (const terrain::FoliageInstance& f : h.data->foliage) {
      const float dx = f.x - lx, dz = f.z - lz;
      if (dx * dx + dz * dz <= r * r) ++n;
    }
  }
  lua_pushinteger(L, n);
  return 1;
}

}  // namespace

bool World3D::terrain_height(float x, float z, float* y, Vec3* normal) const {
  Hit h;
  if (!terrain_under(*this, x, z, &h)) return false;
  const float lx = x - h.origin.x, lz = z - h.origin.z;
  if (y) *y = terrain::height_at(*h.data, lx, lz) + h.origin.y;
  if (normal) *normal = terrain::normal_at(*h.data, lx, lz);
  return true;
}

bool World3D::terrain_solid_height(float x, float z, float* y, Vec3* normal) const {
  for (const Entity3D& e : entities()) {
    if (!e.terrain || !e.terrain->collider || dead_.count(e.id)) continue;
    std::shared_ptr<terrain::TerrainData> t = terrain::acquire(project_dir_, *e.terrain);
    if (!t || !t->valid()) continue;
    const Vec3 o = e.transform.position;
    if (!terrain::inside(*t, x - o.x, z - o.z)) continue;
    const float lx = x - o.x, lz = z - o.z;
    if (y) *y = terrain::height_at(*t, lx, lz) + o.y;
    if (normal) *normal = terrain::normal_at(*t, lx, lz);
    return true;
  }
  return false;
}

int World3D::terrain_layer(float x, float z, float* weight) const {
  Hit h;
  if (!terrain_under(*this, x, z, &h)) return -1;
  float w[terrain::kLayers];
  terrain::weights_at(*h.data, x - h.origin.x, z - h.origin.z, w);
  int best = 0;
  for (int i = 1; i < terrain::kLayers; ++i)
    if (w[i] > w[best]) best = i;
  if (weight) *weight = w[best];
  return best;
}

void World3D::open_terrain_lua(lua_State* L, int sandbox) {
  const luaL_Reg fns[] = {{"height", l_height},
                          {"normal", l_normal},
                          {"layer", l_layer},
                          {"foliage", l_foliage},
                          {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, fns, 1);
  lua_setfield(L, sandbox, "terrain");
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
