// World3D on Jolt (ts_physics3d): bodies from colliders, the rider's
// character, held / carried bodies driven by velocity, impacts, sensors,
// joints and the script-facing physics calls. See World3D.h.

#include "world3d/World3D.h"

#include "render3d/Gltf.h"
#include "render3d/Mesh.h"
#include "runtime/ScriptHost.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace world3d {

using render3d::Quat;
using runtime::LogLevel;
namespace p3 = physics3d;

namespace {

constexpr float kHoldSpeed = 25.0f;      // m/s: fastest a held thing chases the hand
constexpr float kHoldSpin = 40.0f;       // rad/s
constexpr float kHoldBreak = 1.0f;       // m: further than this and it snaps back
constexpr float kCarryReach = 4.0f;      // m: desktop pick-up reach from the chest
constexpr float kCarryThrow = 12.0f;     // m/s: desktop throw
constexpr int kImpactQuietTicks = 8;     // per entity between impact sounds
constexpr float kRestSpeed = 0.15f;      // m/s: "landed"

float max3(const Vec3& v) { return std::max(v.x, std::max(v.y, v.z)); }

// Shape from a collider primitive, sized by the entity scale.
p3::ShapeDesc prim_shape(scene3d::ColliderShape3D shape, const Vec3& size, const Vec3& scale) {
  p3::ShapeDesc d;
  const Vec3 s{std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)};
  switch (shape) {
    case scene3d::ColliderShape3D::Capsule:
      d.type = p3::ShapeType::Capsule;
      d.size = {size.x * std::max(s.x, s.z), size.y * s.y, 0.0f};
      break;
    case scene3d::ColliderShape3D::Sphere:
      d.type = p3::ShapeType::Sphere;
      d.size = {size.x * max3(s), 0.0f, 0.0f};
      break;
    default:
      d.type = p3::ShapeType::Box;
      d.size = render3d::mul(size, s);
      break;
  }
  return d;
}

// Velocity that closes the gap to the target in one tick (clamped).
Vec3 chase(const Vec3& from, const Vec3& to, float dt, float max_speed) {
  Vec3 v = (to - from) / dt;
  const float l = render3d::length(v);
  if (l > max_speed) v = v * (max_speed / l);
  return v;
}

Vec3 spin_to(const Quat& from, const Quat& to, float dt, float max_spin) {
  Quat d = render3d::normalize(to * from.conjugate());
  if (d.w < 0.0f) d = {-d.x, -d.y, -d.z, -d.w};
  const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (s < 1e-6f) return {};
  const float angle = 2.0f * std::atan2(s, d.w);
  Vec3 w = Vec3{d.x, d.y, d.z} * (angle / s / dt);
  const float l = render3d::length(w);
  if (l > max_spin) w = w * (max_spin / l);
  return w;
}

}  // namespace

// ---- Bodies -----------------------------------------------------------------

p3::BodyId World3D::body_of(std::uint64_t id) const {
  auto it = body_ids_.find(id);
  return it == body_ids_.end() ? 0 : it->second;
}

std::uint64_t World3D::entity_of(p3::BodyId body) const {
  auto it = body_owner_.find(body);
  return it == body_owner_.end() ? 0 : it->second;
}

bool World3D::phys_wants_body(const Entity3D& e) const {
  if (e.player || e.vr_rig || dead_.count(e.id) || collider_off_.count(e.id)) return false;
  if (e.collider) return true;
  // A grabbable without a collider uses its unit primitive's box.
  return e.grabbable.has_value() || e.rigid_body.has_value();
}

