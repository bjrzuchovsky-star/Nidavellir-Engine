// Script queries that grew past the fixed ABI: components, tags, instantiate
// (a named entity or a .prefab.json) and Destroy with a delay.

#include "runtime/World.h"

#include "scene/Prefab.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

namespace fs = std::filesystem;

int put_text(const std::string& s, char* out, int cap) {
  const int n = static_cast<int>(s.size());
  if (out && cap >= n) std::memcpy(out, s.data(), s.size());
  return n;
}

std::vector<std::string> fields(const char* args) {
  std::vector<std::string> f;
  std::string cur;
  for (const char* p = args ? args : ""; *p; ++p) {
    if (*p == '\t') {
      f.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(*p);
    }
  }
  f.push_back(cur);
  return f;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool ieq(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

bool has_component(const Entity2D& e, const std::string& name) {
  if (name == "transform") return true;
  if (name == "collider") return e.collider.has_value();
  if (name == "rigidbody") return e.body.has_value();
  if (name == "renderer") return e.sprite.has_value();
  if (name == "audio") return e.audio.has_value();
  if (name == "camera") return e.camera.has_value();
  if (name == "animator") return e.animator.has_value();
  if (name == "script") return e.script.has_value();
  return false;
}

}  // namespace

void World::flush_delayed() {
  const std::uint64_t now = script_tick();
  for (auto it = delayed_.begin(); it != delayed_.end();) {
    if (it->tick <= now) {
      const std::uint64_t id = it->id;
      it = delayed_.erase(it);
      script_destroy(id);
    } else {
      ++it;
    }
  }
}

std::uint64_t World::script_adopt(Entity2D data) {
  if (actors_.size() >= kMaxActors) return 0;
  data.player.reset();
  data.camera.reset();
  data.spawn.reset();
  if (data.id == 0 || live(data.id)) data.id = next_id_++;
  else if (data.id >= next_id_) next_id_ = data.id + 1;
  normalize_components(data);
  Actor a;
  a.data = std::move(data);
  a.prev_x = a.data.x;
  a.prev_y = a.data.y;
  a.spawned = true;
  setup_actor_animation(a);
  const std::uint64_t id = a.data.id;
  actors_.push_back(std::move(a));
  dirty_ = true;
  if (scripts_ && actors_.back().data.script) {
    const Entity2D copy = actors_.back().data;
    scripts_->attach(copy);
  }
  return id;
}

int World::script_query(const char* op, const char* args, char* out, int cap) {
  if (!op) return -1;
  const std::vector<std::string> f = fields(args);
  auto id_of = [&](std::size_t i) -> std::uint64_t {
    return i < f.size() ? static_cast<std::uint64_t>(std::strtoull(f[i].c_str(), nullptr, 10)) : 0;
  };
  if (ieq(op, "alive")) {
    return put_text(live(id_of(0)) ? "1" : "0", out, cap);
  }
  if (ieq(op, "name")) {
    const Actor* a = live(id_of(0));
    return put_text(a ? a->data.name : "", out, cap);
  }
  if (ieq(op, "tag")) {
    const Actor* a = live(id_of(0));
    return put_text(a ? a->data.tag : "", out, cap);
  }
  if (ieq(op, "set_tag")) {
    if (Actor* a = live(id_of(0))) a->data.tag = f.size() > 1 ? f[1] : "";
    return put_text("1", out, cap);
  }
  if (ieq(op, "find_tag")) {
    const std::string want = f.empty() ? "" : f[0];
    for (const Actor& a : actors_) {
      if (!a.dead && a.data.tag == want) return put_text(std::to_string(a.data.id), out, cap);
    }
    return put_text("0", out, cap);
  }
  if (ieq(op, "has") || ieq(op, "add") || ieq(op, "remove")) {
    Actor* a = live(id_of(0));
    const std::string name = lower(f.size() > 1 ? f[1] : "");
    if (!a) return put_text("0", out, cap);
    if (ieq(op, "has")) return put_text(has_component(a->data, name) ? "1" : "0", out, cap);
    if (ieq(op, "remove")) {
      if (name == "collider") a->data.collider.reset();
      else if (name == "rigidbody") a->data.body.reset();
      else if (name == "renderer") a->data.sprite.reset();
      else if (name == "audio") a->data.audio.reset();
      else if (name == "camera") a->data.camera.reset();
      else if (name == "animator") a->data.animator.reset();
      else return put_text("0", out, cap);
      dirty_ = true;
      return put_text("1", out, cap);
    }
    if (name == "transform" || name == "script") return put_text("1", out, cap);
    if (name == "collider" && !a->data.collider) a->data.collider = default_collider(a->data);
    else if (name == "rigidbody" && !a->data.body) a->data.body = RigidBody2DData{};
    else if (name == "renderer" && !a->data.sprite) a->data.sprite = SpriteData{};
    else if (name == "audio" && !a->data.audio) a->data.audio = AudioSourceData{};
    else if (name == "camera" && !a->data.camera) a->data.camera = Camera2DData{};
    else if (name == "animator" && !a->data.animator) a->data.animator = AnimatorData{};
    else if (name != "collider" && name != "rigidbody" && name != "renderer" && name != "audio" &&
             name != "camera" && name != "animator") {
      return put_text("0", out, cap);
    }
    dirty_ = true;
    return put_text("1", out, cap);
  }
  if (ieq(op, "destroy_in")) {
    const std::uint64_t id = id_of(0);
    const double seconds = f.size() > 1 ? std::atof(f[1].c_str()) : 0.0;
    if (!live(id)) return put_text("0", out, cap);
    if (!(seconds > 0.0)) {
      script_destroy(id);
      return put_text("1", out, cap);
    }
    const auto ticks = static_cast<std::uint64_t>(std::ceil(seconds * static_cast<double>(kTickRate)));
    delayed_.push_back({id, script_tick() + std::max<std::uint64_t>(ticks, 1)});
    return put_text("1", out, cap);
  }
  if (ieq(op, "raycast") || ieq(op, "overlap")) {
    float x = 0, y = 0, z = 0, dx = 0, dy = 0, dz = 0, extra = ieq(op, "overlap") ? 0.5f : 100.0f;
    const int nread = args ? std::sscanf(args, "%f %f %f %f %f %f %f", &x, &y, &z, &dx, &dy, &dz, &extra) : 0;
    if (ieq(op, "overlap")) {
      const float r = nread >= 4 ? dx : 0.5f;
      for (const Actor& a : actors_) {
        if (a.dead || !a.data.collider || a.data.collider->trigger) continue;
        const Aabb box = collider_box(a.data);
        const float cx = std::max(box.x0, std::min(x, box.x1));
        const float cy = std::max(box.y0, std::min(y, box.y1));
        const float ddx = x - cx, ddy = y - cy;
        if (ddx * ddx + ddy * ddy <= r * r) return put_text(std::to_string(a.data.id), out, cap);
      }
      return put_text("0", out, cap);
    }
    if (nread < 6) return put_text("0", out, cap);
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-8f) return put_text("0", out, cap);
    dx /= len;
    dy /= len;
    const float max_t = extra > 0 ? extra : 100.0f;
    float best = max_t;
    std::uint64_t hit = 0;
    for (const Actor& a : actors_) {
      if (a.dead || !a.data.collider || a.data.collider->trigger) continue;
      const Aabb box = collider_box(a.data);
      float t0 = 0, t1 = max_t;
      auto slab = [&](float o, float d, float mn, float mx) {
        if (std::fabs(d) < 1e-8f) return o >= mn && o <= mx;
        float ta = (mn - o) / d, tb = (mx - o) / d;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        return t0 <= t1;
      };
      if (!slab(x, dx, box.x0, box.x1) || !slab(y, dy, box.y0, box.y1)) continue;
      if (t0 >= 0 && t0 < best) {
        best = t0;
        hit = a.data.id;
      }
    }
    if (!hit) return put_text("0", out, cap);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%llu %.6g %.6g %.6g %.6g", static_cast<unsigned long long>(hit), best, x + dx * best,
                  y + dy * best, 0.0);
    return put_text(buf, out, cap);
  }
  if (ieq(op, "instantiate")) {
    const std::string what = f.empty() ? "" : f[0];
    const float x = f.size() > 1 ? static_cast<float>(std::atof(f[1].c_str())) : 0.0f;
    const float y = f.size() > 2 ? static_cast<float>(std::atof(f[2].c_str())) : 0.0f;
    if (what.size() > 12 && what.ends_with(".prefab.json")) {
      prefab::Asset asset;
      std::string err;
      const fs::path path = fs::path(project_dir_) / what;
      if (!prefab::load_asset(path.string(), &asset, &err) || asset.dim != prefab::Dim::D2) return put_text("0", out, cap);
      std::uint64_t next = next_id_;
      const double pos[3] = {x, y, 0};
      const std::vector<json::Value> made = prefab::instantiate(asset, what, pos, 1, &next);
      std::uint64_t root = 0;
      for (const json::Value& j : made) {
        Entity2D e;
        if (!scene_json::entity_from_json(j, &e, &err)) continue;
        const std::uint64_t id = script_adopt(std::move(e));
        if (!root) root = id;
      }
      if (next > next_id_) next_id_ = next;
      return put_text(std::to_string(root), out, cap);
    }
    const std::uint64_t id = script_spawn(what, x, y);
    return put_text(std::to_string(id), out, cap);
  }
  return -1;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
