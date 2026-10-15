// World3D VR mode: player rig, locomotion (smooth, snap / smooth turn,
// teleport arc), hands (grab / throw, laser + trigger), the comfort menu,
// thrown bodies and the VR trigger checks. See World3D.h.

#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace ts {
namespace tombstone {
namespace world3d {

using render3d::Mat4;
using render3d::Quat;
using runtime::LogLevel;

namespace {

constexpr float kDeadZone = 0.25f;
constexpr float kBodyGravity = 9.81f;
constexpr float kStepHeight = 0.45f;   // m the rig climbs while walking
constexpr float kBodyRadius = 0.2f;    // m, the walking column around the head
constexpr std::size_t kTrail = 6;      // ticks of grip history for throws

Quat yaw_quat(float deg) { return Quat::axis_angle({0, 1, 0}, render3d::radians(deg)); }

bool overlaps(const Aabb& a, const Aabb& b) {
  return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
         a.min.z < b.max.z && a.max.z > b.min.z;
}

bool inside(const Aabb& b, const Vec3& p) {
  return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y &&
         p.z >= b.min.z && p.z <= b.max.z;
}

float distance_to_box(const Aabb& b, const Vec3& p) {
  const Vec3 c{std::clamp(p.x, b.min.x, b.max.x), std::clamp(p.y, b.min.y, b.max.y),
               std::clamp(p.z, b.min.z, b.max.z)};
  return render3d::length(p - c);
}

Vec3 flat_forward(const Quat& q) {
  Vec3 f = q.rotate({0, 0, -1});
  f.y = 0.0f;
  const float l = render3d::length(f);
  return l > 1e-4f ? f / l : Vec3{0, 0, -1};
}

const char* loco_name(scene3d::VrRig3D::Locomotion l) {
  switch (l) {
    case scene3d::VrRig3D::Locomotion::Teleport: return "teleport only";
    case scene3d::VrRig3D::Locomotion::Smooth: return "smooth only";
    case scene3d::VrRig3D::Locomotion::Both: break;
  }
  return "teleport + smooth";
}

}  // namespace

// ---- Rig + poses ------------------------------------------------------------

void World3D::vr_start() {
  rig_id_ = 0;
  rig_cfg_ = scene3d::VrRig3D{};
  rig_pos_ = {};
  rig_yaw_ = 0.0f;
  for (const Entity3D& e : ents_) {
    if (e.vr_rig) {
      rig_id_ = e.id;
      rig_cfg_ = *e.vr_rig;
      rig_pos_ = e.transform.position;
      rig_yaw_ = e.transform.rotation.to_euler_deg().y;
      break;
    }
  }
  if (rig_id_) {
    player_ = rig_id_;
  } else if (const Entity3D* p = find(player_)) {
    rig_pos_ = p->transform.position;
    rig_pos_.y = vr_floor_at(rig_pos_ + Vec3{0, 1.0f, 0}, 2.0f);
    post(LogLevel::Warn, "vr", "No VR Rig in this scene; standing where " + p->name + " is");
  } else {
    post(LogLevel::Warn, "vr", "No VR Rig in this scene; standing at the origin");
  }
  for (const Entity3D& e : ents_) {
    if (e.grabbable) grab_home_[e.id] = e.transform;
  }
  for (VrHandState& h : hands_) h = VrHandState{};
  tele_ = VrTeleport{};
  menu_open_ = false;
  menu_.clear();
  vignette_ = turn_activity_ = 0.0f;
  snap_latched_ = false;
  bodies_.clear();
  haptics_.clear();
  vr_stats_ = VrStats{};
  if (!vr_in_.head.valid) {
    vr_in_.head.position = {0.0f, 1.7f, 0.0f};
    vr_in_.head.valid = true;
  }
  post(LogLevel::Info, "vr",
       std::string("VR rig ready (") + loco_name(rig_cfg_.locomotion) + ", " +
           (rig_cfg_.snap_turn_deg > 0 ? std::to_string(static_cast<int>(rig_cfg_.snap_turn_deg)) +
                                             " deg snap turn"
                                       : std::string("smooth turn")) +
           (rig_cfg_.vignette ? ", vignette" : "") + ")");
}

Mat4 World3D::rig_matrix() const {
  const Vec3 lift{0.0f, rig_cfg_.seated ? rig_cfg_.seated_offset : 0.0f, 0.0f};
  return Mat4::translate(rig_pos_ + lift) * Mat4::rotate(yaw_quat(rig_yaw_));
}

VrPose World3D::to_world(const VrPose& t) const {
  const Quat q = yaw_quat(rig_yaw_);
  const Vec3 lift{0.0f, rig_cfg_.seated ? rig_cfg_.seated_offset : 0.0f, 0.0f};
  VrPose w;
  w.position = rig_pos_ + lift + q.rotate(t.position);
  w.orientation = render3d::normalize(q * t.orientation);
  w.valid = t.valid;
  return w;
}

void World3D::vr_update_poses() {
  head_ = to_world(vr_in_.head);
  for (int h = 0; h < 2; ++h) {
    hands_[h].grip = to_world(vr_in_.hands[h].grip);
    hands_[h].aim = to_world(vr_in_.hands[h].aim);
    hands_[h].trigger = vr_in_.hands[h].trigger;
    hands_[h].squeeze = vr_in_.hands[h].squeeze;
  }
  // Desktop view of a VR world: through the head.
  camera_.position = head_.position;
  camera_.target = head_.position + head_.forward();
  camera_.explicit_matrices = false;
}

void World3D::vr_follow_held() {
  for (VrHandState& hs : hands_) {
    if (!hs.held) continue;
    Entity3D* e = live(hs.held);
    if (!e) {
      hs.held = 0;
      continue;
    }
    e->transform.position = hs.grip.position + hs.grip.orientation.rotate(hs.hold_offset);
    e->transform.rotation = render3d::normalize(hs.grip.orientation * hs.hold_rotation);
  }
}

void World3D::set_vr_input(const VrInput& in) {
  vr_in_ = in;
  if (!vr_in_.head.valid) {
    vr_in_.head.position = {0.0f, 1.7f, 0.0f};
    vr_in_.head.orientation = {};
    vr_in_.head.valid = true;
  }
  const bool m = in.hands[0].menu || in.hands[1].menu;
  if (m && !menu_held_) vr_menu_latched_ = true;
  menu_held_ = m;
  if (running_ && vr_) {
    // Head, hands and held things follow at display rate, not tick rate.
    vr_update_poses();
    vr_follow_held();
  }
}

Aabb World3D::vr_bounds(const Entity3D& e) const {
  if (e.collider) return collider_box(e);
  const Aabb unit{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  return unit.transformed(e.transform.matrix());
}

float World3D::vr_floor_at(const Vec3& p, float max_step) const {
  float best = -std::numeric_limits<float>::infinity();
  for (const Static& s : statics_) {
    if (p.x < s.box.min.x || p.x > s.box.max.x || p.z < s.box.min.z || p.z > s.box.max.z) continue;
    const float top = s.box.max.y;
    if (top <= rig_pos_.y + max_step && top > best) best = top;
  }
  return std::isfinite(best) ? best : rig_pos_.y;
}

void World3D::vr_teleport_to(const Vec3& p) {
  const Vec3 off = head_.position - rig_pos_;
  rig_pos_.x = p.x - off.x;
  rig_pos_.z = p.z - off.z;
  rig_pos_.y = p.y;
  vr_update_poses();
  vr_follow_held();
}

void World3D::vr_haptic(int hand, float amplitude, float seconds) {
  if (haptics_.size() >= 32) return;
  haptics_.push_back({hand & 1, std::clamp(amplitude, 0.0f, 1.0f), std::clamp(seconds, 0.0f, 2.0f)});
}

std::vector<VrHaptic> World3D::take_haptics() {
  std::vector<VrHaptic> out;
  out.swap(haptics_);
  return out;
}

void World3D::vr_event(std::uint64_t target, const char* hook, int hand, double num) {
  if (!host_ || !target || dead_.count(target) || !host_->running(target)) return;
  host_->hand_event(target, hook, hand_name(hand), num);
}

// ---- Tick ---------------------------------------------------------------------

void World3D::vr_step() {
  const float dt = static_cast<float>(kTickSeconds);
  vr_update_poses();
  if (vr_menu_latched_) {
    vr_menu_latched_ = false;
    set_comfort_menu(!menu_open_);
  }
  vr_locomotion(dt);
  vr_update_poses();
  vr_hands();
  vr_bodies_step(dt);
  vr_update_triggers();
  if (Entity3D* r = live(rig_id_)) {
    r->transform.position = rig_pos_;
    r->transform.rotation = yaw_quat(rig_yaw_);
    r->vr_rig = rig_cfg_;
  }
}

void World3D::vr_locomotion(float dt) {
  using Loco = scene3d::VrRig3D::Locomotion;
  const VrHandInput& L = vr_in_.hands[kLeftHand];
  const VrHandInput& R = vr_in_.hands[kRightHand];
  bool moving = false;

  // Smooth locomotion: left stick, relative to where the head looks.
  if (rig_cfg_.locomotion != Loco::Teleport) {
    const float mag = std::sqrt(L.stick_x * L.stick_x + L.stick_y * L.stick_y);
    if (mag > kDeadZone) {
      const Vec3 f = flat_forward(head_.orientation);
      const Vec3 r{-f.z, 0.0f, f.x};
      Vec3 d = f * L.stick_y + r * L.stick_x;
      if (render3d::length(d) > 1.0f) d = render3d::normalize(d);
      const Vec3 step = d * (rig_cfg_.move_speed * dt);
      auto blocked = [&](const Vec3& head) {
        const Aabb body{{head.x - kBodyRadius, rig_pos_.y + kStepHeight, head.z - kBodyRadius},
                        {head.x + kBodyRadius, std::max(head.y, rig_pos_.y + kStepHeight + 0.1f),
                         head.z + kBodyRadius}};
        for (const Static& s : statics_) {
          if (overlaps(body, s.box)) return true;
        }
        return false;
      };
      Vec3 applied;
      if (!blocked(head_.position + step)) {
        applied = step;
      } else if (!blocked(head_.position + Vec3{step.x, 0, 0})) {
        applied = {step.x, 0, 0};
      } else if (!blocked(head_.position + Vec3{0, 0, step.z})) {
        applied = {0, 0, step.z};
      }
      rig_pos_ += applied;
      const Vec3 head_after = head_.position + applied;
      rig_pos_.y = vr_floor_at(head_after, kStepHeight);
      vr_stats_.smooth_moved += render3d::length(applied);
      moving = render3d::length(applied) > 0.0f;
    }
  }

  // Turning: right stick x (snap, or smooth when snap_turn_deg is 0), about
  // the head so the player does not swing around the rig origin.
  auto turn = [&](float deg) {
    const Vec3 pivot = head_.position;
    rig_yaw_ += deg;
    Vec3 rel = rig_pos_ - pivot;
    rel = yaw_quat(deg).rotate(rel);
    rig_pos_ = {pivot.x + rel.x, rig_pos_.y, pivot.z + rel.z};
    vr_update_poses();
  };
  const float tx = R.stick_x;
  if (!tele_.aiming) {
    if (rig_cfg_.snap_turn_deg > 0.0f) {
      if (!snap_latched_ && std::fabs(tx) > 0.7f) {
        turn(tx > 0 ? -rig_cfg_.snap_turn_deg : rig_cfg_.snap_turn_deg);
        snap_latched_ = true;
        ++vr_stats_.snap_turns;
        turn_activity_ = 1.0f;
      } else if (std::fabs(tx) < 0.35f) {
        snap_latched_ = false;
      }
    } else if (std::fabs(tx) > kDeadZone) {
      turn(-tx * rig_cfg_.smooth_turn_speed * dt);
      turn_activity_ = 1.0f;
    }
  }

  // Teleport: right stick forward aims the arc, releasing it goes.
  if (rig_cfg_.locomotion != Loco::Smooth) {
    const float ty = R.stick_y;
    if (!tele_.aiming && ty > 0.7f && std::fabs(tx) < 0.6f) tele_.aiming = true;
    if (tele_.aiming) {
      tele_.arc.clear();
      tele_.valid = false;
      const VrPose& aim = hands_[kRightHand].aim;
      if (aim.valid) {
        const float speed = std::sqrt(rig_cfg_.teleport_range * kBodyGravity);
        Vec3 p = aim.position;
        Vec3 v = aim.forward() * speed;
        tele_.arc.push_back(p);
        bool any_floor = false;
        for (const Static& s : statics_) any_floor = any_floor || s.box.max.y <= rig_pos_.y + 0.05f;
        for (int i = 0; i < 90; ++i) {
          const float st = 0.03f;
          const Vec3 a = p;
          v.y -= kBodyGravity * st;
          p += v * st;
          bool stop = false;
          for (const Static& s : statics_) {
            if (!inside(s.box, p)) continue;
            stop = true;
            if (a.y >= s.box.max.y - 1e-3f) {
              tele_.target = {p.x, s.box.max.y, p.z};
              tele_.valid = true;
              p = tele_.target;
            }
            break;
          }
          if (!stop && !any_floor && p.y <= 0.0f && a.y > 0.0f) {
            tele_.target = {p.x, 0.0f, p.z};
            tele_.valid = true;
            p = tele_.target;
            stop = true;
          }
          tele_.arc.push_back(p);
          if (stop || p.y < kKillY) break;
        }
        if (tele_.valid) {
          // In range, and room to stand there.
          const Vec3 d = tele_.target - head_.position;
          if (std::sqrt(d.x * d.x + d.z * d.z) > rig_cfg_.teleport_range + 0.5f) tele_.valid = false;
          const Aabb stand{tele_.target + Vec3{-0.15f, 0.1f, -0.15f},
                           tele_.target + Vec3{0.15f, 1.6f, 0.15f}};
          for (const Static& s : statics_) {
            if (overlaps(stand, s.box)) tele_.valid = false;
          }
        }
      }
      if (ty < 0.3f) {
        if (tele_.valid) {
          vr_teleport_to(tele_.target);
          ++vr_stats_.teleports;
          turn_activity_ = 1.0f;
        }
        tele_.aiming = false;
        tele_.valid = false;
        tele_.arc.clear();
      }
    }
  } else {
    tele_ = VrTeleport{};
  }

  turn_activity_ *= 0.85f;
  const float want = rig_cfg_.vignette ? std::max(moving ? 1.0f : 0.0f, turn_activity_) : 0.0f;
  vignette_ += (want - vignette_) * 0.25f;
  if (vignette_ < 0.01f) vignette_ = 0.0f;
}

// ---- Hands ----------------------------------------------------------------------

void World3D::vr_laser(int h) {
  VrHandState& hs = hands_[h];
  const std::uint64_t prev = hs.pointed;
  hs.pointed = 0;
  hs.menu_button = -1;
  hs.laser_hit = false;
  hs.laser_end = hs.aim.position;
  if (hs.aim.valid && !hs.held) {
    const render3d::Ray ray{hs.aim.position, render3d::normalize(hs.aim.forward())};
    float best = kLaserReach;
    if (menu_open_) {
      for (std::size_t i = 0; i < menu_.size(); ++i) {
        float t = 0.0f;
        if (render3d::ray_aabb(ray, menu_[i].box, &t) && t > 0.0f && t < best) {
          best = t;
          hs.menu_button = static_cast<int>(i);
        }
      }
    }
    if (hs.menu_button < 0) {
      for (const Entity3D& e : ents_) {
        if (!e.visible || dead_.count(e.id) || e.vr_rig || (!e.mesh && !e.collider)) continue;
        if (e.id == hands_[0].held || e.id == hands_[1].held) continue;
        float t = 0.0f;
        if (render3d::ray_aabb(ray, vr_bounds(e), &t) && t > 0.0f && t < best) {
          best = t;
          hs.pointed = e.id;
        }
      }
    }
    hs.laser_hit = best < kLaserReach;
    hs.laser_end = ray.origin + ray.dir * best;
  }
  if (hs.pointed != prev) {
    if (prev) vr_event(prev, "on_unpoint", h, 0.0);
    if (hs.pointed) vr_event(hs.pointed, "on_point", h, 0.0);
  }
}

void World3D::vr_hands() {
  for (int h = 0; h < 2; ++h) {
    VrHandState& hs = hands_[h];
    const VrHandInput& in = vr_in_.hands[h];
    if (hs.grip.valid) {
      hs.trail.push_back(hs.grip.position);
      while (hs.trail.size() > kTrail) hs.trail.pop_front();
    } else {
      hs.trail.clear();
    }
    vr_laser(h);
    const bool trig = hs.trigger_down ? in.trigger > 0.55f : in.trigger > 0.75f;
    const bool sq = hs.squeeze_down ? in.squeeze > 0.4f : in.squeeze > 0.7f;
    if (trig && !hs.trigger_down) {
      if (hs.menu_button >= 0) {
        vr_press_menu(hs.menu_button);
        vr_haptic(h, 0.5f, 0.03f);
      } else if (hs.pointed && host_ && host_->running(hs.pointed)) {
        if (host_->has_hook(hs.pointed, "on_vr_trigger")) {
          ++vr_stats_.laser_triggers;
          vr_event(hs.pointed, "on_vr_trigger", h, 0.0);
          vr_haptic(h, 0.35f, 0.03f);
        } else if (host_->has_hook(hs.pointed, "on_interact")) {
          ++vr_stats_.laser_triggers;
          host_->interact(hs.pointed, player_);
          vr_haptic(h, 0.35f, 0.03f);
        }
      }
    }
    hs.trigger_down = trig;
    if (sq && !hs.squeeze_down) vr_grab(h);
    if (!sq && hs.squeeze_down) vr_release(h);
    hs.squeeze_down = sq;
  }
  vr_follow_held();
}

void World3D::vr_grab(int h) {
  VrHandState& hs = hands_[h];
  if (hs.held || !hs.grip.valid) return;
  std::uint64_t best = 0;
  float best_d = kGrabReach;
  for (const Entity3D& e : ents_) {
    if (!e.grabbable || !e.visible || dead_.count(e.id)) continue;
    const float d = distance_to_box(vr_bounds(e), hs.grip.position);
    if (d < best_d) {
      best_d = d;
      best = e.id;
    }
  }
  bool pulled = false;
  if (!best && hs.pointed) {
    // Force grab: the laser is on a grabbable within reach.
    const Entity3D* e = find(hs.pointed);
    if (e && e->grabbable &&
        render3d::length(e->transform.position - hs.aim.position) <= kForceGrabReach) {
      best = e->id;
      pulled = true;
    }
  }
  if (!best) return;
  VrHandState& other = hands_[h ^ 1];
  if (other.held == best) other.held = 0;
  Entity3D* e = live(best);
  if (!e) return;
  bodies_.erase(best);
  const Quat inv = render3d::normalize(hs.grip.orientation).conjugate();
  const bool snap = pulled || (e->grabbable && e->grabbable->snap_to_hand);
  hs.hold_offset = snap ? Vec3{} : inv.rotate(e->transform.position - hs.grip.position);
  hs.hold_rotation = render3d::normalize(inv * e->transform.rotation);
  hs.held = best;
  hs.pointed = 0;
  ++vr_stats_.grabs;
  vr_haptic(h, 0.6f, 0.04f);
  vr_sound("builtin:grab", best, 0.8f, pulled ? 1.25f : 1.0f);
  {
    runtime::FxEvent spark;  // grab sparkle
    spark.preset = "sparkle";
    spark.entity = best;
    spark.at_entity = true;
    fx(std::move(spark));
  }
  post(LogLevel::Info, "vr", std::string(hand_name(h)) + " hand grabbed " + e->name, e->id);
  vr_event(best, "on_grab", h, 0.0);
  vr_follow_held();
}

void World3D::vr_release(int h) {
  VrHandState& hs = hands_[h];
  const std::uint64_t id = hs.held;
  hs.held = 0;
  const Entity3D* e = find(id);
  if (!id || !e || dead_.count(id)) return;
  Vec3 v;
  if (hs.trail.size() >= 2) {
    v = (hs.trail.back() - hs.trail.front()) /
        (static_cast<float>(hs.trail.size() - 1) * static_cast<float>(kTickSeconds));
  }
  const scene3d::Grabbable3D g = e->grabbable ? *e->grabbable : scene3d::Grabbable3D{};
  if (!g.throwable) v = {};
  v *= g.throw_scale;
  const float speed = render3d::length(v);
  bodies_[id].vel = v;
  ++vr_stats_.releases;
  if (speed > 1.5f) ++vr_stats_.throws;
  // A whoosh that grows with the throw; a soft drop otherwise.
  if (speed > 1.5f) {
    vr_sound("builtin:throw", id, std::min(1.0f, 0.4f + speed * 0.08f), 1.0f);
  } else {
    vr_sound("builtin:grab", id, 0.5f, 0.75f);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), " (%.1f m/s)", static_cast<double>(speed));
  post(LogLevel::Info, "vr",
       std::string(hand_name(h)) + " hand " + (speed > 1.5f ? "threw " : "let go of ") + e->name +
           buf,
       id);
  vr_event(id, "on_release", h, speed);
}

// ---- Thrown / dropped things ----------------------------------------------------------

void World3D::vr_bodies_step(float dt) {
  bool any_floor = false;
  for (const Static& s : statics_) any_floor = any_floor || s.box.max.y <= 0.05f;
  for (auto it = bodies_.begin(); it != bodies_.end();) {
    Entity3D* e = live(it->first);
    if (!e || it->first == hands_[0].held || it->first == hands_[1].held) {
      it = bodies_.erase(it);
      continue;
    }
    Body& b = it->second;
    b.vel.y -= kBodyGravity * dt;
    e->transform.position += b.vel * dt;
    bool landed = false;
    for (const Static& s : statics_) {
      const Aabb box = vr_bounds(*e);
      if (!overlaps(box, s.box)) continue;
      const float pen[3] = {std::min(box.max.x - s.box.min.x, s.box.max.x - box.min.x),
                            std::min(box.max.y - s.box.min.y, s.box.max.y - box.min.y),
                            std::min(box.max.z - s.box.min.z, s.box.max.z - box.min.z)};
      int axis = 0;
      if (pen[1] <= pen[axis]) axis = 1;
      if (pen[2] < pen[axis]) axis = 2;
      const float sign = box.center()[axis] >= s.box.center()[axis] ? 1.0f : -1.0f;
      e->transform.position[axis] += pen[axis] * sign;
      if (axis == 1 && sign > 0) {
        if (b.vel.y < 0) b.vel.y = -b.vel.y * 0.25f;
        if (b.vel.y < 0.6f) {
          b.vel.y = 0.0f;
          landed = true;
        }
        b.vel.x *= 0.8f;
        b.vel.z *= 0.8f;
      } else {
        b.vel[axis] = -b.vel[axis] * 0.3f;
      }
    }
    if (!any_floor) {
      const Aabb box = vr_bounds(*e);
      if (box.min.y < 0.0f) {
        e->transform.position.y -= box.min.y;
        if (b.vel.y < 0) b.vel.y = 0;
        b.vel.x *= 0.8f;
        b.vel.z *= 0.8f;
        landed = true;
      }
    }
    if (e->transform.position.y < kKillY) {
      auto home = grab_home_.find(e->id);
      if (home != grab_home_.end()) e->transform = home->second;
      post(LogLevel::Warn, "vr", e->name + " fell off the edge of the map; back where it was", e->id);
      it = bodies_.erase(it);
      continue;
    }
    if (landed && render3d::length(b.vel) < 0.15f) {
      ++vr_stats_.landings;
      it = bodies_.erase(it);
      continue;
    }
    ++it;
  }
}

void World3D::vr_update_triggers() {
  std::vector<const Entity3D*> triggers;
  for (const Entity3D& e : ents_) {
    if (e.collider && e.collider->trigger && !dead_.count(e.id) && !collider_off_.count(e.id)) {
      triggers.push_back(&e);
    }
  }
  // Thrown / dropped things.
  for (auto& [id, b] : bodies_) {
    const Entity3D* e = find(id);
    if (!e) continue;
    const Aabb box = vr_bounds(*e);
    std::set<std::uint64_t> now;
    for (const Entity3D* t : triggers) {
      if (t->id != id && overlaps(box, collider_box(*t))) now.insert(t->id);
    }
    for (std::uint64_t t : now) {
      if (b.inside.count(t)) continue;
      ++trigger_events_;
      const Entity3D* te = find(t);
      post(LogLevel::Info, "trigger", e->name + " flew into " + (te ? te->name : "?"), t);
      if (host_) host_->trigger(true, t, id);
    }
    for (std::uint64_t t : b.inside) {
      if (now.count(t)) continue;
      ++trigger_events_;
      if (host_) host_->trigger(false, t, id);
    }
    b.inside = std::move(now);
  }
  // The player: a column from the rig floor up to the head.
  if (!player_) return;
  const Aabb me{{head_.position.x - 0.25f, rig_pos_.y, head_.position.z - 0.25f},
                {head_.position.x + 0.25f, std::max(head_.position.y, rig_pos_.y + 0.5f),
                 head_.position.z + 0.25f}};
  std::set<std::uint64_t> now;
  for (const Entity3D* t : triggers) {
    if (t->id != player_ && overlaps(me, collider_box(*t))) now.insert(t->id);
  }
  for (std::uint64_t id : now) {
    if (inside_.count(id)) continue;
    ++trigger_events_;
    const Entity3D* t = find(id);
    post(LogLevel::Info, "trigger", "The player walked into " + (t ? t->name : std::string("?")), id);
    if (host_) host_->trigger(true, id, player_);
  }
  for (std::uint64_t id : inside_) {
    if (now.count(id)) continue;
    ++trigger_events_;
    if (host_) host_->trigger(false, id, player_);
  }
  inside_ = std::move(now);
}

// ---- Comfort menu ----------------------------------------------------------------------

void World3D::set_comfort_menu(bool open) {
  menu_open_ = open;
  if (open) {
    const Vec3 f = flat_forward(head_.orientation);
    menu_anchor_ = head_.position + f * 0.6f + Vec3{0.0f, -0.2f, 0.0f};
    menu_yaw_ = render3d::degrees(std::atan2(-f.x, -f.z));
    vr_layout_menu();
  } else {
    menu_.clear();
  }
  post(LogLevel::Info, "vr", open ? "Comfort menu open (point + trigger to change)"
                                  : "Comfort menu closed");
}

void World3D::vr_layout_menu() {
  using K = VrMenuButton::Kind;
  const Quat q = yaw_quat(menu_yaw_);
  const Vec3 right = q.rotate({1, 0, 0});
  const Vec3 fwd = q.rotate({0, 0, -1});
  const K kinds[4] = {K::Vignette, K::SnapTurn, K::Locomotion, K::Seated};
  menu_.clear();
  for (int i = 0; i < 4; ++i) {
    VrMenuButton b;
    b.kind = kinds[i];
    const Vec3 c = menu_anchor_ + right * ((static_cast<float>(i) - 1.5f) * 0.24f);
    const Vec3 half{std::fabs(right.x) * 0.1f + std::fabs(fwd.x) * 0.02f, 0.06f,
                    std::fabs(right.z) * 0.1f + std::fabs(fwd.z) * 0.02f};
    b.box = {c - half, c + half};
    switch (b.kind) {
      case K::Vignette:
        b.on = rig_cfg_.vignette;
        b.label = std::string("Vignette: ") + (b.on ? "on" : "off");
        break;
      case K::SnapTurn:
        b.on = rig_cfg_.snap_turn_deg > 0.0f;
        b.label = b.on ? "Turn: " + std::to_string(static_cast<int>(rig_cfg_.snap_turn_deg)) +
                             " deg snap"
                       : std::string("Turn: smooth");
        break;
      case K::Locomotion:
        b.on = rig_cfg_.locomotion != scene3d::VrRig3D::Locomotion::Smooth;
        b.label = std::string("Move: ") + loco_name(rig_cfg_.locomotion);
        break;
      case K::Seated:
        b.on = rig_cfg_.seated;
        b.label = b.on ? "Seated" : "Standing";
        break;
    }
    menu_.push_back(std::move(b));
  }
}

void World3D::vr_sound(const char* clip, std::uint64_t entity, float volume, float pitch) {
  runtime::SoundEvent ev;
  ev.clip = clip;
  ev.volume = volume;
  ev.pitch = pitch;
  ev.variation = 0.08f;
  ev.positional = entity != 0;
  ev.entity = entity;
  if (entity == 0) ev.bus = "ui";
  play_sound(std::move(ev));
}

void World3D::vr_press_menu(int button) {
  if (button < 0 || button >= static_cast<int>(menu_.size())) return;
  vr_sound("builtin:click", 0, 0.7f, 1.0f);
  using K = VrMenuButton::Kind;
  using Loco = scene3d::VrRig3D::Locomotion;
  switch (menu_[button].kind) {
    case K::Vignette:
      rig_cfg_.vignette = !rig_cfg_.vignette;
      break;
    case K::SnapTurn: {
      const float s = rig_cfg_.snap_turn_deg;
      rig_cfg_.snap_turn_deg = s <= 0.0f ? 30.0f : s < 40.0f ? 45.0f : s < 80.0f ? 90.0f : 0.0f;
      break;
    }
    case K::Locomotion:
      rig_cfg_.locomotion = rig_cfg_.locomotion == Loco::Both       ? Loco::Teleport
                            : rig_cfg_.locomotion == Loco::Teleport ? Loco::Smooth
                                                                    : Loco::Both;
      break;
    case K::Seated:
      rig_cfg_.seated = !rig_cfg_.seated;
      break;
  }
  ++vr_stats_.menu_presses;
  vr_layout_menu();
  post(LogLevel::Info, "vr", "Comfort: " + menu_[button].label);
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
