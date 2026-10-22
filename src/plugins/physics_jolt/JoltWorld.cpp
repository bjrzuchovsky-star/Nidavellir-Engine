// Jolt Physics backend of the ts.physics.jolt Trading Post plugin (the
// code that used to be ts_physics3d's World). Jolt stays out of every
// public header; the engine sees only physics3d::World.
#include "plugins/physics_jolt/JoltWorld.h"

// Jolt wants its own header first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace ts {
namespace tombstone {
namespace physics3d {

namespace {

// ---- Jolt process-wide setup (once; never torn down) ----------------------

void jolt_init() {
  static std::once_flag once;
  std::call_once(once, [] {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  });
}

// Object layer = layer * 2 + moving; broad phase: 0 = non-moving, 1 = moving.
JPH::ObjectLayer obj_layer(std::uint8_t layer, bool moving) {
  return static_cast<JPH::ObjectLayer>((layer & 7) * 2 + (moving ? 1 : 0));
}
int layer_of(JPH::ObjectLayer l) { return (l >> 1) & 7; }
bool moving_of(JPH::ObjectLayer l) { return (l & 1) != 0; }

class BpLayers final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
    return JPH::BroadPhaseLayer(moving_of(l) ? 1 : 0);
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
    return static_cast<JPH::BroadPhaseLayer::Type>(l) ? "moving" : "static";
  }
#endif
};

class ObjVsBp final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer l, JPH::BroadPhaseLayer bp) const override {
    // Non-moving things only ever test against moving ones.
    return moving_of(l) || static_cast<JPH::BroadPhaseLayer::Type>(bp) == 1;
  }
};

class PairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  explicit PairFilter(const Settings* s) : s_(s) {}
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    if (!moving_of(a) && !moving_of(b)) return false;
    return s_->collides(layer_of(a), layer_of(b));
  }

 private:
  const Settings* s_;
};

// Query filter: our layer mask.
class MaskFilter final : public JPH::ObjectLayerFilter {
 public:
  explicit MaskFilter(std::uint8_t mask) : mask_(mask) {}
  bool ShouldCollide(JPH::ObjectLayer l) const override { return (mask_ >> layer_of(l)) & 1u; }

 private:
  std::uint8_t mask_;
};

class SolidFilter final : public JPH::BodyFilter {
 public:
  SolidFilter(JPH::BodyID ignore, bool sensors) : ignore_(ignore), sensors_(sensors) {}
  bool ShouldCollide(const JPH::BodyID& id) const override { return id != ignore_; }
  bool ShouldCollideLocked(const JPH::Body& b) const override { return sensors_ || !b.IsSensor(); }

 private:
  JPH::BodyID ignore_;
  bool sensors_;
};

