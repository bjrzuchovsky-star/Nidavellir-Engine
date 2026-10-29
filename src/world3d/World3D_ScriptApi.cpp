// ABI 4 for C# and C++ scripts: 3D pose, Move/Rotate deltas, local euler,
// input axes and a static-collider raycast. The Rider plugin is not
// involved; a script moves the entity it is on.

#include "world3d/World3D.h"

#include "scene/Prefab.h"
#include "render3d/Scene3D.h"

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
namespace world3d {

namespace {

bool ieq(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
      return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

void write_pose(const Entity3D& e, float* o) {
  for (int i = 0; i < 18; ++i) o[i] = 0.0f;
  const render3d::Vec3 p = e.transform.position;
  const render3d::Vec3 eu = e.transform.rotation.to_euler_deg();
  const render3d::Vec3 s = e.transform.scale;
  const render3d::Vec3 f = e.transform.rotation.rotate({0.0f, 0.0f, -1.0f});
  const render3d::Vec3 r = e.transform.rotation.rotate({1.0f, 0.0f, 0.0f});
  const render3d::Vec3 u = e.transform.rotation.rotate({0.0f, 1.0f, 0.0f});
  o[0] = p.x;
  o[1] = p.y;
  o[2] = p.z;
  o[3] = eu.x;
  o[4] = eu.y;
  o[5] = eu.z;
  o[6] = s.x;
  o[7] = s.y;
  o[8] = s.z;
  o[9] = f.x;
  o[10] = f.y;
  o[11] = f.z;
  o[12] = r.x;
  o[13] = r.y;
  o[14] = r.z;
  o[15] = u.x;
  o[16] = u.y;
  o[17] = u.z;
}

}  // namespace

void World3D::note_script_input(const Input3D& in) {
  script_mouse_x_ = in.mouse_dx;
  script_mouse_y_ = in.mouse_dy;
  script_move_right_ = in.move_right;
  script_move_forward_ = in.move_forward;
  script_jump_down_ = in.jump && !script_jump_held_;
  script_jump_held_ = in.jump;
  script_action_down_ = in.action && !script_action_held_;
  script_action_held_ = in.action;
}

void World3D::separate_from_statics(Entity3D& e) {
  if (!e.collider || e.collider->trigger) return;
  if (e.id == player_ && phys_ && phys_->has_character()) return;
  float r = 0.4f;
  float h = 1.0f;
  render3d::Vec3 off;
  if (e.collider->shape == scene3d::Collider3D::Shape::Capsule) {
    r = e.collider->size.x;
    h = std::max(e.collider->size.y, 2.0f * r);
  } else {
    r = 0.5f * std::max(e.collider->size.x, e.collider->size.z);
    h = e.collider->size.y;
  }
  off = e.collider->offset;
  const float half = std::max(0.0f, h * 0.5f - r);
  for (int iter = 0; iter < 4; ++iter) {
    bool moved = false;
    for (const Static& s : statics_) {
      if (s.id == e.id) continue;
      const render3d::Vec3 c = e.transform.position + off;
      const render3d::Vec3 push =
          capsule_box_push(c - render3d::Vec3{0, half, 0}, c + render3d::Vec3{0, half, 0}, r, s.box);
      if (render3d::length(push) <= 1e-6f) continue;
      e.transform.position += push;
      moved = true;
    }
    if (!moved) break;
  }
}

void World3D::apply_one_local(Entity3D& e, const Vec3& euler) {
  const render3d::Quat local = render3d::Quat::from_euler_deg(euler);
  if (e.parent != 0) {
    if (const Entity3D* p = find(e.parent)) {
      e.transform.rotation = render3d::normalize(p->transform.rotation * local);
      return;
    }
  }
  e.transform.rotation = local;
}

void World3D::apply_script_locals() {
  for (const auto& kv : local_euler_) {
    Entity3D* e = live(kv.first);
    if (e) apply_one_local(*e, kv.second);
  }
}

bool World3D::script_pose(std::uint64_t id, float* out18) const {
  const Entity3D* e = find(id);
  if (!e || dead_.count(id) || !out18) return false;
  write_pose(*e, out18);
  return true;
}

void World3D::script_move_by(std::uint64_t id, float x, float y, float z) {
  Entity3D* e = live(id);
  if (!e) return;
  e->transform.position += render3d::Vec3{x, y, z};
  separate_from_statics(*e);
}

void World3D::script_set_position(std::uint64_t id, float x, float y, float z) {
  Entity3D* e = live(id);
  if (!e) return;
  e->transform.position = {x, y, z};
  separate_from_statics(*e);
}

void World3D::script_rotate_by(std::uint64_t id, float ex, float ey, float ez) {
  Entity3D* e = live(id);
  if (!e) return;
  local_euler_.erase(id);
  render3d::Vec3 eu = e->transform.rotation.to_euler_deg();
  eu.x += ex;
  eu.y += ey;
  eu.z += ez;
  e->transform.rotation = render3d::Quat::from_euler_deg(eu);
}

void World3D::script_set_local_euler(std::uint64_t id, float ex, float ey, float ez) {
  Entity3D* e = live(id);
  if (!e) return;
  const Vec3 euler{ex, ey, ez};
  local_euler_[id] = euler;
  apply_one_local(*e, euler);
}

float World3D::script_axis(int slot, const char* name) const {
  if (slot != 0 || !name) return 0.0f;
  if (ieq(name, "Horizontal")) return script_move_right_;
  if (ieq(name, "Vertical")) return script_move_forward_;
  // Raw mouse +x is right. Negate so Rotate(Up * GetAxis("Mouse X")) yaws
  // right in this right-handed, -Z-forward world.
  if (ieq(name, "Mouse X")) return -script_mouse_x_;
  // Raw mouse +y is down. The sample does `pitch -= mouseY` and positive
  // pitch looks up, so the raw sign already looks up when the mouse goes up.
  if (ieq(name, "Mouse Y")) return script_mouse_y_;
  return 0.0f;
}

bool World3D::script_button_down(int slot, const char* name) const {
  if (slot != 0 || !name) return false;
  if (ieq(name, "Jump")) return script_jump_down_;
  if (ieq(name, "Action") || ieq(name, "Use")) return script_action_down_;
  return false;
}

int put_text(const std::string& s, char* out, int cap) {
  const int n = static_cast<int>(s.size());
  if (out && cap >= n) std::memcpy(out, s.data(), s.size());
  return n;
}

std::vector<std::string> fields_of(const char* args) {
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

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool has_component3(const Entity3D& e, const std::string& name) {
  if (name == "transform") return true;
  if (name == "collider") return e.collider.has_value();
  if (name == "rigidbody") return e.rigid_body.has_value();
  if (name == "renderer") return e.mesh.has_value();
  if (name == "audio") return e.audio.has_value();
  if (name == "camera") return e.camera.has_value();
  if (name == "animator") return e.animator.has_value();
  if (name == "script") return e.script.has_value();
  return false;
}

void World3D::flush_delayed() {
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

std::uint64_t World3D::script_adopt(Entity3D data) {
  if (ents_.size() >= 4096) return 0;
  data.player.reset();
  if (data.id == 0 || find(data.id)) {
    std::uint64_t id = 1;
    for (const Entity3D& e : ents_) id = std::max(id, e.id + 1);
    data.id = id;
  }
  const std::uint64_t id = data.id;
  const bool scripted = data.script.has_value();
  ents_.push_back(std::move(data));
  spawned_.insert(id);
  if (host_ && scripted) host_->attach(*script_entity(id));
  rebuild_statics();
  return id;
}

int World3D::script_query(const char* op, const char* args, char* out, int cap) {
  if (!op) return -1;
  const std::vector<std::string> f = fields_of(args);
  auto id_of = [&](std::size_t i) -> std::uint64_t {
    return i < f.size() ? static_cast<std::uint64_t>(std::strtoull(f[i].c_str(), nullptr, 10)) : 0;
  };
  if (ieq(op, "alive")) return put_text(live(id_of(0)) ? "1" : "0", out, cap);
  if (ieq(op, "name")) {
    const Entity3D* e = live(id_of(0));
    return put_text(e ? e->name : "", out, cap);
  }
  if (ieq(op, "tag")) {
    const Entity3D* e = live(id_of(0));
    return put_text(e ? e->tag : "", out, cap);
  }
  if (ieq(op, "set_tag")) {
    if (Entity3D* e = live(id_of(0))) e->tag = f.size() > 1 ? f[1] : "";
    return put_text("1", out, cap);
  }
  if (ieq(op, "find_tag")) {
    const std::string want = f.empty() ? "" : f[0];
    for (const Entity3D& e : ents_) {
      if (!dead_.count(e.id) && e.tag == want) return put_text(std::to_string(e.id), out, cap);
    }
    return put_text("0", out, cap);
  }
  if (ieq(op, "has") || ieq(op, "add") || ieq(op, "remove")) {
    Entity3D* e = live(id_of(0));
    const std::string name = lower_copy(f.size() > 1 ? f[1] : "");
    if (!e) return put_text("0", out, cap);
    if (ieq(op, "has")) return put_text(has_component3(*e, name) ? "1" : "0", out, cap);
    if (ieq(op, "remove")) {
      if (name == "collider") e->collider.reset();
      else if (name == "rigidbody") e->rigid_body.reset();
      else if (name == "renderer") e->mesh.reset();
      else if (name == "audio") e->audio.reset();
      else if (name == "camera") e->camera.reset();
      else if (name == "animator") e->animator.reset();
      else return put_text("0", out, cap);
      rebuild_statics();
      return put_text("1", out, cap);
    }
    if (name == "transform" || name == "script") return put_text("1", out, cap);
    if (name == "collider" && !e->collider) {
      scene3d::Collider3D c;
      c.size = {1, 1, 1};
      e->collider = c;
    } else if (name == "rigidbody" && !e->rigid_body) {
      e->rigid_body = scene3d::RigidBody3D{};
    } else if (name == "renderer" && !e->mesh) {
      e->mesh = scene3d::MeshRef{};
    } else if (name == "audio" && !e->audio) {
      e->audio = AudioSourceData{};
    } else if (name == "camera" && !e->camera) {
      e->camera = scene3d::PlayCamera3D{};
    } else if (name == "animator" && !e->animator) {
      e->animator = scene3d::Animator3D{};
    } else if (name != "collider" && name != "rigidbody" && name != "renderer" && name != "audio" &&
               name != "camera" && name != "animator") {
      return put_text("0", out, cap);
    }
    rebuild_statics();
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
  if (ieq(op, "instantiate")) {
    const std::string what = f.empty() ? "" : f[0];
    const float x = f.size() > 1 ? static_cast<float>(std::atof(f[1].c_str())) : 0.0f;
    const float y = f.size() > 2 ? static_cast<float>(std::atof(f[2].c_str())) : 0.0f;
    const float z = f.size() > 3 ? static_cast<float>(std::atof(f[3].c_str())) : 0.0f;
    if (what.size() > 12 && what.ends_with(".prefab.json")) {
      prefab::Asset asset;
      std::string err;
      const std::filesystem::path path = std::filesystem::path(project_dir_) / what;
      if (!prefab::load_asset(path.string(), &asset, &err) || asset.dim != prefab::Dim::D3) return put_text("0", out, cap);
      std::uint64_t next = 1;
      for (const Entity3D& e : ents_) next = std::max(next, e.id + 1);
      const double pos[3] = {x, y, z};
      const std::vector<json::Value> made = prefab::instantiate(asset, what, pos, 1, &next);
      std::uint64_t root = 0;
      for (const json::Value& j : made) {
        Entity3D e;
        if (!scene3d::entity_from_json(j, &e, &err)) continue;
        const std::uint64_t id = script_adopt(std::move(e));
        if (!root) root = id;
      }
      return put_text(std::to_string(root), out, cap);
    }
    const std::uint64_t id = script_spawn(what, x, z);
    if (id) script_set_position(id, x, y, z);
    return put_text(std::to_string(id), out, cap);
  }
  if (ieq(op, "overlap")) {
    float x = 0, y = 0, z = 0, r = 0.5f;
    if (!args || std::sscanf(args, "%f %f %f %f", &x, &y, &z, &r) < 3) return put_text("0", out, cap);
    for (const Entity3D& e : ents_) {
      if (dead_.count(e.id) || !e.collider || e.collider->trigger) continue;
      const Aabb box = collider_box(e);
      const float cx = std::max(box.min.x, std::min(x, box.max.x));
      const float cy = std::max(box.min.y, std::min(y, box.max.y));
      const float cz = std::max(box.min.z, std::min(z, box.max.z));
      const float dx = x - cx, dy = y - cy, dz = z - cz;
      if (dx * dx + dy * dy + dz * dz <= r * r) return put_text(std::to_string(e.id), out, cap);
    }
    return put_text("0", out, cap);
  }
  if (!ieq(op, "raycast")) return -1;
  float x = 0, y = 0, z = 0, dx = 0, dy = 0, dz = 0, dist = 100.0f;
  if (!args || std::sscanf(args, "%f %f %f %f %f %f %f", &x, &y, &z, &dx, &dy, &dz, &dist) < 6) return -1;
  render3d::Vec3 dir{dx, dy, dz};
  const float len = render3d::length(dir);
  if (len < 1e-8f || !(dist > 0.0f)) return 0;
  dir = dir * (1.0f / len);
  const render3d::Ray ray{{x, y, z}, dir};
  float best = dist;
  std::uint64_t hit = 0;
  for (const Static& s : statics_) {
    float t = 0.0f;
    if (render3d::ray_aabb(ray, s.box, &t) && t >= 0.0f && t < best) {
      best = t;
      hit = s.id;
    }
  }
  if (!hit) return 0;
  const render3d::Vec3 p = ray.origin + ray.dir * best;
  char buf[160];
  const int n = std::snprintf(buf, sizeof(buf), "%llu %.6g %.6g %.6g %.6g",
                              static_cast<unsigned long long>(hit), best, p.x, p.y, p.z);
  if (n < 0) return -1;
  if (out && cap >= n) std::memcpy(out, buf, static_cast<std::size_t>(n));
  return n;
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