const render3d::MeshData* World3D::phys_mesh(const std::string& source) {
  auto it = phys_meshes_.find(source);
  if (it != phys_meshes_.end()) return it->second.get();
  auto mesh = std::make_unique<render3d::MeshData>();
  bool ok = false;
  if (source.rfind("primitive:", 0) == 0) {
    ok = render3d::make_primitive(source, mesh.get());
  } else if (!source.empty()) {
    render3d::Model model;
    std::string err;
    const std::string path = (std::filesystem::path(project_dir_) / source).string();
    if (render3d::load_gltf(path, &model, &err)) {
      for (const render3d::ModelPrimitive& p : model.primitives) {
        const std::uint32_t base = static_cast<std::uint32_t>(mesh->vertices.size());
        mesh->vertices.insert(mesh->vertices.end(), p.mesh.vertices.begin(), p.mesh.vertices.end());
        for (std::uint32_t i : p.mesh.indices) mesh->indices.push_back(base + i);
      }
      ok = mesh->valid();
    }
  }
  if (!ok) mesh.reset();
  const render3d::MeshData* out = mesh.get();
  phys_meshes_[source] = std::move(mesh);
  return out;
}

void World3D::phys_add(const Entity3D& e) {
  if (!phys_ || body_ids_.count(e.id)) return;
  const scene3d::Collider3D c = e.collider ? *e.collider : scene3d::Collider3D{};
  p3::BodyDesc d;
  d.user = e.id;
  d.position = e.transform.position;
  d.rotation = render3d::normalize(e.transform.rotation);
  d.sensor = c.trigger;
  if (e.rigid_body) {
    const scene3d::RigidBody3D& rb = *e.rigid_body;
    d.motion = rb.motion == scene3d::RigidBody3D::Motion::Static      ? p3::Motion::Static
               : rb.motion == scene3d::RigidBody3D::Motion::Kinematic ? p3::Motion::Kinematic
                                                                       : p3::Motion::Dynamic;
    d.mass = rb.mass;
    d.friction = rb.friction;
    d.restitution = rb.restitution;
    d.linear_damping = rb.linear_damping;
    d.angular_damping = rb.angular_damping;
    d.gravity_scale = rb.gravity_scale;
    d.ccd = rb.ccd;
  } else if (e.grabbable) {
    // Props: dynamic, swept (they get thrown), light enough to wave about.
    d.motion = p3::Motion::Dynamic;
    d.ccd = true;
    const Vec3 sz = render3d::mul(c.size, e.transform.scale);
    d.mass = std::clamp(std::fabs(sz.x * sz.y * sz.z) * 400.0f, 0.2f, 8.0f);
    d.restitution = 0.15f;
  } else {
    d.motion = p3::Motion::Static;
  }
  d.layer = static_cast<std::uint8_t>(c.layer >= 0 ? c.layer
                                       : c.trigger ? p3::kLayerTrigger
                                       : d.motion == p3::Motion::Static ? p3::kLayerStatic
                                                                        : p3::kLayerDefault);
  const Vec3& scale = e.transform.scale;
  const bool hull = c.shape == scene3d::ColliderShape3D::ConvexHull;
  const bool tri = c.shape == scene3d::ColliderShape3D::Mesh;
  if (hull || tri) {
    const render3d::MeshData* m = e.mesh ? phys_mesh(e.mesh->source) : nullptr;
    p3::ShapeDesc s;
    s.type = hull || d.motion == p3::Motion::Dynamic ? p3::ShapeType::ConvexHull : p3::ShapeType::Mesh;
    if (tri && d.motion == p3::Motion::Dynamic) {
      post(LogLevel::Warn, "physics",
           e.name + ": a mesh collider can only be static; using its convex hull", e.id);
    }
    if (m) {
      s.points.reserve(m->vertices.size());
      for (const auto& v : m->vertices) {
        s.points.push_back(render3d::mul(Vec3{v.position.x, v.position.y, v.position.z}, scale));
      }
      s.indices = m->indices;
    }
    s.offset = render3d::mul(c.offset, scale);
    d.shapes.push_back(std::move(s));
  } else {
    p3::ShapeDesc s = prim_shape(c.shape, c.size, scale);
    s.offset = render3d::mul(c.offset, scale);
    d.shapes.push_back(std::move(s));
  }
  for (const scene3d::ColliderPart3D& part : c.parts) {
    p3::ShapeDesc s = prim_shape(part.shape, part.size, scale);
    s.offset = render3d::mul(part.offset, scale);
    s.rotation = Quat::from_euler_deg(part.rotation_deg);
    d.shapes.push_back(std::move(s));
  }
  std::string err;
  p3::BodyId id = phys_->create(d, &err);
  if (!id && (hull || tri)) {
    // Bad mesh data: fall back to the collider box so it still collides.
    post(LogLevel::Warn, "physics", e.name + ": " + err + "; using a box", e.id);
    d.shapes.clear();
    d.shapes.push_back(prim_shape(scene3d::ColliderShape3D::Box, c.size, scale));
    id = phys_->create(d, &err);
  }
  if (!id) {
    post(LogLevel::Warn, "physics", e.name + ": no body (" + err + ")", e.id);
    return;
  }
  body_ids_[e.id] = id;
  body_owner_[id] = e.id;
  synced_[e.id] = e.transform;
}

