#include "mmo/MmoWorld.h"

#include "scene/JsonTree.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace ts {
namespace tombstone {
namespace mmo {

namespace fs = std::filesystem;
using runtime::Aabb;

namespace {

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool rect_of(const json::Value& v, Aabb* out) {
  float r[4];
  if (!v.is_array() || v.size() != 4) return false;
  for (std::size_t i = 0; i < 4; ++i) {
    if (!v[i].is_number()) return false;
    r[i] = v[i].as_float();
  }
  *out = Aabb{std::min(r[0], r[2]), std::min(r[1], r[3]), std::max(r[0], r[2]), std::max(r[1], r[3])};
  return out->x1 > out->x0 && out->y1 > out->y0;
}

bool point_of(const json::Value& v, float* x, float* y) {
  if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) return false;
  *x = v[0].as_float();
  *y = v[1].as_float();
  return true;
}

// Scene -> geometry, spawn, items (entities named "item:<name>").
bool load_scene(const std::string& project_dir, ZoneDef* z, bool derive, std::string* error) {
  if (z->scene.empty()) return true;
  const fs::path path = fs::path(project_dir) / z->scene;
  const std::string text = read_text(path);
  if (text.empty()) {
    if (error) *error = "zone " + z->geo.id + ": no scene at " + path.string();
    return false;
  }
  scene_json::SceneDoc doc;
  std::string err;
  if (!scene_json::parse(text, &doc, &err, path.string())) {
    if (error) *error = "zone " + z->geo.id + ": " + err;
    return false;
  }
  float x0 = std::numeric_limits<float>::max(), y0 = x0, x1 = -x0, y1 = -x0;
  bool have_spawn = false;
  for (const Entity2D& e : doc.entities) {
    x0 = std::min(x0, e.x);
    y0 = std::min(y0, e.y);
    x1 = std::max(x1, e.x + e.w);
    y1 = std::max(y1, e.y + e.h);
    if (e.name.rfind("item:", 0) == 0 && e.name.size() > 5) {
      if (derive) z->items.push_back(ItemSpawn{e.name.substr(5), e.x + e.w * 0.5f, e.y + e.h * 0.5f, 30.0});
      continue;  // pickups are the server's, not the World's
    }
    if (e.player) {
      if (derive && !have_spawn) {
        z->spawn_x = e.x + e.w * 0.5f;
        z->spawn_y = e.y + e.h * 0.5f;
        have_spawn = true;
      }
      continue;  // riders are characters, not scene actors
    }
    if (e.collider && !e.collider->trigger && !e.collider->dynamic) {
      const ColliderData& c = *e.collider;
      z->geo.blockers.push_back(Aabb{e.x + c.offset_x, e.y + c.offset_y, e.x + c.offset_x + c.w,
                                     e.y + c.offset_y + c.h});
    }
    z->entities.push_back(e);
  }
  z->solidity = doc.tile_solidity;
  if (derive && x1 > x0) {
    const float m = 256.0f;
    z->geo.bounds = Aabb{x0 - m, y0 - m, x1 + m, y1 + m};
    if (!have_spawn) {
      z->spawn_x = (x0 + x1) * 0.5f;
      z->spawn_y = (y0 + y1) * 0.5f;
    }
  }
  return true;
}

bool box_overlaps(const Aabb& a, const Aabb& b) {
  return a.x0 < b.x1 - runtime::kContactSlop && b.x0 < a.x1 - runtime::kContactSlop &&
         a.y0 < b.y1 - runtime::kContactSlop && b.y0 < a.y1 - runtime::kContactSlop;
}

}  // namespace

const ZoneDef* WorldConfig::zone(const std::string& id) const {
  for (const ZoneDef& z : zones)
    if (z.geo.id == id) return &z;
  return nullptr;
}

bool load_world_config(const std::string& project_dir, WorldConfig* out, std::string* error) {
  *out = WorldConfig{};
  const fs::path cfg = fs::path(project_dir) / "mmo.json";
  std::error_code ec;
  if (!fs::exists(cfg, ec)) {
    ZoneDef z;
    z.geo.id = "main";
    z.geo.name = "Main";
    const std::string scene = scene_json::scene_path_for_project(project_dir);
    z.scene = fs::exists(scene, ec) ? fs::path(scene).filename().string() : std::string();
    if (!load_scene(project_dir, &z, true, error)) return false;
    out->zones.push_back(std::move(z));
    return true;
  }
  json::Value root;
  std::string err;
  if (!json::parse(read_text(cfg), &root, &err) || !root.is_object()) {
    if (error) *error = "mmo.json: " + (err.empty() ? std::string("not an object") : err);
    return false;
  }
  out->tick_rate = std::clamp(static_cast<int>(root.num("tick_rate", 20)), 5, 60);
  out->speed = std::clamp(root.flt("speed", 160.0f), 10.0f, 2000.0f);
  out->interest_radius = std::clamp(root.flt("interest_radius", 640.0f), 64.0f, 8192.0f);
  out->save_every = std::clamp(root.num("save_every", 30.0), 1.0, 3600.0);
  out->linkdead_seconds = std::clamp(root.num("linkdead_seconds", 30.0), 0.0, 3600.0);
  out->max_players = std::clamp(static_cast<int>(root.num("max_players", 512)), 1, 4000);
  out->open_registration = root.flag("open_registration", true);
  out->argon2_kib = static_cast<std::uint32_t>(std::clamp(root.num("argon2_kib", 4096), 64.0, 262144.0));
  const json::Value& chat = root["chat"];
  if (chat.is_object()) {
    if (chat["channels"].is_array()) {
      out->chat.channels.clear();
      for (const json::Value& c : chat["channels"].items())
        if (c.is_string() && !c.as_string().empty()) out->chat.channels.push_back(c.as_string());
    }
    out->chat.say_radius = chat.flt("say_radius", out->chat.say_radius);
    out->chat.max_length = static_cast<std::size_t>(std::clamp(chat.num("max_length", 200), 16.0, 1000.0));
    out->chat.per_10s = std::clamp(static_cast<int>(chat.num("per_10s", 8)), 1, 100);
  }
  for (const json::Value& zv : root["zones"].items()) {
    ZoneDef z;
    z.geo.id = zv.str("id");
    if (z.geo.id.empty()) {
      if (error) *error = "mmo.json: a zone without an id";
      return false;
    }
    z.geo.name = zv.str("name", z.geo.id);
    z.scene = zv.str("scene");
    const bool derive = !zv.has("bounds");
    if (!load_scene(project_dir, &z, derive || !zv.has("spawn"), error)) return false;
    if (zv.has("bounds") && !rect_of(zv["bounds"], &z.geo.bounds)) {
      if (error) *error = "mmo.json: zone " + z.geo.id + " bounds must be [x0, y0, x1, y1]";
      return false;
    }
    if (zv.has("spawn")) point_of(zv["spawn"], &z.spawn_x, &z.spawn_y);
    for (const json::Value& bv : zv["blockers"].items()) {
      runtime::Aabb b;
      if (!rect_of(bv, &b)) {
        if (error) *error = "mmo.json: zone " + z.geo.id + " has a bad blocker";
        return false;
      }
      z.geo.blockers.push_back(b);
    }
    for (const json::Value& pv : zv["portals"].items()) {
      Portal p;
      p.to = pv.str("to");
      if (!rect_of(pv["rect"], &p.rect) || p.to.empty() || !point_of(pv["at"], &p.at_x, &p.at_y)) {
        if (error) *error = "mmo.json: zone " + z.geo.id + " has a bad portal";
        return false;
      }
      z.geo.portals.push_back(p);
    }
    for (const json::Value& iv : zv["items"].items()) {
      ItemSpawn it;
      it.name = iv.str("name");
      it.respawn = iv.num("respawn", 30.0);
      if (it.name.empty() || !point_of(iv["at"], &it.x, &it.y)) {
        if (error) *error = "mmo.json: zone " + z.geo.id + " has a bad item";
        return false;
      }
      z.items.push_back(it);
    }
    out->zones.push_back(std::move(z));
  }
  if (out->zones.empty()) {
    if (error) *error = "mmo.json: no zones";
    return false;
  }
  for (const ZoneDef& z : out->zones)
    for (const Portal& p : z.geo.portals)
      if (!out->zone(p.to)) {
        if (error) *error = "mmo.json: portal in " + z.geo.id + " leads to unknown zone " + p.to;
        return false;
      }
  return true;
}

void step_rider(const ZoneGeometry& geo, float speed, float dt, float mx, float my, float* x, float* y) {
  if (!std::isfinite(mx) || !std::isfinite(my)) mx = my = 0.0f;
  const float len = std::sqrt(mx * mx + my * my);
  if (len > 1.0f) {
    mx /= len;
    my /= len;
  }
  float dx = mx * speed * dt, dy = my * speed * dt;
  const float h = kRiderHalf;
  // Substeps of at most half a rider, so nothing thin is skipped.
  const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)) / h)));
  dx /= static_cast<float>(steps);
  dy /= static_cast<float>(steps);
  for (int s = 0; s < steps; ++s) {
    if (dx != 0.0f) {
      *x += dx;
      const Aabb box{*x - h, *y - h, *x + h, *y + h};
      for (const Aabb& b : geo.blockers)
        if (box_overlaps(box, b)) *x = dx > 0 ? b.x0 - h : b.x1 + h;
    }
    if (dy != 0.0f) {
      *y += dy;
      const Aabb box{*x - h, *y - h, *x + h, *y + h};
      for (const Aabb& b : geo.blockers)
        if (box_overlaps(box, b)) *y = dy > 0 ? b.y0 - h : b.y1 + h;
    }
  }
  *x = std::clamp(*x, geo.bounds.x0 + h, geo.bounds.x1 - h);
  *y = std::clamp(*y, geo.bounds.y0 + h, geo.bounds.y1 - h);
}

