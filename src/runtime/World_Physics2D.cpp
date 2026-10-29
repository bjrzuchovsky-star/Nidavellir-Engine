// The 2D World's Box2D side: "body" entities are rigid bodies; riders,
// pushables and static solids get proxies so bodies collide with them, and
// solid tiles become one static body. Riders keep the deterministic
// platformer collision (they push dynamic bodies through their kinematic
// proxies; static / kinematic bodies block them in resolve_axis).

#include "runtime/World.h"

#include <cmath>
#include <algorithm>
#include <map>
#include <string>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

namespace p2 = physics2d;

p2::Motion motion_of(RigidBody2DData::Motion m) {
  switch (m) {
    case RigidBody2DData::Motion::Static: return p2::Motion::Static;
    case RigidBody2DData::Motion::Kinematic: return p2::Motion::Kinematic;
    default: return p2::Motion::Dynamic;
  }
}

// The entity's collider (or rect) as a shape around the entity centre.
p2::ShapeDesc shape_of(const Entity2D& e) {
  const Aabb a = collider_box(e, 0.0f, 0.0f);
  p2::ShapeDesc s;
  s.w = std::max(0.5f, a.x1 - a.x0);
  s.h = std::max(0.5f, a.y1 - a.y0);
  s.offset = {(a.x0 + a.x1) * 0.5f - e.w * 0.5f, (a.y0 + a.y1) * 0.5f - e.h * 0.5f};
  const ColliderData::Shape k = e.collider ? e.collider->shape : ColliderData::Shape::Box;
  if (k == ColliderData::Shape::Circle) {
    s.type = p2::ShapeType::Circle;
    s.radius = 0.5f * std::min(s.w, s.h);
  } else if (k == ColliderData::Shape::Capsule) {
    s.type = p2::ShapeType::Capsule;
  }
  return s;
}

float rot_of(const Entity2D& e) { return turned(e) ? e.rotation : 0.0f; }

void append_shapes(const Entity2D& e, std::vector<p2::ShapeDesc>* out) {
  if (e.collider && e.collider->shape == ColliderData::Shape::Polygon && !e.collider->polygons.empty()) {
    for (const auto& piece : e.collider->polygons) {
      p2::ShapeDesc s;
      s.type = p2::ShapeType::Polygon;
      s.points.reserve(piece.size());
      for (const auto& [x, y] : piece) s.points.push_back({x - e.w * 0.5f, y - e.h * 0.5f});
      out->push_back(std::move(s));
    }
    return;
  }
  out->push_back(shape_of(e));
}

}  // namespace

std::uint32_t World::body2d_of(std::uint64_t entity) const {
  const auto it = bodies2d_.find(entity);
  return it == bodies2d_.end() ? 0 : it->second.id;
}

std::uint64_t World::entity_of_body2d(std::uint32_t body) const {
  return phys2d_ && body && phys2d_->exists(body) ? phys2d_->user(body) : 0;
}

std::uint32_t World::joint2d_of(std::uint64_t entity) const {
  const auto it = joints2d_.find(entity);
  return it == joints2d_.end() ? 0 : it->second;
}

std::size_t World::rigid_body_count() const {
  std::size_t n = 0;
  for (const auto& [id, b] : bodies2d_) n += b.proxy ? 0 : 1;
  return n;
}

bool World::phys2d_apply(std::uint64_t id, const char* what, float x, float y) {
  const std::uint32_t b = body2d_of(id);
  if (!b || bodies2d_.at(id).proxy || phys2d_->motion(b) != p2::Motion::Dynamic) return false;
  const std::string w = what ? what : "";
  if (w == "force") {
    phys2d_->add_force(b, {x, y});
  } else if (w == "impulse") {
    phys2d_->add_impulse(b, {x, y});
  } else if (w == "torque") {
    phys2d_->add_torque(b, x);
  } else {
    return false;
  }
  return true;
}

bool World::phys2d_set_velocity(std::uint64_t id, float vx, float vy) {
  const std::uint32_t b = body2d_of(id);
  if (!b || bodies2d_.at(id).proxy || phys2d_->motion(b) == p2::Motion::Static) return false;
  phys2d_->set_velocity(b, {vx, vy});
  return true;
}

bool World::phys2d_velocity(std::uint64_t id, float* vx, float* vy) const {
  const std::uint32_t b = body2d_of(id);
  if (!b) return false;
  const p2::Vec2 v = phys2d_->velocity(b);
  *vx = v.x;
  *vy = v.y;
  return true;
}

bool World::phys2d_raycast(float x, float y, float dx, float dy, float dist,
                           p2::RayHit* hit) const {
  return phys2d_ && phys2d_->raycast({x, y}, {dx, dy}, dist, hit);
}