void World3D::phys_remove(std::uint64_t id) {
  auto it = body_ids_.find(id);
  if (it == body_ids_.end()) return;
  for (auto j = joint_ids_.begin(); j != joint_ids_.end();) {
    // phys_->destroy() drops the joints on the body; forget our handles.
    if (j->first == id) {
      j = joint_ids_.erase(j);
    } else {
      ++j;
    }
  }
  if (phys_) phys_->destroy(it->second);
  body_owner_.erase(it->second);
  body_ids_.erase(it);
  synced_.erase(id);
  flying_.erase(id);
  if (carry_.id == id) carry_.id = 0;
}

void World3D::phys_build() {
  p3::Settings ps;
  const scene3d::PhysicsSettings3D& cfg = authored_.physics;
  ps.gravity = cfg.gravity;
  ps.collision_steps = cfg.collision_steps;
  for (int i = 0; i < p3::kLayerCount; ++i) ps.collide[i] = cfg.collide[i];
  phys_ = std::make_unique<p3::World>(ps);
  for (const Entity3D& e : ents_) {
    if (phys_wants_body(e)) phys_add(e);
  }
  // Joints (this entity's body to its target, or the world).
  for (const Entity3D& e : ents_) {
    if (!e.joint) continue;
    std::string err;
    const p3::JointId j = phys_joint(*e.joint, e.joint->target, e.id, &err);
    if (j) {
      joint_ids_[e.id] = j;
    } else {
      post(LogLevel::Warn, "physics", e.name + ": joint not made (" + err + ")", e.id);
    }
  }
  // The desktop rider rides a Jolt character.
  if (!vr_) {
    if (const Entity3D* p = find(player_)) {
      p3::CharacterDesc cd;
      if (p->collider) {
        cd.radius = p->collider->shape == scene3d::Collider3D::Shape::Capsule
                        ? p->collider->size.x
                        : 0.5f * std::max(p->collider->size.x, p->collider->size.z);
        cd.height = p->collider->size.y;
        cd.offset = p->collider->offset;
      }
      phys_->create_character(cd, p->transform.position);
      synced_[player_] = p->transform;
    }
  }
}

physics3d::JointId World3D::phys_joint(const scene3d::Joint3D& j, std::uint64_t a,
                                       std::uint64_t b, std::string* error) {
  if (!phys_) return 0;
  const p3::BodyId ba = a ? body_of(a) : 0;
  const p3::BodyId bb = body_of(b);
  if ((a && !ba) || !bb) {
    if (error) *error = "both ends need a collider / body";
    return 0;
  }
  const Entity3D* eb = find(b);
  p3::JointDesc d;
  d.a = ba;
  d.b = bb;
  d.type = static_cast<p3::JointType>(j.type);
  d.anchor = eb ? eb->transform.matrix().transform_point(j.anchor) : j.anchor;
  d.axis = eb ? eb->transform.rotation.rotate(j.axis) : j.axis;
  d.limits = j.limits;
  if (j.type == scene3d::Joint3D::Type::Hinge) {
    d.min = render3d::radians(j.min);
    d.max = render3d::radians(j.max);
  } else {
    d.min = j.min;
    d.max = j.max;
  }
  if (j.type == scene3d::Joint3D::Type::Distance) {
    // Chain from the anchor on b to the other body's centre (or the
    // anchor's spot in the world, pinned there).
    const Entity3D* ea = a ? find(a) : nullptr;
    d.anchor_b = d.anchor;
    d.anchor = ea ? ea->transform.position : d.anchor + Vec3{0, 0.001f, 0};
    if (!j.limits) {
      d.limits = true;
      d.min = d.max = render3d::length(d.anchor_b - d.anchor);
    }
  }
  return phys_->add_joint(d, error);
}

