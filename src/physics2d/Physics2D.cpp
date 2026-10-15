#include "physics2d/Physics2D.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace ts {
namespace tombstone {
namespace physics2d {

namespace {

constexpr float kDeg = 3.14159265358979f / 180.0f;

std::uintptr_t as_key(BodyId id) { return static_cast<std::uintptr_t>(id); }

}  // namespace

struct World::Impl {
  b2WorldId world{};
  b2BodyId ground{};
  float ppm = 32.0f;
  struct Slot {
    b2BodyId id;
    std::uint64_t user = 0;
    Motion motion = Motion::Dynamic;
    bool sensor = false;
  };
  std::map<BodyId, Slot> bodies;
  BodyId next = 1;
  struct Joint {
    b2JointId id;
    JointType type;
  };
  std::map<JointId, Joint> joints;
  JointId next_joint = 1;
  std::vector<Contact> contacts;
  std::vector<SensorEvent> sensor_events;

  b2Vec2 m(Vec2 px) const { return {px.x / ppm, px.y / ppm}; }
  Vec2 px(b2Vec2 v) const { return {v.x * ppm, v.y * ppm}; }
  const Slot* slot(BodyId id) const {
    auto it = bodies.find(id);
    return it == bodies.end() ? nullptr : &it->second;
  }
  BodyId of_shape(b2ShapeId s) const {
    if (!b2Shape_IsValid(s)) return 0;
    const b2BodyId b = b2Shape_GetBody(s);
    return static_cast<BodyId>(reinterpret_cast<std::uintptr_t>(b2Body_GetUserData(b)));
  }
  std::uint64_t user_of_shape(b2ShapeId s) const {
    const Slot* sl = slot(of_shape(s));
    return sl ? sl->user : 0;
  }
};

World::World(Vec2 gravity, float ppm) : impl_(std::make_unique<Impl>()) {
  impl_->ppm = ppm > 0.0f ? ppm : 32.0f;
  b2WorldDef wd = b2DefaultWorldDef();
  wd.gravity = impl_->m(gravity);
  wd.workerCount = 1;  // single-threaded: same order every run
  impl_->world = b2CreateWorld(&wd);
  b2BodyDef gd = b2DefaultBodyDef();
  impl_->ground = b2CreateBody(impl_->world, &gd);
}

World::~World() {
  if (b2World_IsValid(impl_->world)) b2DestroyWorld(impl_->world);
}

float World::pixels_per_metre() const { return impl_->ppm; }
void World::set_gravity(Vec2 g) { b2World_SetGravity(impl_->world, impl_->m(g)); }

BodyId World::create(const BodyDesc& d, std::string* error) {
  if (d.shapes.empty()) {
    if (error) *error = "body has no shapes";
    return 0;
  }
  b2BodyDef bd = b2DefaultBodyDef();
  bd.type = d.motion == Motion::Static      ? b2_staticBody
            : d.motion == Motion::Kinematic ? b2_kinematicBody
                                            : b2_dynamicBody;
  bd.position = impl_->m(d.position);
  bd.rotation = b2MakeRot(d.angle * kDeg);
  bd.linearDamping = std::max(0.0f, d.linear_damping);
  bd.angularDamping = std::max(0.0f, d.angular_damping);
  bd.gravityScale = d.gravity_scale;
  bd.fixedRotation = d.fixed_rotation;
  bd.isBullet = d.bullet;
  const BodyId id = impl_->next++;
  bd.userData = reinterpret_cast<void*>(as_key(id));
  const b2BodyId body = b2CreateBody(impl_->world, &bd);
  b2ShapeDef sd = b2DefaultShapeDef();
  sd.density = std::max(0.0f, d.density);
  sd.material.friction = std::max(0.0f, d.friction);
  sd.material.restitution = std::clamp(d.restitution, 0.0f, 1.0f);
  sd.isSensor = d.sensor;
  sd.enableSensorEvents = d.events;
  sd.enableHitEvents = d.events && !d.sensor;
  sd.filter.categoryBits = d.category;
  sd.filter.maskBits = d.mask;
  int made = 0;
  for (const ShapeDesc& s : d.shapes) {
    const b2Vec2 c = impl_->m(s.offset);
    switch (s.type) {
      case ShapeType::Box: {
        const float hw = std::max(0.5f, std::fabs(s.w) * 0.5f) / impl_->ppm;
        const float hh = std::max(0.5f, std::fabs(s.h) * 0.5f) / impl_->ppm;
        const b2Polygon p = b2MakeOffsetBox(hw, hh, c, b2MakeRot(s.angle * kDeg));
        b2CreatePolygonShape(body, &sd, &p);
        ++made;
        break;
      }
      case ShapeType::Circle: {
        b2Circle circle{c, std::max(0.5f, s.radius) / impl_->ppm};
        b2CreateCircleShape(body, &sd, &circle);
        ++made;
        break;
      }
      case ShapeType::Capsule: {
        const float w = std::max(1.0f, std::fabs(s.w)), h = std::max(1.0f, std::fabs(s.h));
        const float r = 0.5f * std::min(w, h);
        const float half = 0.5f * std::max(w, h) - r;
        const b2Rot rot = b2MakeRot(s.angle * kDeg);
        b2Vec2 axis = w >= h ? b2Vec2{half / impl_->ppm, 0.0f} : b2Vec2{0.0f, half / impl_->ppm};
        axis = b2RotateVector(rot, axis);
        b2Capsule cap{b2Sub(c, axis), b2Add(c, axis), r / impl_->ppm};
        if (half <= 0.01f) {
          b2Circle circle{c, r / impl_->ppm};
          b2CreateCircleShape(body, &sd, &circle);
        } else {
          b2CreateCapsuleShape(body, &sd, &cap);
        }
        ++made;
        break;
      }
      case ShapeType::Polygon: {
        std::vector<b2Vec2> pts;
        for (const Vec2& p : s.points) {
          if (pts.size() >= B2_MAX_POLYGON_VERTICES) break;
          pts.push_back(b2Add(c, impl_->m(p)));
        }
        if (pts.size() < 3) break;
        const b2Hull hull = b2ComputeHull(pts.data(), static_cast<int>(pts.size()));
        if (hull.count < 3) break;
        const b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(body, &sd, &poly);
        ++made;
        break;
      }
    }
  }
  if (made == 0) {
    b2DestroyBody(body);
    if (error) *error = "no valid shape (degenerate polygon?)";
    return 0;
  }
  impl_->bodies[id] = {body, d.user, d.motion, d.sensor};
  return id;
}

void World::destroy(BodyId id) {
  auto it = impl_->bodies.find(id);
  if (it == impl_->bodies.end()) return;
  // Joints on it die with it in Box2D; drop our handles.
  for (auto j = impl_->joints.begin(); j != impl_->joints.end();) {
    if (!b2Joint_IsValid(j->second.id) ||
        B2_ID_EQUALS(b2Joint_GetBodyA(j->second.id), it->second.id) ||
        B2_ID_EQUALS(b2Joint_GetBodyB(j->second.id), it->second.id)) {
      j = impl_->joints.erase(j);
    } else {
      ++j;
    }
  }
  b2DestroyBody(it->second.id);
  impl_->bodies.erase(it);
}

bool World::exists(BodyId id) const { return impl_->slot(id) != nullptr; }
std::size_t World::body_count() const { return impl_->bodies.size(); }
std::uint64_t World::user(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s ? s->user : 0;
}
Motion World::motion(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s ? s->motion : Motion::Static;
}

Vec2 World::position(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s ? impl_->px(b2Body_GetPosition(s->id)) : Vec2{};
}
float World::angle(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s ? b2Rot_GetAngle(b2Body_GetRotation(s->id)) / kDeg : 0.0f;
}
void World::set_transform(BodyId id, Vec2 p, float angle) {
  const auto* s = impl_->slot(id);
  if (!s) return;
  b2Body_SetTransform(s->id, impl_->m(p), b2MakeRot(angle * kDeg));
  if (s->motion != Motion::Static) b2Body_SetAwake(s->id, true);
}
Vec2 World::velocity(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s ? impl_->px(b2Body_GetLinearVelocity(s->id)) : Vec2{};
}
float World::angular_velocity(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s ? b2Body_GetAngularVelocity(s->id) / kDeg : 0.0f;
}
void World::set_velocity(BodyId id, Vec2 v) {
  const auto* s = impl_->slot(id);
  if (!s || s->motion == Motion::Static) return;
  b2Body_SetLinearVelocity(s->id, impl_->m(v));
}
void World::set_angular_velocity(BodyId id, float w) {
  const auto* s = impl_->slot(id);
  if (!s || s->motion == Motion::Static) return;
  b2Body_SetAngularVelocity(s->id, w * kDeg);
}
void World::move_kinematic(BodyId id, Vec2 p, float a, float dt) {
  const auto* s = impl_->slot(id);
  if (!s || s->motion != Motion::Kinematic || !(dt > 0.0f)) return;
  const Vec2 now = position(id);
  b2Body_SetLinearVelocity(s->id, impl_->m({(p.x - now.x) / dt, (p.y - now.y) / dt}));
  float da = a - angle(id);
  while (da > 180.0f) da -= 360.0f;
  while (da < -180.0f) da += 360.0f;
  b2Body_SetAngularVelocity(s->id, da * kDeg / dt);
}
void World::add_force(BodyId id, Vec2 f) {
  const auto* s = impl_->slot(id);
  if (s && s->motion == Motion::Dynamic) b2Body_ApplyForceToCenter(s->id, impl_->m(f), true);
}
void World::add_impulse(BodyId id, Vec2 j) {
  const auto* s = impl_->slot(id);
  if (s && s->motion == Motion::Dynamic) b2Body_ApplyLinearImpulseToCenter(s->id, impl_->m(j), true);
}
void World::add_torque(BodyId id, float t) {
  const auto* s = impl_->slot(id);
  if (s && s->motion == Motion::Dynamic) {
    b2Body_ApplyTorque(s->id, t / (impl_->ppm * impl_->ppm), true);
  }
}
void World::set_mass(BodyId id, float mass, float inertia) {
  const auto* s = impl_->slot(id);
  if (!s || s->motion != Motion::Dynamic) return;
  b2MassData md;
  md.mass = std::max(1e-4f, mass);
  md.center = {0.0f, 0.0f};
  md.rotationalInertia = std::max(1e-6f, inertia / (impl_->ppm * impl_->ppm));
  b2Body_SetMassData(s->id, md);
}
float World::mass(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s ? b2Body_GetMass(s->id) : 0.0f;
}
bool World::awake(BodyId id) const {
  const auto* s = impl_->slot(id);
  return s && b2Body_IsAwake(s->id);
}

bool World::raycast(Vec2 from, Vec2 dir, float max_dist, RayHit* hit) const {
  const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
  if (l < 1e-6f || !(max_dist > 0.0f)) return false;
  const Vec2 t{dir.x / l * max_dist, dir.y / l * max_dist};
  const b2RayResult r = b2World_CastRayClosest(impl_->world, impl_->m(from), impl_->m(t),
                                                b2DefaultQueryFilter());
  if (!r.hit) return false;
  if (hit) {
    hit->body = impl_->of_shape(r.shapeId);
    hit->user = impl_->user_of_shape(r.shapeId);
    hit->point = impl_->px(r.point);
    hit->normal = {r.normal.x, r.normal.y};
    hit->distance = r.fraction * max_dist;
  }
  return true;
}

JointId World::add_joint(const JointDesc& d, std::string* error) {
  const auto* sa = d.a ? impl_->slot(d.a) : nullptr;
  const auto* sb = impl_->slot(d.b);
  if ((d.a && !sa) || !sb) {
    if (error) *error = "joint needs a body (and another body or the world)";
    return 0;
  }
  const b2BodyId a = sa ? sa->id : impl_->ground;
  const b2BodyId b = sb->id;
  const b2Vec2 anchor = impl_->m(d.anchor);
  const float ref = b2Rot_GetAngle(b2Body_GetRotation(b)) - b2Rot_GetAngle(b2Body_GetRotation(a));
  b2JointId j{};
  switch (d.type) {
    case JointType::Weld: {
      b2WeldJointDef jd = b2DefaultWeldJointDef();
      jd.bodyIdA = a;
      jd.bodyIdB = b;
      jd.localAnchorA = b2Body_GetLocalPoint(a, anchor);
      jd.localAnchorB = b2Body_GetLocalPoint(b, anchor);
      jd.referenceAngle = ref;
      j = b2CreateWeldJoint(impl_->world, &jd);
      break;
    }
    case JointType::Revolute: {
      b2RevoluteJointDef jd = b2DefaultRevoluteJointDef();
      jd.bodyIdA = a;
      jd.bodyIdB = b;
      jd.localAnchorA = b2Body_GetLocalPoint(a, anchor);
      jd.localAnchorB = b2Body_GetLocalPoint(b, anchor);
      jd.referenceAngle = ref;
      jd.enableLimit = d.limits;
      jd.lowerAngle = std::min(d.min, d.max) * kDeg;
      jd.upperAngle = std::max(d.min, d.max) * kDeg;
      j = b2CreateRevoluteJoint(impl_->world, &jd);
      break;
    }
    case JointType::Prismatic: {
      b2PrismaticJointDef jd = b2DefaultPrismaticJointDef();
      jd.bodyIdA = a;
      jd.bodyIdB = b;
      jd.localAnchorA = b2Body_GetLocalPoint(a, anchor);
      jd.localAnchorB = b2Body_GetLocalPoint(b, anchor);
      b2Vec2 ax{d.axis.x, d.axis.y};
      ax = b2Normalize(ax);
      jd.localAxisA = b2Body_GetLocalVector(a, ax);
      jd.referenceAngle = ref;
      jd.enableLimit = d.limits;
      jd.lowerTranslation = std::min(d.min, d.max) / impl_->ppm;
      jd.upperTranslation = std::max(d.min, d.max) / impl_->ppm;
      j = b2CreatePrismaticJoint(impl_->world, &jd);
      break;
    }
    case JointType::Distance: {
      b2DistanceJointDef jd = b2DefaultDistanceJointDef();
      jd.bodyIdA = a;
      jd.bodyIdB = b;
      const b2Vec2 pb = impl_->m(d.anchor_b);
      jd.localAnchorA = b2Body_GetLocalPoint(a, anchor);
      jd.localAnchorB = b2Body_GetLocalPoint(b, pb);
      jd.length = std::max(0.01f, b2Distance(anchor, pb));
      jd.enableLimit = d.limits;
      jd.minLength = std::max(0.0f, std::min(d.min, d.max)) / impl_->ppm;
      jd.maxLength = std::max(d.min, d.max) / impl_->ppm;
      if (d.limits) {
        // A rope: free between min and max.
        jd.enableSpring = true;
        jd.hertz = 0.0f;
      }
      j = b2CreateDistanceJoint(impl_->world, &jd);
      break;
    }
  }
  if (!b2Joint_IsValid(j)) {
    if (error) *error = "joint could not be made";
    return 0;
  }
  const JointId id = impl_->next_joint++;
  impl_->joints[id] = {j, d.type};
  return id;
}

void World::remove_joint(JointId id) {
  auto it = impl_->joints.find(id);
  if (it == impl_->joints.end()) return;
  if (b2Joint_IsValid(it->second.id)) b2DestroyJoint(it->second.id);
  impl_->joints.erase(it);
}

std::size_t World::joint_count() const { return impl_->joints.size(); }

float World::joint_value(JointId id) const {
  auto it = impl_->joints.find(id);
  if (it == impl_->joints.end() || !b2Joint_IsValid(it->second.id)) return 0.0f;
  switch (it->second.type) {
    case JointType::Revolute: return b2RevoluteJoint_GetAngle(it->second.id) / kDeg;
    case JointType::Prismatic: return b2PrismaticJoint_GetTranslation(it->second.id) * impl_->ppm;
    case JointType::Distance: return b2DistanceJoint_GetCurrentLength(it->second.id) * impl_->ppm;
    default: return 0.0f;
  }
}

void World::step(float dt, int sub_steps) {
  impl_->contacts.clear();
  impl_->sensor_events.clear();
  if (!(dt > 0.0f)) return;
  b2World_Step(impl_->world, dt, std::clamp(sub_steps, 1, 16));
  const b2ContactEvents ce = b2World_GetContactEvents(impl_->world);
  for (int i = 0; i < ce.hitCount; ++i) {
    const b2ContactHitEvent& h = ce.hitEvents[i];
    Contact c;
    c.a = impl_->user_of_shape(h.shapeIdA);
    c.b = impl_->user_of_shape(h.shapeIdB);
    c.point = impl_->px(h.point);
    c.normal = {h.normal.x, h.normal.y};
    c.speed = h.approachSpeed * impl_->ppm;
    impl_->contacts.push_back(c);
  }
  const b2SensorEvents se = b2World_GetSensorEvents(impl_->world);
  for (int i = 0; i < se.beginCount; ++i) {
    impl_->sensor_events.push_back({impl_->user_of_shape(se.beginEvents[i].sensorShapeId),
                                    impl_->user_of_shape(se.beginEvents[i].visitorShapeId), true});
  }
  for (int i = 0; i < se.endCount; ++i) {
    impl_->sensor_events.push_back({impl_->user_of_shape(se.endEvents[i].sensorShapeId),
                                    impl_->user_of_shape(se.endEvents[i].visitorShapeId), false});
  }
}

const std::vector<Contact>& World::contacts() const { return impl_->contacts; }
const std::vector<SensorEvent>& World::sensor_events() const { return impl_->sensor_events; }

std::uint64_t World::state_hash() const {
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&](float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int i = 0; i < 4; ++i) {
      h ^= (u >> (i * 8)) & 0xffu;
      h *= 1099511628211ull;
    }
  };
  for (const auto& [id, s] : impl_->bodies) {
    if (s.motion == Motion::Static) continue;
    const b2Vec2 p = b2Body_GetPosition(s.id), v = b2Body_GetLinearVelocity(s.id);
    const b2Rot q = b2Body_GetRotation(s.id);
    for (float f : {p.x, p.y, q.c, q.s, v.x, v.y, b2Body_GetAngularVelocity(s.id)}) mix(f);
  }
  return h;
}