const Portal* portal_at(const ZoneGeometry& geo, float x, float y) {
  for (const Portal& p : geo.portals)
    if (x >= p.rect.x0 && x <= p.rect.x1 && y >= p.rect.y0 && y <= p.rect.y1) return &p;
  return nullptr;
}

void InterestGrid::reset(float cell) {
  cell_ = std::max(16.0f, cell);
  for (auto& [k, v] : cells_) v.clear();
  count_ = 0;
}

void InterestGrid::insert(std::uint64_t id, float x, float y) {
  const int cx = static_cast<int>(std::floor(x / cell_)), cy = static_cast<int>(std::floor(y / cell_));
  cells_[key(cx, cy)].push_back(Item{id, x, y});
  ++count_;
}

void InterestGrid::query(float x, float y, float radius, std::vector<std::uint64_t>* out) const {
  const int r = static_cast<int>(std::ceil(radius / cell_));
  const int cx = static_cast<int>(std::floor(x / cell_)), cy = static_cast<int>(std::floor(y / cell_));
  const float r2 = radius * radius;
  for (int ix = cx - r; ix <= cx + r; ++ix)
    for (int iy = cy - r; iy <= cy + r; ++iy) {
      auto it = cells_.find(key(ix, iy));
      if (it == cells_.end()) continue;
      for (const Item& i : it->second) {
        const float dx = i.x - x, dy = i.y - y;
        if (dx * dx + dy * dy <= r2) out->push_back(i.id);
      }
    }
}