JPH::Vec3 jv(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat jq(const Quat& q) {
  JPH::Quat r(q.x, q.y, q.z, q.w);
  const float n = r.Length();
  return n > 1e-6f ? r / n : JPH::Quat::sIdentity();
}
Vec3 tv(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
Quat tq(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

BodyId to_id(JPH::BodyID b) { return b.IsInvalid() ? 0 : b.GetIndexAndSequenceNumber() + 1; }
JPH::BodyID to_jph(BodyId id) { return id ? JPH::BodyID(id - 1) : JPH::BodyID(); }

std::uint64_t pair_key(BodyId a, BodyId b) {
  if (a > b) std::swap(a, b);
  return (static_cast<std::uint64_t>(a) << 32) | b;
}

float safe(float v, float lo) { return std::isfinite(v) ? std::max(v, lo) : lo; }

JPH::RefConst<JPH::Shape> make_shape(const ShapeDesc& d, bool dynamic, std::string* error) {
  JPH::RefConst<JPH::Shape> base;
  switch (d.type) {
    case ShapeType::Box: {
      const JPH::Vec3 half(safe(d.size.x, 0.002f) * 0.5f, safe(d.size.y, 0.002f) * 0.5f,
                           safe(d.size.z, 0.002f) * 0.5f);
      const float cr = std::min(JPH::cDefaultConvexRadius, 0.5f * half.ReduceMin());
      base = new JPH::BoxShape(half, cr);
      break;
    }
    case ShapeType::Sphere:
      base = new JPH::SphereShape(safe(d.size.x, 0.001f));
      break;
    case ShapeType::Capsule: {
      const float r = safe(d.size.x, 0.001f);
      const float half = std::max(0.0f, 0.5f * d.size.y - r);
      if (half <= 1e-4f) {
        base = new JPH::SphereShape(r);
      } else {
        base = new JPH::CapsuleShape(half, r);
      }
      break;
    }
    case ShapeType::ConvexHull: {
      JPH::Array<JPH::Vec3> pts;
      pts.reserve(d.points.size());
      for (const Vec3& p : d.points) pts.push_back(jv(p));
      if (pts.size() < 4) {
        if (error) *error = "convex hull needs at least 4 points";
        return nullptr;
      }
      JPH::ConvexHullShapeSettings s(pts, 0.01f);
      JPH::ShapeSettings::ShapeResult r = s.Create();
      if (!r.IsValid()) {
        if (error) *error = std::string("convex hull: ") + r.GetError().c_str();
        return nullptr;
      }
      base = r.Get();
      break;
    }
    case ShapeType::HeightField: {
      if (dynamic) {
        if (error) *error = "a height field collider can only be static";
        return nullptr;
      }
      const int n = d.samples;
      if (n < 2 || d.heights.size() != static_cast<std::size_t>(n) * n) {
        if (error) *error = "height field: bad sample grid";
        return nullptr;
      }
      // Jolt wants the sample count to be a multiple of its block size (2):
      // pad an odd grid with one no-collision row / column.
      const int padded = (n % 2 == 0) ? n : n + 1;
      std::vector<float> samples(static_cast<std::size_t>(padded) * padded, JPH::HeightFieldShapeConstants::cNoCollisionValue);
      for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
          samples[static_cast<std::size_t>(z) * padded + x] = d.heights[static_cast<std::size_t>(z) * n + x];
      const float cell = safe(d.size.x, 0.001f);
      const float half = cell * static_cast<float>(n - 1) * 0.5f;
      JPH::HeightFieldShapeSettings s(samples.data(), JPH::Vec3(-half, 0.0f, -half), JPH::Vec3(cell, 1.0f, cell),
                                      static_cast<JPH::uint32>(padded));
      s.mBitsPerSample = s.CalculateBitsPerSampleForError(0.01f);
      JPH::ShapeSettings::ShapeResult r = s.Create();
      if (!r.IsValid()) {
        if (error) *error = std::string("height field: ") + r.GetError().c_str();
        return nullptr;
      }
      base = r.Get();
      break;
    }
    case ShapeType::Mesh: {
      if (dynamic) {
        if (error) *error = "a triangle mesh collider can only be static (use a convex hull)";
        return nullptr;
      }
      JPH::VertexList verts;
      verts.reserve(d.points.size());
      for (const Vec3& p : d.points) verts.push_back(JPH::Float3(p.x, p.y, p.z));
      JPH::IndexedTriangleList tris;
      for (std::size_t i = 0; i + 2 < d.indices.size(); i += 3) {
        const std::uint32_t a = d.indices[i], b = d.indices[i + 1], c = d.indices[i + 2];
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;
        tris.push_back(JPH::IndexedTriangle(a, b, c));
      }
      if (tris.empty()) {
        if (error) *error = "mesh collider has no triangles";
        return nullptr;
      }
      JPH::MeshShapeSettings s(verts, tris);
      JPH::ShapeSettings::ShapeResult r = s.Create();
      if (!r.IsValid()) {
        if (error) *error = std::string("mesh: ") + r.GetError().c_str();
        return nullptr;
      }
      base = r.Get();
      break;
    }
  }
  return base;
}

}  // namespace

// ---- JoltWorld ----------------------------------------------------------------

struct JoltWorld::Impl final : public JPH::ContactListener {
  Settings settings;
  BpLayers bp;
  ObjVsBp obj_vs_bp;
  PairFilter pairs{&settings};
  std::unique_ptr<JPH::TempAllocatorImpl> temp;
  std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
  JPH::PhysicsSystem system;
  struct Info {
    std::uint8_t layer = kLayerDefault;
    Motion motion = Motion::Static;
    bool sensor = false;
    std::uint64_t user = 0;
  };
  std::map<BodyId, Info> infos;
  struct Joint {
    JointType type;
    JPH::Ref<JPH::TwoBodyConstraint> c;
    JPH::Vec3 l1 = JPH::Vec3::sZero(), l2 = JPH::Vec3::sZero();  // distance ends, COM space
  };
  std::map<JointId, Joint> joints;
  JointId next_joint = 1;
  JPH::Ref<JPH::CharacterVirtual> character;
  CharacterDesc char_desc;
  CharacterState char_state;
  std::uint64_t steps = 0;
  std::mutex mu;  // contact callbacks (single-threaded today; cheap insurance)
  std::vector<Contact> contacts;
  std::vector<SensorEvent> sensor_events;
  std::vector<SensorEvent> pending_events;  // from destroy() between steps
  std::map<std::uint64_t, int> sensor_pairs;  // pair -> sub-shape pair count
  std::map<std::uint64_t, std::pair<BodyId, BodyId>> sensor_who;  // pair -> (sensor, other)
  std::vector<std::uint64_t> dormant;  // pairs whose body sleeps inside the sensor

  // A dormant pair whose body woke up without touching the sensor again
  // has left it.
  void settle_dormant() {
    for (auto it = dormant.begin(); it != dormant.end();) {
      const std::uint64_t k = *it;
      auto who = sensor_who.find(k);
      if (who == sensor_who.end() || sensor_pairs.count(k)) {
        it = dormant.erase(it);
        continue;
      }
      const JPH::Body* other = body(who->second.second);
      if (other && !other->IsActive()) {
        ++it;
        continue;
      }
      SensorEvent ev;
      ev.sensor = who->second.first;
      ev.other = who->second.second;
      auto si = infos.find(ev.sensor), oi = infos.find(ev.other);
      ev.sensor_user = si != infos.end() ? si->second.user : 0;
      ev.other_user = oi != infos.end() ? oi->second.user : 0;
      ev.enter = false;
      sensor_events.push_back(ev);
      sensor_who.erase(who);
      it = dormant.erase(it);
    }
  }

  JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }
  const JPH::BodyInterface& bi() const { return system.GetBodyInterfaceNoLock(); }
  const JPH::Body* body(BodyId id) const {
    return system.GetBodyLockInterfaceNoLock().TryGetBody(to_jph(id));
  }
  JPH::Body* body_mut(BodyId id) {
    return system.GetBodyLockInterfaceNoLock().TryGetBody(to_jph(id));
  }

  void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                      JPH::ContactSettings&) override {
    const BodyId a = to_id(b1.GetID()), b = to_id(b2.GetID());
    std::lock_guard<std::mutex> lock(mu);
    if (b1.IsSensor() || b2.IsSensor()) {
      const std::uint64_t k = pair_key(a, b);
      if (sensor_pairs[k]++ == 0 && !sensor_who.count(k)) {
        const bool s1 = b1.IsSensor();
        SensorEvent ev;
        ev.sensor = s1 ? a : b;
        ev.other = s1 ? b : a;
        ev.sensor_user = s1 ? b1.GetUserData() : b2.GetUserData();
        ev.other_user = s1 ? b2.GetUserData() : b1.GetUserData();
        ev.enter = true;
        sensor_events.push_back(ev);
        sensor_who[k] = {ev.sensor, ev.other};
      }
      return;
    }
    if (m.mRelativeContactPointsOn1.empty()) return;
    const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
    const JPH::Vec3 n = m.mWorldSpaceNormal;
    const JPH::Vec3 rel = b2.GetPointVelocity(p) - b1.GetPointVelocity(p);
    const float speed = -rel.Dot(n);
    if (speed <= 0.05f) return;
    const float im1 = b1.IsDynamic() ? b1.GetMotionProperties()->GetInverseMass() : 0.0f;
    const float im2 = b2.IsDynamic() ? b2.GetMotionProperties()->GetInverseMass() : 0.0f;
    Contact c;
    c.a = a;
    c.b = b;
    c.user_a = b1.GetUserData();
    c.user_b = b2.GetUserData();
    c.point = tv(JPH::Vec3(p));
    c.normal = tv(n);
    c.speed = speed;
    c.impulse = (im1 + im2) > 0.0f ? speed / (im1 + im2) : 0.0f;
    if (contacts.size() < 4096) contacts.push_back(c);
  }

  void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
    const BodyId a = to_id(pair.GetBody1ID()), b = to_id(pair.GetBody2ID());
    std::lock_guard<std::mutex> lock(mu);
    const std::uint64_t k = pair_key(a, b);
    auto it = sensor_pairs.find(k);
    if (it == sensor_pairs.end()) return;
    if (--it->second > 0) return;
    sensor_pairs.erase(it);
    auto who = sensor_who.find(k);
    if (who == sensor_who.end()) return;
    // Jolt drops sensor contacts when the other body falls asleep; it is
    // still inside, so keep the pair (dormant) and report no exit.
    const JPH::Body* other = body(who->second.second);
    if (other && !other->IsActive() && !other->IsStatic()) {
      dormant.push_back(k);
      return;
    }
    SensorEvent ev;
    ev.sensor = who->second.first;
    ev.other = who->second.second;
    auto si = infos.find(ev.sensor), oi = infos.find(ev.other);
    ev.sensor_user = si != infos.end() ? si->second.user : 0;
    ev.other_user = oi != infos.end() ? oi->second.user : 0;
    ev.enter = false;
    sensor_events.push_back(ev);
    sensor_who.erase(who);
  }

  void forget_sensor_pairs(BodyId id) {
    for (auto it = sensor_who.begin(); it != sensor_who.end();) {
      if (it->second.first == id || it->second.second == id) {
        SensorEvent ev;
        ev.sensor = it->second.first;
        ev.other = it->second.second;
        auto si = infos.find(ev.sensor), oi = infos.find(ev.other);
        ev.sensor_user = si != infos.end() ? si->second.user : 0;
        ev.other_user = oi != infos.end() ? oi->second.user : 0;
        ev.enter = false;
        pending_events.push_back(ev);
        sensor_pairs.erase(it->first);
        it = sensor_who.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool cast(const JPH::Shape* shape, const Vec3& from, const Quat& rot, const Vec3& dir,
            float max_dist, RayHit* hit, std::uint8_t mask, BodyId ignore) const {
    const Vec3 d = render3d::normalize(dir);
    const JPH::RShapeCast sc = JPH::RShapeCast::sFromWorldTransform(
        shape, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(jq(rot), JPH::RVec3(from.x, from.y, from.z)),
        jv(d * max_dist));
    JPH::ShapeCastSettings ss;
    ss.mReturnDeepestPoint = false;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> col;
    MaskFilter mf(mask);
    SolidFilter bf(to_jph(ignore), false);
    system.GetNarrowPhaseQuery().CastShape(sc, ss, JPH::RVec3::sZero(), col, {}, mf, bf);
    if (!col.HadHit()) return false;
    if (hit) {
      hit->body = to_id(col.mHit.mBodyID2);
      const JPH::Body* b = body(hit->body);
      hit->user = b ? b->GetUserData() : 0;
      hit->distance = col.mHit.mFraction * max_dist;
      hit->point = tv(JPH::Vec3(col.mHit.mContactPointOn2));
      const JPH::Vec3 ax = col.mHit.mPenetrationAxis;
      hit->normal = ax.LengthSq() > 1e-12f ? tv(-ax.Normalized()) : Vec3{0, 1, 0};
    }
    return true;
  }

  std::vector<BodyId> overlap(const JPH::Shape* shape, const Vec3& c, const Quat& rot,
                              bool sensors) const {
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> col;
    JPH::CollideShapeSettings cs;
    SolidFilter bf(JPH::BodyID(), sensors);
    system.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(jq(rot), JPH::RVec3(c.x, c.y, c.z)), cs,
        JPH::RVec3::sZero(), col, {}, {}, bf);
    std::vector<BodyId> out;
    for (const auto& h : col.mHits) {
      const BodyId id = to_id(h.mBodyID2);
      if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
  }
};

JoltWorld::JoltWorld(const Settings& settings) : impl_(std::make_unique<Impl>()) {
  jolt_init();
  impl_->settings = settings;
  impl_->temp = std::make_unique<JPH::TempAllocatorImpl>(8 * 1024 * 1024);
  impl_->jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
  impl_->system.Init(8192, 0, 16384, 8192, impl_->bp, impl_->obj_vs_bp, impl_->pairs);
  impl_->system.SetGravity(jv(settings.gravity));
  impl_->system.SetContactListener(impl_.get());
}

JoltWorld::~JoltWorld() {
  destroy_character();
  for (auto& [id, j] : impl_->joints) impl_->system.RemoveConstraint(j.c);
  impl_->joints.clear();
  for (auto& [id, info] : impl_->infos) {
    impl_->bi().RemoveBody(to_jph(id));
    impl_->bi().DestroyBody(to_jph(id));
  }
  impl_->infos.clear();
  impl_->system.SetContactListener(nullptr);
}

const Settings& JoltWorld::settings() const { return impl_->settings; }

void JoltWorld::set_settings(const Settings& s) {
  impl_->settings = s;
  impl_->system.SetGravity(jv(s.gravity));
  for (auto& [id, info] : impl_->infos) {
    if (info.motion != Motion::Static) impl_->bi().ActivateBody(to_jph(id));
  }
}

BodyId JoltWorld::create(const BodyDesc& d, std::string* error) {
  const bool dynamic = d.motion == Motion::Dynamic;
  if (d.shapes.empty()) {
    if (error) *error = "body has no shapes";
    return 0;
  }
  JPH::RefConst<JPH::Shape> shape;
  if (d.shapes.size() == 1 && d.shapes[0].offset == Vec3{} && d.shapes[0].rotation == Quat{}) {
    shape = make_shape(d.shapes[0], dynamic && !d.sensor, error);
  } else if (d.shapes.size() == 1) {
    JPH::RefConst<JPH::Shape> s = make_shape(d.shapes[0], dynamic && !d.sensor, error);
    if (!s) return 0;
    shape = new JPH::RotatedTranslatedShape(jv(d.shapes[0].offset), jq(d.shapes[0].rotation), s);
  } else {
    JPH::StaticCompoundShapeSettings cs;
    for (const ShapeDesc& sd : d.shapes) {
      JPH::RefConst<JPH::Shape> s = make_shape(sd, dynamic && !d.sensor, error);
      if (!s) return 0;
      cs.AddShape(jv(sd.offset), jq(sd.rotation), s);
    }
    JPH::ShapeSettings::ShapeResult r = cs.Create();
    if (!r.IsValid()) {
      if (error) *error = std::string("compound: ") + r.GetError().c_str();
      return 0;
    }
    shape = r.Get();
  }
  if (!shape) return 0;
  const JPH::EMotionType mt = d.motion == Motion::Dynamic     ? JPH::EMotionType::Dynamic
                              : d.motion == Motion::Kinematic ? JPH::EMotionType::Kinematic
                                                              : JPH::EMotionType::Static;
  JPH::BodyCreationSettings bs(shape, JPH::RVec3(d.position.x, d.position.y, d.position.z),
                               jq(d.rotation), mt, obj_layer(d.layer, d.motion != Motion::Static));
  bs.mUserData = d.user;
  bs.mFriction = std::max(0.0f, d.friction);
  bs.mRestitution = std::clamp(d.restitution, 0.0f, 1.0f);
  bs.mLinearDamping = std::max(0.0f, d.linear_damping);
  bs.mAngularDamping = std::max(0.0f, d.angular_damping);
  bs.mGravityFactor = d.gravity_scale;
  bs.mIsSensor = d.sensor;
  bs.mAllowSleeping = d.allow_sleep;
  bs.mMotionQuality = d.ccd ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
  if (d.sensor && d.motion != Motion::Static) bs.mCollideKinematicVsNonDynamic = true;
  if (dynamic && d.mass > 0.0f) {
    bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bs.mMassPropertiesOverride.mMass = d.mass;
  }
  const JPH::BodyID jid = impl_->bi().CreateAndAddBody(
      bs, d.motion == Motion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
  if (jid.IsInvalid()) {
    if (error) *error = "too many bodies";
    return 0;
  }
  const BodyId id = to_id(jid);
  impl_->infos[id] = {static_cast<std::uint8_t>(d.layer & 7), d.motion, d.sensor, d.user};
  return id;
}

void JoltWorld::destroy(BodyId id) {
  auto it = impl_->infos.find(id);
  if (it == impl_->infos.end()) return;
  // Joints on it go too.
  for (auto j = impl_->joints.begin(); j != impl_->joints.end();) {
    const BodyId a = to_id(j->second.c->GetBody1()->GetID());
    const BodyId b = to_id(j->second.c->GetBody2()->GetID());
    if (a == id || b == id) {
      impl_->system.RemoveConstraint(j->second.c);
      j = impl_->joints.erase(j);
    } else {
      ++j;
    }
  }
  impl_->forget_sensor_pairs(id);
  impl_->bi().RemoveBody(to_jph(id));
  impl_->bi().DestroyBody(to_jph(id));
  impl_->infos.erase(it);
}

bool JoltWorld::exists(BodyId id) const { return impl_->infos.count(id) != 0; }
std::size_t JoltWorld::body_count() const { return impl_->infos.size(); }
std::uint64_t JoltWorld::user(BodyId id) const {
  auto it = impl_->infos.find(id);
  return it == impl_->infos.end() ? 0 : it->second.user;
}
BodyId JoltWorld::find_user(std::uint64_t user) const {
  for (const auto& [id, info] : impl_->infos) {
    if (info.user == user) return id;
  }
  return 0;
}
Motion JoltWorld::motion(BodyId id) const {
  auto it = impl_->infos.find(id);
  return it == impl_->infos.end() ? Motion::Static : it->second.motion;
}
bool JoltWorld::sensor(BodyId id) const {
  auto it = impl_->infos.find(id);
  return it != impl_->infos.end() && it->second.sensor;
}
std::uint8_t JoltWorld::layer(BodyId id) const {
  auto it = impl_->infos.find(id);
  return it == impl_->infos.end() ? 0 : it->second.layer;
}
void JoltWorld::set_layer(BodyId id, std::uint8_t layer) {
  auto it = impl_->infos.find(id);
  if (it == impl_->infos.end()) return;
  it->second.layer = layer & 7;
  impl_->bi().SetObjectLayer(to_jph(id), obj_layer(layer, it->second.motion != Motion::Static));
  if (it->second.motion != Motion::Static) impl_->bi().ActivateBody(to_jph(id));
}
float JoltWorld::mass(BodyId id) const {
  const JPH::Body* b = impl_->body(id);
  if (!b || !b->IsDynamic()) return 0.0f;
  const float im = b->GetMotionProperties()->GetInverseMass();
  return im > 0.0f ? 1.0f / im : 0.0f;
}

Vec3 JoltWorld::position(BodyId id) const {
  if (!exists(id)) return {};
  return tv(JPH::Vec3(impl_->bi().GetPosition(to_jph(id))));
}
Quat JoltWorld::rotation(BodyId id) const {
  if (!exists(id)) return {};
  return tq(impl_->bi().GetRotation(to_jph(id)));
}
void JoltWorld::set_transform(BodyId id, const Vec3& p, const Quat& q, bool wake) {
  if (!exists(id)) return;
  impl_->bi().SetPositionAndRotation(to_jph(id), JPH::RVec3(p.x, p.y, p.z), jq(q),
                                     wake && motion(id) != Motion::Static
                                         ? JPH::EActivation::Activate
                                         : JPH::EActivation::DontActivate);
}
void JoltWorld::move_kinematic(BodyId id, const Vec3& p, const Quat& q, float dt) {
  if (motion(id) != Motion::Kinematic) return;
  impl_->bi().MoveKinematic(to_jph(id), JPH::RVec3(p.x, p.y, p.z), jq(q), dt);
}
Vec3 JoltWorld::velocity(BodyId id) const {
  if (!exists(id)) return {};
  return tv(impl_->bi().GetLinearVelocity(to_jph(id)));
}
Vec3 JoltWorld::angular_velocity(BodyId id) const {
  if (!exists(id)) return {};
  return tv(impl_->bi().GetAngularVelocity(to_jph(id)));
}
void JoltWorld::set_velocity(BodyId id, const Vec3& v) {
  if (motion(id) == Motion::Static) return;
  impl_->bi().SetLinearVelocity(to_jph(id), jv(v));
  impl_->bi().ActivateBody(to_jph(id));
}
void JoltWorld::set_angular_velocity(BodyId id, const Vec3& w) {
  if (motion(id) == Motion::Static) return;
  impl_->bi().SetAngularVelocity(to_jph(id), jv(w));
  impl_->bi().ActivateBody(to_jph(id));
}
void JoltWorld::add_force(BodyId id, const Vec3& f) {
  if (motion(id) != Motion::Dynamic) return;
  impl_->bi().AddForce(to_jph(id), jv(f));
}
void JoltWorld::add_force_at(BodyId id, const Vec3& f, const Vec3& point) {
  if (motion(id) != Motion::Dynamic) return;
  impl_->bi().AddForce(to_jph(id), jv(f), JPH::RVec3(point.x, point.y, point.z));
}
void JoltWorld::add_torque(BodyId id, const Vec3& t) {
  if (motion(id) != Motion::Dynamic) return;
  impl_->bi().AddTorque(to_jph(id), jv(t));
}
void JoltWorld::add_impulse(BodyId id, const Vec3& j) {
  if (motion(id) != Motion::Dynamic) return;
  impl_->bi().AddImpulse(to_jph(id), jv(j));
}
void JoltWorld::add_impulse_at(BodyId id, const Vec3& j, const Vec3& point) {
  if (motion(id) != Motion::Dynamic) return;
  impl_->bi().AddImpulse(to_jph(id), jv(j), JPH::RVec3(point.x, point.y, point.z));
}
void JoltWorld::set_gravity_scale(BodyId id, float s) {
  if (motion(id) != Motion::Dynamic) return;
  impl_->bi().SetGravityFactor(to_jph(id), s);
}
bool JoltWorld::awake(BodyId id) const { return exists(id) && impl_->bi().IsActive(to_jph(id)); }
void JoltWorld::wake(BodyId id) {
  if (motion(id) != Motion::Static) impl_->bi().ActivateBody(to_jph(id));
}

bool JoltWorld::raycast(const Vec3& from, const Vec3& dir, float max_dist, RayHit* hit,
                    std::uint8_t mask, BodyId ignore) const {
  const Vec3 d = render3d::normalize(dir);
  if (!(max_dist > 0.0f) || render3d::length(d) < 0.5f) return false;
  const JPH::RRayCast ray(JPH::RVec3(from.x, from.y, from.z), jv(d * max_dist));
  JPH::RayCastResult r;
  MaskFilter mf(mask);
  SolidFilter bf(to_jph(ignore), false);
  if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, r, {}, mf, bf)) return false;
  if (hit) {
    hit->body = to_id(r.mBodyID);
    hit->distance = r.mFraction * max_dist;
    hit->point = from + d * hit->distance;
    const JPH::Body* b = impl_->body(hit->body);
    hit->user = b ? b->GetUserData() : 0;
    hit->normal = b ? tv(b->GetWorldSpaceSurfaceNormal(
                          r.mSubShapeID2, JPH::RVec3(hit->point.x, hit->point.y, hit->point.z)))
                    : Vec3{0, 1, 0};
  }
  return true;
}

bool JoltWorld::sphere_cast(const Vec3& from, float radius, const Vec3& dir, float max_dist,
                        RayHit* hit, std::uint8_t mask, BodyId ignore) const {
  JPH::RefConst<JPH::Shape> s = new JPH::SphereShape(std::max(radius, 0.001f));
  return impl_->cast(s.GetPtr(), from, Quat{}, dir, max_dist, hit, mask, ignore);
}

bool JoltWorld::box_cast(const Vec3& from, const Vec3& half, const Quat& rot, const Vec3& dir,
                     float max_dist, RayHit* hit, std::uint8_t mask, BodyId ignore) const {
  const JPH::Vec3 h(std::max(half.x, 0.002f), std::max(half.y, 0.002f), std::max(half.z, 0.002f));
  JPH::RefConst<JPH::Shape> s = new JPH::BoxShape(h, std::min(0.02f, 0.5f * h.ReduceMin()));
  return impl_->cast(s.GetPtr(), from, rot, dir, max_dist, hit, mask, ignore);
}

std::vector<BodyId> JoltWorld::overlap_sphere(const Vec3& c, float radius, bool sensors) const {
  JPH::RefConst<JPH::Shape> s = new JPH::SphereShape(std::max(radius, 0.001f));
  return impl_->overlap(s.GetPtr(), c, Quat{}, sensors);
}

std::vector<BodyId> JoltWorld::overlap_box(const Vec3& c, const Vec3& half, const Quat& rot,
                                       bool sensors) const {
  const JPH::Vec3 h(std::max(half.x, 0.002f), std::max(half.y, 0.002f), std::max(half.z, 0.002f));
  JPH::RefConst<JPH::Shape> s = new JPH::BoxShape(h, std::min(0.02f, 0.5f * h.ReduceMin()));
  return impl_->overlap(s.GetPtr(), c, rot, sensors);
}

JointId JoltWorld::add_joint(const JointDesc& d, std::string* error) {
  JPH::Body* a = d.a ? impl_->body_mut(d.a) : &JPH::Body::sFixedToWorld;
  JPH::Body* b = d.b ? impl_->body_mut(d.b) : &JPH::Body::sFixedToWorld;
  if (!a || !b || a == b) {
    if (error) *error = "joint needs two different bodies (or one and the world)";
    return 0;
  }
  if (!a->IsDynamic() && !b->IsDynamic()) {
    if (error) *error = "joint needs at least one dynamic body";
    return 0;
  }
  const JPH::RVec3 p(d.anchor.x, d.anchor.y, d.anchor.z);
  JPH::Vec3 axis = jv(d.axis);
  axis = axis.LengthSq() > 1e-8f ? axis.Normalized() : JPH::Vec3::sAxisY();
  const JPH::Vec3 normal = axis.GetNormalizedPerpendicular();
  JPH::Ref<JPH::TwoBodyConstraint> c;
  switch (d.type) {
    case JointType::Fixed: {
      JPH::FixedConstraintSettings s;
      s.mAutoDetectPoint = true;
      c = s.Create(*a, *b);
      break;
    }
    case JointType::Hinge: {
      JPH::HingeConstraintSettings s;
      s.mPoint1 = s.mPoint2 = p;
      s.mHingeAxis1 = s.mHingeAxis2 = axis;
      s.mNormalAxis1 = s.mNormalAxis2 = normal;
      if (d.limits) {
        s.mLimitsMin = std::clamp(std::min(d.min, d.max), -JPH::JPH_PI, 0.0f);
        s.mLimitsMax = std::clamp(std::max(d.min, d.max), 0.0f, JPH::JPH_PI);
      }
      c = s.Create(*a, *b);
      break;
    }
    case JointType::Slider: {
      JPH::SliderConstraintSettings s;
      s.mAutoDetectPoint = true;
      s.SetSliderAxis(axis);
      if (d.limits) {
        s.mLimitsMin = std::min(0.0f, std::min(d.min, d.max));
        s.mLimitsMax = std::max(0.0f, std::max(d.min, d.max));
      }
      c = s.Create(*a, *b);
      break;
    }
    case JointType::Distance: {
      JPH::DistanceConstraintSettings s;
      s.mPoint1 = p;
      s.mPoint2 = JPH::RVec3(d.anchor_b.x, d.anchor_b.y, d.anchor_b.z);
      if (d.limits) {
        s.mMinDistance = std::max(0.0f, std::min(d.min, d.max));
        s.mMaxDistance = std::max(d.min, d.max);
      }
      c = s.Create(*a, *b);
      break;
    }
    case JointType::Ball: {
      JPH::PointConstraintSettings s;
      s.mPoint1 = s.mPoint2 = p;
      c = s.Create(*a, *b);
      break;
    }
  }
  if (!c) {
    if (error) *error = "joint could not be made";
    return 0;
  }
  impl_->system.AddConstraint(c);
  if (a->IsDynamic()) impl_->bi().ActivateBody(a->GetID());
  if (b->IsDynamic()) impl_->bi().ActivateBody(b->GetID());
  const JointId id = impl_->next_joint++;
  Impl::Joint j{d.type, c};
  if (d.type == JointType::Distance) {
    j.l1 = JPH::Vec3(a->GetInverseCenterOfMassTransform() * p);
    j.l2 = JPH::Vec3(b->GetInverseCenterOfMassTransform() *
                     JPH::RVec3(d.anchor_b.x, d.anchor_b.y, d.anchor_b.z));
  }
  impl_->joints[id] = j;
  return id;
}

void JoltWorld::remove_joint(JointId id) {
  auto it = impl_->joints.find(id);
  if (it == impl_->joints.end()) return;
  impl_->system.RemoveConstraint(it->second.c);
  for (JPH::Body* b : {it->second.c->GetBody1(), it->second.c->GetBody2()}) {
    if (b->IsDynamic()) impl_->bi().ActivateBody(b->GetID());
  }
  impl_->joints.erase(it);
}

std::size_t JoltWorld::joint_count() const { return impl_->joints.size(); }

float JoltWorld::joint_value(JointId id) const {
  auto it = impl_->joints.find(id);
  if (it == impl_->joints.end()) return 0.0f;
  JPH::TwoBodyConstraint* c = it->second.c.GetPtr();
  switch (it->second.type) {
    case JointType::Hinge: return static_cast<JPH::HingeConstraint*>(c)->GetCurrentAngle();
    case JointType::Slider: return static_cast<JPH::SliderConstraint*>(c)->GetCurrentPosition();
    case JointType::Distance: {
      const JPH::RVec3 p1 = c->GetBody1()->GetCenterOfMassTransform() * JPH::RVec3(it->second.l1);
      const JPH::RVec3 p2 = c->GetBody2()->GetCenterOfMassTransform() * JPH::RVec3(it->second.l2);
      return JPH::Vec3(p2 - p1).Length();
    }
    default: return 0.0f;
  }
}

bool JoltWorld::create_character(const CharacterDesc& d, const Vec3& position) {
  destroy_character();
  const float r = std::max(0.05f, d.radius);
  const float half = std::max(0.0f, 0.5f * d.height - r);
  JPH::RefConst<JPH::Shape> base;
  if (half > 1e-4f) {
    base = new JPH::CapsuleShape(half, r);
  } else {
    base = new JPH::SphereShape(r);
  }
  JPH::RefConst<JPH::Shape> shape =
      d.offset == Vec3{} ? base : JPH::RefConst<JPH::Shape>(new JPH::RotatedTranslatedShape(jv(d.offset), JPH::Quat::sIdentity(), base));
  JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings();
  s->mShape = shape;
  s->mMaxSlopeAngle = JPH::DegreesToRadians(d.max_slope_deg);
  s->mMass = d.mass;
  s->mMaxStrength = d.push_strength;
  // Contacts below the lower cap's centre hold the rider up.
  s->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -(d.offset.y - half));
  impl_->char_desc = d;
  impl_->character = new JPH::CharacterVirtual(s, JPH::RVec3(position.x, position.y, position.z),
                                               JPH::Quat::sIdentity(), 0, &impl_->system);
  impl_->char_state = {};
  impl_->char_state.position = position;
  return true;
}