void World3D::phys_unjoint(physics3d::JointId id) {
  if (!phys_) return;
  phys_->remove_joint(id);
  for (auto it = joint_ids_.begin(); it != joint_ids_.end(); ++it) {
    if (it->second == id) {
      joint_ids_.erase(it);
      break;
    }
  }
}

// ---- Tick -------------------------------------------------------------------

void World3D::phys_pre(float dt) {
  if (!phys_) return;
  // Bodies come and go with colliders switched off / on, kills and spawns.
  for (const Entity3D& e : ents_) {
    const bool want = phys_wants_body(e);
    const bool have = body_ids_.count(e.id) != 0;
    if (want && !have) phys_add(e);
    if (!want && have) phys_remove(e.id);
  }
  for (auto it = body_ids_.begin(); it != body_ids_.end();) {
    const std::uint64_t id = it->first;
    ++it;
    if (!find(id)) phys_remove(id);
  }
  // Scripts / parents moved an entity: the body follows.
  for (const Entity3D& e : ents_) {
    auto b = body_ids_.find(e.id);
    if (b == body_ids_.end()) continue;
    auto was = synced_.find(e.id);
    if (was == synced_.end()) continue;
    if (was->second == e.transform) continue;
    if (!(was->second.scale == e.transform.scale)) {
      // A new size needs a new shape.
      phys_remove(e.id);
      phys_add(e);
      continue;
    }
    if (phys_->motion(b->second) == p3::Motion::Kinematic) {
      phys_->move_kinematic(b->second, e.transform.position, e.transform.rotation, dt);
    } else {
      phys_->set_transform(b->second, e.transform.position, e.transform.rotation);
    }
    synced_[e.id] = e.transform;
  }
}

void World3D::phys_step(float dt) {
  if (phys_) phys_->step(dt);
}