bool write_sample_world(const std::string& project_dir, std::string* error) {
  std::error_code ec;
  fs::create_directories(project_dir, ec);
  std::ofstream out(fs::path(project_dir) / "mmo.json", std::ios::binary);
  if (!out) {
    if (error) *error = "could not write " + (fs::path(project_dir) / "mmo.json").string();
    return false;
  }
  // A 4 km frontier town with a gold mine through a portal. Nuggets are
  // pickups; the buildings are blockers. Interest radius 640 px means a
  // rider sees a small part of the town at a time.
  out << R"({
  "tick_rate": 20,
  "speed": 160,
  "interest_radius": 640,
  "save_every": 30,
  "linkdead_seconds": 30,
  "max_players": 512,
  "open_registration": true,
  "chat": {"channels": ["trade", "help"], "say_radius": 480, "max_length": 200, "per_10s": 8},
  "zones": [
    {"id": "town", "name": "Frontier Town", "bounds": [0, 0, 4000, 4000], "spawn": [2000, 2000],
     "blockers": [[1700, 1700, 1900, 1800], [2100, 1700, 2300, 1800], [1700, 2200, 1900, 2300],
                  [2100, 2200, 2300, 2300], [600, 600, 900, 700], [3100, 3100, 3400, 3200]],
     "portals": [{"rect": [3900, 1950, 4000, 2050], "to": "mine", "at": [120, 750]}],
     "items": [{"name": "gold_nugget", "at": [2060, 2000], "respawn": 20},
               {"name": "gold_nugget", "at": [800, 900], "respawn": 20},
               {"name": "gold_nugget", "at": [3200, 900], "respawn": 20},
               {"name": "horseshoe", "at": [1000, 3000], "respawn": 40},
               {"name": "horseshoe", "at": [3000, 3000], "respawn": 40}]},
    {"id": "mine", "name": "Lucky Strike Mine", "bounds": [0, 0, 1500, 1500], "spawn": [120, 750],
     "blockers": [[400, 300, 450, 1200], [900, 300, 950, 1200]],
     "portals": [{"rect": [0, 700, 40, 800], "to": "town", "at": [3850, 2000]}],
     "items": [{"name": "gold_nugget", "at": [700, 750], "respawn": 10},
               {"name": "gold_nugget", "at": [1200, 400], "respawn": 10}]}
  ]
}
)";
  return true;
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