bool JoltWorld::has_character() const { return impl_->character != nullptr; }
void JoltWorld::destroy_character() { impl_->character = nullptr; }

CharacterState JoltWorld::move_character(const Vec3& v, float dt) {
  CharacterState& st = impl_->char_state;
  JPH::CharacterVirtual* ch = impl_->character.GetPtr();
  if (!ch) return st;
  ch->SetLinearVelocity(jv(v));
  JPH::CharacterVirtual::ExtendedUpdateSettings es;
  es.mWalkStairsStepUp = JPH::Vec3(0.0f, impl_->char_desc.step_up, 0.0f);
  // Only stick to the floor while walking, never mid-jump.
  if (v.y > 0.1f) es.mStickToFloorStepDown = JPH::Vec3::sZero();
  MaskFilter mf(static_cast<std::uint8_t>(impl_->settings.collide[kLayerPlayer]));
  SolidFilter bf(JPH::BodyID(), false);
  ch->ExtendedUpdate(dt, jv(impl_->settings.gravity), es, {}, mf, bf, {}, *impl_->temp);
  st.position = tv(JPH::Vec3(ch->GetPosition()));
  st.velocity = tv(ch->GetLinearVelocity());
  st.grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
  st.ground_normal = tv(ch->GetGroundNormal());
  st.contacts = static_cast<int>(ch->GetActiveContacts().size());
  return st;
}