void World3D::phys_post() {
  if (!phys_) return;
  // Moving bodies write their pose back (scale is the entity's own).
  for (auto& [id, body] : body_ids_) {
    if (phys_->motion(body) != p3::Motion::Dynamic) continue;
    Entity3D* e = live(id);
    if (!e) continue;
    e->transform.position = phys_->position(body);
    e->transform.rotation = phys_->rotation(body);
    if (e->transform.position.y < kKillY) {
      auto home = grab_home_.find(id);
      Transform t = home != grab_home_.end() ? home->second : e->transform;
      if (home == grab_home_.end()) {
        if (const Entity3D* a = authored_.find(id)) t = a->transform;
      }
      t.scale = e->transform.scale;
      e->transform = t;
      phys_->set_transform(body, t.position, t.rotation);
      phys_->set_velocity(body, {});
      phys_->set_angular_velocity(body, {});
      flying_.erase(id);
      post(LogLevel::Warn, "physics", e->name + " fell off the edge of the map; back where it was", id);
    }
    synced_[id] = e->transform;
  }
  // Thrown / dropped things that came to rest.
  for (auto it = flying_.begin(); it != flying_.end();) {
    const p3::BodyId b = body_of(*it);
    if (!b || phys_held(*it)) {
      it = flying_.erase(it);
      continue;
    }
    if (!phys_->awake(b) || render3d::length(phys_->velocity(b)) < kRestSpeed) {
      ++vr_stats_.landings;
      it = flying_.erase(it);
      continue;
    }
    ++it;
  }
  // Impacts.
  for (const p3::Contact& c : phys_->contacts()) {
    if (c.speed < kImpactSpeed) continue;
    const std::uint64_t ea = c.user_a, eb = c.user_b;
    const Entity3D* a = find(ea);
    const Entity3D* b = find(eb);
    auto loud = [&](const Entity3D* e) {
      return e && (!e->rigid_body || e->rigid_body->impact_events) &&
             (e->grabbable || e->rigid_body);
    };
    if (!loud(a) && !loud(b)) continue;
    ++impact_stats_.impacts;
    impact_stats_.max_speed = std::max(impact_stats_.max_speed, c.speed);
    impact_stats_.last_a = ea;
    impact_stats_.last_b = eb;
    const bool held = phys_held(ea) || phys_held(eb);
    if (held) ++impact_stats_.held_hits;
    // Hooks fire on both sides (quiet period per entity keeps it sane).
    const std::uint64_t now = tick_;
    bool sounded = false;
    for (int side = 0; side < 2; ++side) {
      const Entity3D* me = side ? b : a;
      const Entity3D* other = side ? a : b;
      if (!loud(me)) continue;
      auto& until = impact_quiet_until_[me->id];
      if (now < until) continue;
      until = now + kImpactQuietTicks;
      if (host_ && host_->running(me->id) && host_->has_hook(me->id, "on_impact")) {
        host_->hand_event(me->id, "on_impact", other ? other->name : std::string("the ground"),
                          c.speed);
      }
      if (!sounded) {
        sounded = true;
        runtime::SoundEvent ev;
        ev.clip = "builtin:impact";
        ev.volume = std::clamp(0.25f + c.speed * 0.09f, 0.25f, 1.0f);
        ev.pitch = std::clamp(1.4f - 0.06f * c.impulse, 0.6f, 1.4f);
        ev.variation = 0.1f;
        ev.positional = true;
        ev.three_d = true;
        ev.x = c.point.x;
        ev.y = c.point.y;
        ev.z = c.point.z;
        ev.entity = me->id;
        play_sound(std::move(ev));
        if (c.speed > 2.5f) {
          runtime::FxEvent fx_ev;
          fx_ev.preset = "dust";
          fx_ev.x = c.point.x;
          fx_ev.y = c.point.y;
          fx_ev.z = c.point.z;
          fx_ev.count = std::clamp(static_cast<int>(c.speed * 2.0f), 3, 24);
          fx(std::move(fx_ev));
        }
      }
    }
    // The holding hand feels it.
    for (int h = 0; h < 2; ++h) {
      if (hands_[h].held && (hands_[h].held == ea || hands_[h].held == eb)) {
        vr_haptic(h, std::clamp(c.speed / 6.0f, 0.2f, 1.0f), 0.04f);
      }
    }
  }
  // Bodies in triggers (the rider's own checks stay in update_triggers /
  // vr_update_triggers).
  for (const p3::SensorEvent& ev : phys_->sensor_events()) {
    const std::uint64_t trig = ev.sensor_user, other = ev.other_user;
    if (!trig || !other || other == player_ || phys_->sensor(ev.other)) continue;
    const Entity3D* t = find(trig);
    const Entity3D* o = find(other);
    if (!t || !o) continue;
    ++trigger_events_;
    if (ev.enter) post(LogLevel::Info, "trigger", o->name + " flew into " + t->name, trig);
    if (host_) host_->trigger(ev.enter, trig, other);
  }
}

// ---- Holding ----------------------------------------------------------------

bool World3D::phys_held(std::uint64_t id) const {
  if (!id) return false;
  return hands_[0].held == id || hands_[1].held == id || carry_.id == id;
}

