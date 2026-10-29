// ABI 4 for C# and C++ scripts: 3D pose, Move/Rotate deltas, local euler,
// input axes and a static-collider raycast. The Rider plugin is not
// involved; a script moves the entity it is on.

#include "world3d/World3D.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

int World3D::script_query(const char* op, const char* args, char* out, int cap) {
  if (!op) return -1;
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