void JoltWorld::set_character_position(const Vec3& p) {
  if (!impl_->character) return;
  impl_->character->SetPosition(JPH::RVec3(p.x, p.y, p.z));
  impl_->character->SetLinearVelocity(JPH::Vec3::sZero());
  impl_->char_state.position = p;
  impl_->char_state.velocity = {};
}

CharacterState JoltWorld::character() const { return impl_->char_state; }

void JoltWorld::step(float dt) {
  impl_->contacts.clear();
  impl_->sensor_events.swap(impl_->pending_events);
  impl_->pending_events.clear();
  if (!(dt > 0.0f)) return;
  impl_->system.Update(dt, std::clamp(impl_->settings.collision_steps, 1, 8), impl_->temp.get(),
                       impl_->jobs.get());
  impl_->settle_dormant();
  ++impl_->steps;
}

std::uint64_t JoltWorld::steps() const { return impl_->steps; }
const std::vector<Contact>& JoltWorld::contacts() const { return impl_->contacts; }
const std::vector<SensorEvent>& JoltWorld::sensor_events() const { return impl_->sensor_events; }

std::uint64_t JoltWorld::state_hash() const {
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&](float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int i = 0; i < 4; ++i) {
      h ^= (u >> (i * 8)) & 0xffu;
      h *= 1099511628211ull;
    }
  };
  for (const auto& [id, info] : impl_->infos) {  // std::map: id order
    if (info.motion == Motion::Static) continue;
    const Vec3 p = position(id), v = velocity(id), w = angular_velocity(id);
    const Quat q = rotation(id);
    for (float f : {p.x, p.y, p.z, q.x, q.y, q.z, q.w, v.x, v.y, v.z, w.x, w.y, w.z}) mix(f);
  }
  return h;
}