void World3D::phys_on_hold(std::uint64_t id, bool held) {
  const p3::BodyId b = body_of(id);
  if (!b || !phys_) return;
  if (held) {
    phys_->set_gravity_scale(b, 0.0f);
    phys_->set_layer(b, p3::kLayerHeld);
    phys_->wake(b);
  } else if (!phys_held(id)) {
    const Entity3D* e = find(id);
    phys_->set_gravity_scale(b, e && e->rigid_body ? e->rigid_body->gravity_scale : 1.0f);
    const int layer = e && e->collider && e->collider->layer >= 0 ? e->collider->layer
                                                                   : p3::kLayerDefault;
    phys_->set_layer(b, static_cast<std::uint8_t>(layer));
  }
}

void World3D::phys_drive(std::uint64_t id, const Vec3& p, const Quat& q, float dt,
                         float max_speed) {
  const p3::BodyId b = body_of(id);
  if (!b || !phys_) return;
  if (phys_->motion(b) != p3::Motion::Dynamic) {
    phys_->set_transform(b, p, q);
    if (Entity3D* e = live(id)) {
      e->transform.position = p;
      e->transform.rotation = q;
      synced_[id] = e->transform;
    }
    return;
  }
  phys_->set_velocity(b, chase(phys_->position(b), p, dt, max_speed));
  phys_->set_angular_velocity(b, spin_to(phys_->rotation(b), q, dt, kHoldSpin));
}

void World3D::vr_drive_held(float dt) {
  if (!phys_) return;
  for (int h = 0; h < 2; ++h) {
    VrHandState& hs = hands_[h];
    if (!hs.held) continue;
    const p3::BodyId b = body_of(hs.held);
    if (!b) continue;
    Vec3 target = hs.grip.position + hs.grip.orientation.rotate(hs.hold_offset);
    Quat rot = render3d::normalize(hs.grip.orientation * hs.hold_rotation);
    const VrHandState& other = hands_[h ^ 1];
    if (other.held == hs.held) {
      if (h == 1) continue;  // the left hand drives a two-handed hold
      const Vec3 t2 = other.grip.position + other.grip.orientation.rotate(other.hold_offset);
      target = (target + t2) * 0.5f;
    }
    const float gap = render3d::length(phys_->position(b) - target);
    if (hs.pulling && gap < 0.3f) hs.pulling = false;
    if (!hs.pulling && gap > kHoldBreak) {
      // Left behind (teleport, wedged behind a wall): back into the hand.
      phys_->set_transform(b, target, rot);
      phys_->set_velocity(b, {});
      continue;
    }
    phys_drive(hs.held, target, rot, dt, kHoldSpeed);
  }
}