std::uint32_t World::phys2d_joint(const Joint2DData& j, std::uint64_t a, std::uint64_t b,
                                  std::string* error) {
  const std::uint32_t ba = a ? body2d_of(a) : 0;
  const std::uint32_t bb = body2d_of(b);
  const Actor* eb = find(b);
  if (!phys2d_ || (a && !ba) || !bb || !eb) {
    if (error) *error = "both ends need a rigid body";
    return 0;
  }
  p2::JointDesc jd;
  jd.type = static_cast<p2::JointType>(j.type);
  jd.a = ba;
  jd.b = bb;
  const p2::Vec2 centre{eb->data.x + eb->data.w * 0.5f, eb->data.y + eb->data.h * 0.5f};
  jd.anchor = {centre.x + j.anchor_x, centre.y + j.anchor_y};
  if (j.type == Joint2DData::Type::Distance) {
    // From a's centre (or, pinned to the world, the anchor point) to b's.
    const Actor* t = a ? find(a) : nullptr;
    if (t) jd.anchor = {t->data.x + t->data.w * 0.5f, t->data.y + t->data.h * 0.5f};
    jd.anchor_b = centre;
  }
  jd.axis = {j.axis_x, j.axis_y};
  jd.limits = j.limits;
  jd.min = j.min;
  jd.max = j.max;
  return phys2d_->add_joint(jd, error);
}

void World::phys2d_unjoint(std::uint32_t joint) {
  if (phys2d_ && joint) phys2d_->remove_joint(joint);
}

float World::phys2d_joint_value(std::uint32_t joint) const {
  return phys2d_ && joint ? phys2d_->joint_value(joint) : 0.0f;
}