std::vector<World::DebugShape> World::debug_shapes() const {
  std::vector<DebugShape> out;
  for (const auto& [id, s] : impl_->bodies) {
    const int n = b2Body_GetShapeCount(s.id);
    std::vector<b2ShapeId> shapes(static_cast<std::size_t>(std::max(0, n)));
    b2Body_GetShapes(s.id, shapes.data(), n);
    for (b2ShapeId sh : shapes) {
      DebugShape d;
      d.motion = s.motion;
      d.sensor = s.sensor;
      auto add = [&](b2Vec2 local) { d.points.push_back(impl_->px(b2Body_GetWorldPoint(s.id, local))); };
      switch (b2Shape_GetType(sh)) {
        case b2_polygonShape: {
          const b2Polygon p = b2Shape_GetPolygon(sh);
          for (int i = 0; i < p.count; ++i) add(p.vertices[i]);
          break;
        }
        case b2_circleShape: {
          const b2Circle c = b2Shape_GetCircle(sh);
          for (int i = 0; i < 16; ++i) {
            const float a = static_cast<float>(i) * (6.2831853f / 16.0f);
            add({c.center.x + std::cos(a) * c.radius, c.center.y + std::sin(a) * c.radius});
          }
          break;
        }
        case b2_capsuleShape: {
          const b2Capsule c = b2Shape_GetCapsule(sh);
          const b2Vec2 ax = b2Normalize(b2Sub(c.center2, c.center1));
          const float base = std::atan2(ax.y, ax.x);
          for (int i = 0; i <= 8; ++i) {
            const float a = base - 1.5707963f + static_cast<float>(i) * (3.14159265f / 8.0f);
            add({c.center2.x + std::cos(a) * c.radius, c.center2.y + std::sin(a) * c.radius});
          }
          for (int i = 0; i <= 8; ++i) {
            const float a = base + 1.5707963f + static_cast<float>(i) * (3.14159265f / 8.0f);
            add({c.center1.x + std::cos(a) * c.radius, c.center1.y + std::sin(a) * c.radius});
          }
          break;
        }
        default:
          break;
      }
      if (!d.points.empty()) out.push_back(std::move(d));
    }
  }
  return out;
}

}  // namespace physics2d
}  // namespace tombstone
}  // namespace ts