void World3D::phys_carry(const Input3D& in, float dt) {
  if (!phys_) return;
  const Entity3D* p = find(player_);
  const bool grab_edge = in.grab && !carry_.grab_down;
  const bool throw_edge = in.throw_held && !carry_.throw_down;
  carry_.grab_down = in.grab;
  carry_.throw_down = in.throw_held;
  const Vec3 eye = p ? p->transform.position + Vec3{0, 0.6f, 0} : camera_.position;
  Vec3 dir = render3d::normalize(camera_.target - camera_.position);
  dir.y = std::clamp(dir.y + 0.15f, -0.6f, 0.6f);  // the follow cam looks down; aim level-ish
  dir = render3d::normalize(dir);
  const float yaw = render3d::radians(cam_yaw_);
  const Quat cam_q = Quat::axis_angle({0, 1, 0}, yaw);
  if (grab_edge && !carry_.id) {
    p3::RayHit hit;
    p3::BodyId pick = 0;
    if (phys_->sphere_cast(eye, 0.25f, dir, kCarryReach, &hit) &&
        phys_->motion(hit.body) == p3::Motion::Dynamic) {
      pick = hit.body;
    } else if (p) {
      // Nothing on the line of sight: the nearest loose thing just ahead
      // of the rider (the follow camera looks down on it).
      const Vec3 fwd = render3d::normalize(Vec3{dir.x, 0.0f, dir.z});
      const Vec3 c = p->transform.position + fwd * 1.3f + Vec3{0, -0.4f, 0};
      float best = 1e9f;
      for (p3::BodyId b : phys_->overlap_sphere(c, 1.0f)) {
        if (phys_->motion(b) != p3::Motion::Dynamic || !entity_of(b)) continue;
        const float d = render3d::length(phys_->position(b) - c);
        if (d < best) {
          best = d;
          pick = b;
        }
      }
    }
    if (pick) {
      hit.body = pick;
      const std::uint64_t id = entity_of(hit.body);
      if (id) {
        carry_.id = id;
        carry_.distance = std::clamp(render3d::length(phys_->position(hit.body) - eye), 1.2f, 3.0f);
        carry_.rel = render3d::normalize(cam_q.conjugate() * phys_->rotation(hit.body));
        phys_on_hold(id, true);
        flying_.erase(id);
        const Entity3D* e = find(id);
        vr_sound("builtin:grab", id, 0.8f, 1.0f);
        post(LogLevel::Info, "physics", "Picked up " + (e ? e->name : std::string("?")), id);
        vr_event(id, "on_grab", kRightHand, 0.0);
      }
    }
  }
  if (!carry_.id) return;
  const std::uint64_t id = carry_.id;
  const p3::BodyId b = body_of(id);
  if (!b) {
    carry_.id = 0;
    return;
  }
  if (throw_edge || !in.grab) {
    carry_.id = 0;
    phys_on_hold(id, false);
    Vec3 v = phys_->velocity(b);
    if (throw_edge) {
      v = dir * kCarryThrow + Vec3{0, 1.5f, 0};
      vr_sound("builtin:throw", id, 0.8f, 1.0f);
    }
    phys_->set_velocity(b, v);
    flying_.insert(id);
    vr_event(id, "on_release", kRightHand, render3d::length(v));
    return;
  }
  const Vec3 target = eye + dir * carry_.distance;
  if (render3d::length(phys_->position(b) - target) > kHoldBreak + carry_.distance) {
    // Wedged: drop it.
    carry_.id = 0;
    phys_on_hold(id, false);
    return;
  }
  phys_drive(id, target, render3d::normalize(cam_q * carry_.rel), dt, kHoldSpeed);
}

std::size_t World3D::vr_bodies() const {
  std::size_t n = 0;
  for (std::uint64_t id : flying_) {
    if (!phys_held(id)) ++n;
  }
  return n;
}

// ---- Script calls ---------------------------------------------------------------

bool World3D::phys_apply(std::uint64_t id, const char* what, const Vec3& v) {
  const p3::BodyId b = body_of(id);
  if (!b || !phys_ || phys_->motion(b) != p3::Motion::Dynamic) return false;
  const std::string w = what;
  if (w == "force") {
    phys_->add_force(b, v);
  } else if (w == "impulse") {
    phys_->add_impulse(b, v);
    flying_.insert(id);
  } else if (w == "torque") {
    phys_->add_torque(b, v);
  } else if (w == "angular") {
    phys_->set_angular_velocity(b, v);
  } else {
    return false;
  }
  phys_->wake(b);
  return true;
}

bool World3D::phys_set_velocity(std::uint64_t id, const Vec3& v) {
  const p3::BodyId b = body_of(id);
  if (!b || !phys_ || phys_->motion(b) == p3::Motion::Static) return false;
  phys_->set_velocity(b, v);
  return true;
}

Vec3 World3D::phys_velocity(std::uint64_t id) const {
  if (id == player_ && phys_ && phys_->has_character()) return vel_;
  const p3::BodyId b = body_of(id);
  return b && phys_ ? phys_->velocity(b) : Vec3{};
}

bool World3D::phys_raycast(const Vec3& from, const Vec3& dir, float dist, p3::RayHit* hit) const {
  return phys_ && phys_->raycast(from, dir, dist, hit);
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