void World::setup_physics2d() {
  bool any = false;
  for (const Actor& a : actors_) any = any || (!a.dead && a.data.body);
  if (!any) {
    phys2d_.reset();
    bodies2d_.clear();
    joints2d_.clear();
    tiles2d_ = 0;
    contacts2d_.clear();
    return;
  }
  if (!phys2d_) {
    // The ts.physics.box2d plugin's world; none = no-op physics (bodies
    // stay where they were authored; static / kinematic ones still block
    // riders through the core collision).
    phys2d_ = p2::create_world(p2::Vec2{0.0f, 980.0f}, 32.0f);
    if (!phys2d_) {
      bodies2d_.clear();
      joints2d_.clear();
      tiles2d_ = 0;
      contacts2d_.clear();
      if (!phys2d_warned_) {
        phys2d_warned_ = true;
        post(LogLevel::Warn, "physics",
             "This scene has rigid bodies but no 2D physics plugin is on; they stay put. "
             "Switch on Box2D Physics (ts.physics.box2d) in the Trading Post.",
             0);
      }
      return;
    }
    bodies2d_.clear();
    joints2d_.clear();
    // Tiles never move: every solid run of every tilemap in one body.
    p2::BodyDesc td;
    td.motion = p2::Motion::Static;
    td.events = false;
    for (const SolidGrid& g : grids_) {
      const Entity2D& t = actors_[g.actor].data;
      const TileMapData& tm = *t.tilemap;
      const float ts = static_cast<float>(tm.tile_size);
      for (int r = 0; r < tm.rows; ++r) {
        int run = -1;
        for (int c = 0; c <= tm.cols; ++c) {
          const bool solid = c < tm.cols && g.table(tm.at(c, r));
          if (solid && run < 0) {
            run = c;
          } else if (!solid && run >= 0) {
            p2::ShapeDesc s;
            s.w = static_cast<float>(c - run) * ts;
            s.h = ts;
            s.offset = {t.x + static_cast<float>(run) * ts + s.w * 0.5f,
                        t.y + static_cast<float>(r) * ts + ts * 0.5f};
            td.shapes.push_back(s);
            run = -1;
          }
        }
      }
    }
    tiles2d_ = td.shapes.empty() ? 0 : phys2d_->create(td);
  }

  // What should have a body now.
  struct Want {
    std::size_t actor;
    bool proxy;
    p2::Motion motion;
  };
  std::map<std::uint64_t, Want> want;
  for (std::size_t i = 0; i < actors_.size(); ++i) {
    const Actor& a = actors_[i];
    if (a.dead) continue;
    const Entity2D& e = a.data;
    if (e.body) {
      want[e.id] = {i, false, motion_of(e.body->motion)};
    } else if (e.collider && !e.collider->trigger) {
      // Riders and pushables shove bodies; static solids stop them.
      const bool moves = e.player.has_value() || e.collider->dynamic;
      want[e.id] = {i, true, moves ? p2::Motion::Kinematic : p2::Motion::Static};
    }
  }
  for (auto it = bodies2d_.begin(); it != bodies2d_.end();) {
    const auto w = want.find(it->first);
    if (w == want.end() || w->second.proxy != it->second.proxy ||
        phys2d_->motion(it->second.id) != w->second.motion) {
      const auto j = joints2d_.find(it->first);
      if (j != joints2d_.end()) {
        phys2d_->remove_joint(j->second);
        joints2d_.erase(j);
      }
      phys2d_->destroy(it->second.id);
      it = bodies2d_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [id, w] : want) {
    if (bodies2d_.count(id)) continue;
    const Entity2D& e = actors_[w.actor].data;
    p2::BodyDesc d;
    d.user = id;
    d.motion = w.motion;
    append_shapes(e, &d.shapes);
    if (d.shapes.empty()) d.shapes.push_back(shape_of(e));
    d.position = {e.x + e.w * 0.5f, e.y + e.h * 0.5f};
    d.angle = rot_of(e);
    if (e.body) {
      const RigidBody2DData& b = *e.body;
      d.density = b.density;
      d.friction = b.friction;
      d.restitution = b.restitution;
      d.linear_damping = b.linear_damping;
      d.angular_damping = b.angular_damping;
      d.gravity_scale = b.gravity_scale;
      d.fixed_rotation = b.fixed_rotation;
      d.bullet = b.bullet;
      d.sensor = e.collider && e.collider->trigger;
    } else {
      d.fixed_rotation = true;
      d.events = false;
    }
    std::string err;
    const p2::BodyId bid = phys2d_->create(d, &err);
    if (!bid) {
      post(LogLevel::Warn, "physics", e.name + ": no rigid body (" + err + ")", id);
      continue;
    }
    if (e.body && e.body->mass > 0.0f && w.motion == p2::Motion::Dynamic) {
      const p2::ShapeDesc& s = d.shapes.front();
      const float m = e.body->mass;
      const float off = s.offset.x * s.offset.x + s.offset.y * s.offset.y;
      const float inertia = s.type == p2::ShapeType::Circle
                                ? m * (0.5f * s.radius * s.radius + off)
                                : m * ((s.w * s.w + s.h * s.h) / 12.0f + off);
      phys2d_->set_mass(bid, m, inertia);
    }
    bodies2d_[id] = Body2D{bid, w.proxy, d.position.x, d.position.y, d.angle};
  }

  // Joints: this entity's body to its target's (0 = the world).
  for (const Actor& a : actors_) {
    const Entity2D& e = a.data;
    if (a.dead || !e.joint || !e.body || joints2d_.count(e.id)) continue;
    const std::uint32_t mine = body2d_of(e.id);
    const std::uint32_t other = e.joint->target ? body2d_of(e.joint->target) : 0;
    if (!mine || (e.joint->target && !other)) continue;
    std::string err;
    const p2::JointId jid = phys2d_joint(*e.joint, e.joint->target, e.id, &err);
    if (jid) {
      joints2d_[e.id] = jid;
    } else {
      post(LogLevel::Warn, "physics", e.name + ": no joint (" + err + ")", e.id);
    }
  }
}

void World::step_physics2d() {
  contacts2d_.clear();
  if (!phys2d_) return;
  const float dt = static_cast<float>(kTickSeconds);
  // Entities -> bodies: proxies follow their actors; a body a script moved
  // (its pose is not the one written back last tick) jumps there.
  for (auto& [id, b] : bodies2d_) {
    const Actor* a = live(id);
    if (!a) continue;
    const Entity2D& e = a->data;
    const p2::Vec2 c{e.x + e.w * 0.5f, e.y + e.h * 0.5f};
    const float r = rot_of(e);
    const bool moved = c.x != b.x || c.y != b.y || r != b.rotation;
    const p2::Motion m = phys2d_->motion(b.id);
    if (m == p2::Motion::Kinematic) {
      phys2d_->move_kinematic(b.id, c, r, dt);
    } else if (moved) {
      phys2d_->set_transform(b.id, c, r);
    }
    b.x = c.x;
    b.y = c.y;
    b.rotation = r;
  }
  phys2d_->step(dt, 4);
  contacts2d_ = phys2d_->contacts();
  // Bodies -> entities (dynamic rigid bodies only).
  for (auto& [id, b] : bodies2d_) {
    if (b.proxy || phys2d_->motion(b.id) != p2::Motion::Dynamic) continue;
    Actor* a = live(id);
    if (!a) continue;
    const p2::Vec2 p = phys2d_->position(b.id);
    a->data.x = p.x - a->data.w * 0.5f;
    a->data.y = p.y - a->data.h * 0.5f;
    if (!a->data.body->fixed_rotation) a->data.rotation = phys2d_->angle(b.id);
    a->vx = (a->data.x - a->prev_x) / dt;
    a->vy = (a->data.y - a->prev_y) / dt;
    // What the next tick compares against (float-exact round trip).
    b.x = a->data.x + a->data.w * 0.5f;
    b.y = a->data.y + a->data.h * 0.5f;
    b.rotation = rot_of(a->data);
  }
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