std::vector<World::DebugBox> JoltWorld::debug_boxes() const {
  std::vector<DebugBox> out;
  out.reserve(impl_->infos.size());
  for (const auto& [id, info] : impl_->infos) {
    const JPH::Body* b = impl_->body(id);
    if (!b) continue;
    const JPH::AABox& bb = b->GetWorldSpaceBounds();
    DebugBox d;
    d.min = tv(bb.mMin);
    d.max = tv(bb.mMax);
    d.motion = info.motion;
    d.sensor = info.sensor;
    d.sleeping = info.motion != Motion::Static && !b->IsActive();
    out.push_back(d);
  }
  return out;
}

std::vector<BodyId> JoltWorld::bodies() const {
  std::vector<BodyId> out;
  for (const auto& [id, info] : impl_->infos) out.push_back(id);
  return out;
}

}  // namespace physics3d
}  // namespace tombstone
}  // namespace ts

#include "core/Providers.h"

namespace ts {
namespace tombstone {
namespace physics3d {

void offer_jolt() {
  using Factory = WorldFactory;
  core::Providers<Factory>::instance().offer(kJoltPluginId, [] {
    return Factory([](const Settings& s) -> std::unique_ptr<World> {
      return std::make_unique<JoltWorld>(s);
    });
  });
}

}  // namespace physics3d
}  // namespace tombstone
}  // namespace ts
