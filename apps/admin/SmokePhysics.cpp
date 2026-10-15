#include "SmokePhysics.h"

#include "physics3d/Physics3D.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace p3 = ts::tombstone::physics3d;
namespace r3 = ts::tombstone::render3d;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] physics FAILED: " << what << '\n';
  return 1;
}

constexpr float kDt = 1.0f / 60.0f;

p3::BodyDesc box(p3::Motion m, r3::Vec3 pos, r3::Vec3 size, std::uint64_t user = 0) {
  p3::BodyDesc d;
  d.motion = m;
  d.user = user;
  d.position = pos;
  p3::ShapeDesc s;
  s.size = size;
  d.shapes.push_back(s);
  if (m == p3::Motion::Static) d.layer = p3::kLayerStatic;
  return d;
}

std::string f3(r3::Vec3 v) {
  char b[96];
  std::snprintf(b, sizeof(b), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
  return b;
}

// Ten crates stacked on a floor; return the state hash after 4 s.
int stacking(std::uint64_t* hash_out, std::string* why) {
  p3::World w;
  w.create(box(p3::Motion::Static, {0, -0.5f, 0}, {40, 1, 40}));
  std::vector<p3::BodyId> crates;
  for (int i = 0; i < 10; ++i) {
    crates.push_back(w.create(box(p3::Motion::Dynamic, {0, 0.25f + 0.5f * i + 0.01f * i, 0},
                                  {0.5f, 0.5f, 0.5f}, 100 + i)));
  }
  for (int i = 0; i < 240; ++i) w.step(kDt);
  for (int i = 0; i < 10; ++i) {
    const r3::Vec3 p = w.position(crates[i]);
    const float want = 0.25f + 0.5f * i;
    if (std::fabs(p.x) > 0.05f || std::fabs(p.z) > 0.05f || std::fabs(p.y - want) > 0.03f) {
      *why = "crate " + std::to_string(i) + " drifted to " + f3(p);
      return 1;
    }
  }
  int asleep = 0;
  for (p3::BodyId c : crates) asleep += w.awake(c) ? 0 : 1;
  if (asleep < 10) {
    *why = "stack did not settle (" + std::to_string(asleep) + "/10 asleep)";
    return 1;
  }
  *hash_out = w.state_hash();
  return 0;
}

// A small pile hit by a thrown ball, then a hash: run twice, compare.
std::uint64_t scatter_hash() {
  p3::World w;
  w.create(box(p3::Motion::Static, {0, -0.5f, 0}, {40, 1, 40}));
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) {
      w.create(box(p3::Motion::Dynamic, {x * 0.31f - 0.45f, 0.15f + y * 0.3f, 0}, {0.3f, 0.3f, 0.3f}));
    }
  }
  p3::BodyDesc ball;
  ball.motion = p3::Motion::Dynamic;
  ball.position = {0.1f, 0.6f, 4.0f};
  p3::ShapeDesc s;
  s.type = p3::ShapeType::Sphere;
  s.size = {0.2f, 0.2f, 0.2f};
  ball.shapes.push_back(s);
  ball.ccd = true;
  ball.mass = 6.0f;
  const p3::BodyId b = w.create(ball);
  w.set_velocity(b, {0, 1.0f, -14.0f});
  for (int i = 0; i < 180; ++i) w.step(kDt);
  return w.state_hash();
}

int joints(std::string* report) {
  p3::World w;
  w.create(box(p3::Motion::Static, {0, -0.5f, 0}, {40, 1, 40}));
  // Hinge door (+-0.5 rad) shoved hard.
  const p3::BodyId door = w.create(box(p3::Motion::Dynamic, {0.5f, 1.2f, 0}, {1.0f, 2.0f, 0.1f}));
  p3::JointDesc h;
  h.type = p3::JointType::Hinge;
  h.b = door;
  h.anchor = {0, 1.2f, 0};
  h.axis = {0, 1, 0};
  h.limits = true;
  h.min = -0.5f;
  h.max = 0.5f;
  std::string err;
  const p3::JointId hj = w.add_joint(h, &err);
  if (!hj) return fail("hinge: " + err);
  // Slider: block on x in [0, 1].
  const p3::BodyId sled = w.create(box(p3::Motion::Dynamic, {5.0f, 3.0f, 0}, {0.4f, 0.4f, 0.4f}));
  p3::JointDesc sl;
  sl.type = p3::JointType::Slider;
  sl.b = sled;
  sl.anchor = {5.0f, 3.0f, 0};
  sl.axis = {1, 0, 0};
  sl.limits = true;
  sl.min = 0.0f;
  sl.max = 1.0f;
  const p3::JointId sj = w.add_joint(sl, &err);
  if (!sj) return fail("slider: " + err);
  // Distance: a lantern on a 1.5 m chain (min 0.5), kicked sideways.
  const p3::BodyId lantern = w.create(box(p3::Motion::Dynamic, {-4.0f, 3.5f, 0}, {0.2f, 0.3f, 0.2f}));
  p3::JointDesc dj;
  dj.type = p3::JointType::Distance;
  dj.b = lantern;
  dj.anchor = {-4.0f, 5.0f, 0};
  dj.anchor_b = {-4.0f, 3.5f, 0};
  dj.limits = true;
  dj.min = 0.5f;
  dj.max = 1.5f;
  const p3::JointId dji = w.add_joint(dj, &err);
  if (!dji) return fail("distance: " + err);
  // Ball: a sign swinging from a point.
  const p3::BodyId sign = w.create(box(p3::Motion::Dynamic, {-8.0f, 4.0f, 0}, {0.8f, 0.5f, 0.05f}));
  p3::JointDesc bj;
  bj.type = p3::JointType::Ball;
  bj.b = sign;
  bj.anchor = {-8.0f, 4.5f, 0};
  if (!w.add_joint(bj, &err)) return fail("ball: " + err);
  // Fixed: a plank welded to a crate falls as one.
  const p3::BodyId crate = w.create(box(p3::Motion::Dynamic, {8.0f, 3.0f, 0}, {0.5f, 0.5f, 0.5f}));
  const p3::BodyId plank = w.create(box(p3::Motion::Dynamic, {8.75f, 3.0f, 0}, {1.0f, 0.1f, 0.3f}));
  p3::JointDesc fj;
  fj.type = p3::JointType::Fixed;
  fj.a = crate;
  fj.b = plank;
  if (!w.add_joint(fj, &err)) return fail("fixed: " + err);
  if (w.joint_count() != 5) return fail("five joints");
  float max_angle = 0.0f, max_slide = 0.0f, min_slide = 0.0f, max_len = 0.0f;
  for (int i = 0; i < 240; ++i) {
    if (i < 60) {
      w.add_torque(door, {0, (i < 30 ? 400.0f : -400.0f), 0});
      w.add_force(sled, {(i < 30 ? 1500.0f : -1500.0f), 0, 0});
      w.add_force(lantern, {60.0f, -80.0f, 0});
    }
    w.step(kDt);
    max_angle = std::max(max_angle, std::fabs(w.joint_value(hj)));
    max_slide = std::max(max_slide, w.joint_value(sj));
    min_slide = std::min(min_slide, w.joint_value(sj));
    max_len = std::max(max_len, w.joint_value(dji));
  }
  if (max_angle < 0.4f || max_angle > 0.56f) {
    return fail("hinge limit +-0.5 rad, reached " + std::to_string(max_angle));
  }
  if (max_slide < 0.9f || max_slide > 1.09f || min_slide < -0.09f) {
    return fail("slider limit [0, 1] m, reached [" + std::to_string(min_slide) + ", " +
                std::to_string(max_slide) + "]");
  }
  if (max_len > 1.56f) return fail("distance max 1.5 m, reached " + std::to_string(max_len));
  const r3::Vec3 sp = w.position(sign);
  if (sp.y > 4.5f || r3::length(sp - r3::Vec3{-8.0f, 4.5f, 0}) > 0.6f) {
    return fail("ball joint holds the sign at its pivot, sign at " + f3(sp));
  }
  const float gap = r3::length(w.position(plank) - w.position(crate));
  if (std::fabs(gap - 0.75f) > 0.03f || w.position(crate).y > 0.3f) {
    return fail("fixed joint keeps the plank on the crate (gap " + std::to_string(gap) + ")");
  }
  char b[200];
  std::snprintf(b, sizeof(b), "hinge |a|<=%.2f rad, slider [%.2f, %.2f] m, chain <= %.2f m, ball + fixed hold",
                max_angle, min_slide, max_slide, max_len);
  *report = b;
  return 0;
}

int queries_and_events(std::string* report) {
  p3::World w;
  const p3::BodyId floor = w.create(box(p3::Motion::Static, {0, -0.5f, 0}, {40, 1, 40}, 1));
  // Rotated static ramp (45 degrees about z) -> a ray down hits its slope.
  p3::BodyDesc ramp = box(p3::Motion::Static, {6, 1, 0}, {2, 0.2f, 2}, 2);
  ramp.rotation = r3::Quat::axis_angle({0, 0, 1}, r3::radians(45.0f));
  w.create(ramp);
  p3::RayHit hit;
  if (!w.raycast({0, 5, 0}, {0, -1, 0}, 10, &hit) || hit.body != floor ||
      std::fabs(hit.distance - 5.0f) > 0.01f || hit.normal.y < 0.99f) {
    return fail("raycast down onto the floor");
  }
  if (!w.raycast({6, 5, 0}, {0, -1, 0}, 10, &hit) || hit.user != 2 ||
      std::fabs(hit.normal.x + 0.7071f) > 0.02f) {
    return fail("raycast onto the rotated ramp (normal " + f3(hit.normal) + ")");
  }
  if (w.raycast({0, 5, 0}, {0, 1, 0}, 10, &hit)) return fail("ray up hits nothing");
  if (!w.sphere_cast({0, 5, 0}, 0.5f, {0, -1, 0}, 10, &hit) || std::fabs(hit.distance - 4.5f) > 0.02f) {
    return fail("sphere cast stops a radius above the floor (" + std::to_string(hit.distance) + ")");
  }
  if (!w.box_cast({0, 5, 0}, {0.5f, 0.5f, 0.5f}, {}, {0, -1, 0}, 10, &hit) ||
      std::fabs(hit.distance - 4.5f) > 0.03f) {
    return fail("box cast");
  }
  // Mask: a ray that only wants the Default layer skips the static floor.
  if (w.raycast({0, 5, 0}, {0, -1, 0}, 10, &hit, 1u << p3::kLayerDefault)) return fail("layer mask");
  // Sensor + a falling crate: enter, then exit when it is removed.
  p3::BodyDesc trig = box(p3::Motion::Static, {-5, 1, 0}, {2, 2, 2}, 50);
  trig.sensor = true;
  trig.layer = p3::kLayerTrigger;
  const p3::BodyId sensor = w.create(trig);
  const p3::BodyId crate = w.create(box(p3::Motion::Dynamic, {-5, 4, 0}, {0.5f, 0.5f, 0.5f}, 51));
  bool entered = false, exited = false;
  float impact = 0.0f;
  for (int i = 0; i < 120; ++i) {
    w.step(kDt);
    for (const p3::SensorEvent& e : w.sensor_events()) {
      if (std::getenv("TS_PHYS_DEBUG")) std::cerr << i << " sensor ev " << e.sensor << " " << e.other_user << " " << e.enter << "\n";
      if (e.sensor == sensor && e.other_user == 51) (e.enter ? entered : exited) = true;
    }
    for (const p3::Contact& c : w.contacts()) {
      if (c.user_a == 51 || c.user_b == 51) impact = std::max(impact, c.impulse);
    }
  }
  if (!entered || exited) return fail("sensor enter (crate resting inside)");
  if (w.overlap_box({-5, 1, 0}, {1, 1, 1}, {}, true).size() < 2) return fail("overlap finds sensor + crate");
  if (!(impact > 1.0f)) return fail("landing reports an impact impulse");
  w.destroy(crate);
  w.step(kDt);
  for (const p3::SensorEvent& e : w.sensor_events()) exited |= !e.enter && e.sensor == sensor;
  if (!exited) return fail("sensor exit when the body goes");
  // Compound (table: top + 4 legs), convex hull (wedge), static mesh (ramp).
  p3::BodyDesc table;
  table.motion = p3::Motion::Dynamic;
  table.position = {10, 2, 0};
  p3::ShapeDesc top;
  top.size = {1.2f, 0.08f, 0.8f};
  top.offset = {0, 0.5f, 0};
  table.shapes.push_back(top);
  for (int i = 0; i < 4; ++i) {
    p3::ShapeDesc leg;
    leg.size = {0.08f, 1.0f, 0.08f};
    leg.offset = {(i & 1) ? 0.5f : -0.5f, 0.0f, (i & 2) ? 0.3f : -0.3f};
    table.shapes.push_back(leg);
  }
  const p3::BodyId tb = w.create(table);
  p3::BodyDesc wedge;
  wedge.motion = p3::Motion::Dynamic;
  wedge.position = {14, 2, 0};
  p3::ShapeDesc hull;
  hull.type = p3::ShapeType::ConvexHull;
  hull.points = {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f},
                 {-0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f}};
  wedge.shapes.push_back(hull);
  const p3::BodyId wb = w.create(wedge);
  p3::BodyDesc mesh;
  mesh.motion = p3::Motion::Static;
  mesh.layer = p3::kLayerStatic;
  mesh.position = {20, 0, 0};
  p3::ShapeDesc ms;
  ms.type = p3::ShapeType::Mesh;
  ms.points = {{-2, 0, -2}, {2, 1, -2}, {2, 1, 2}, {-2, 0, 2}};
  ms.indices = {0, 2, 1, 0, 3, 2};
  mesh.shapes.push_back(ms);
  if (!w.create(mesh)) return fail("static mesh body");
  std::string err;
  mesh.motion = p3::Motion::Dynamic;
  if (w.create(mesh, &err) || err.find("static") == std::string::npos) {
    return fail("a dynamic mesh is refused with a reason");
  }
  if (!tb || !wb) return fail("compound / convex bodies");
  for (int i = 0; i < 180; ++i) w.step(kDt);
  // Table legs (0.5 below the centre) stand on the floor.
  if (std::fabs(w.position(tb).y - 0.5f) > 0.03f) return fail("compound table stands on its legs, y=" + std::to_string(w.position(tb).y));
  if (w.position(wb).y > 0.05f || w.position(wb).y < -0.05f) return fail("convex wedge lands flat");
  if (!w.raycast({20, 5, 0}, {0, -1, 0}, 10, &hit) || std::fabs(hit.point.y - 0.5f) > 0.02f) {
    return fail("ray onto the mesh ramp");
  }
  // Character: drops onto the floor, walks into a wall, pushes a crate.
  p3::CharacterDesc cd;
  w.create(box(p3::Motion::Static, {0, 1, -6}, {6, 2, 0.4f}));  // wall face at z = -5.8
  w.create_character(cd, {0, 3, 0});
  p3::CharacterState st;
  r3::Vec3 v;
  for (int i = 0; i < 90; ++i) {
    v.y = st.grounded ? 0.0f : v.y - 18.0f * kDt;
    st = w.move_character(v, kDt);
    w.step(kDt);
  }
  if (!st.grounded || std::fabs(st.position.y - 1.0f) > 0.05f) {
    return fail("character lands at y=1, at " + f3(st.position));
  }
  for (int i = 0; i < 120; ++i) {
    st = w.move_character({0, st.grounded ? 0.0f : -1.0f, -4.0f}, kDt);
    w.step(kDt);
  }
  if (st.position.z < -5.45f || st.position.z > -5.3f) return fail("character stops at the wall, " + f3(st.position));
  p3::BodyDesc light = box(p3::Motion::Dynamic, {3, 0.25f, -3}, {0.5f, 0.5f, 0.5f});
  light.mass = 12.0f;  // an empty crate
  const p3::BodyId pushed = w.create(light);
  w.set_character_position({3, 1.0f, 0});
  for (int i = 0; i < 180; ++i) {
    st = w.move_character({0, st.grounded ? 0.0f : -1.0f, -3.0f}, kDt);
    w.step(kDt);
  }
  if (w.position(pushed).z > -3.1f) return fail("character shoves the crate, crate at " + f3(w.position(pushed)) + " rider at " + f3(st.position) + (w.awake(pushed) ? " awake" : " asleep"));
  char b[200];
  std::snprintf(b, sizeof(b),
                "ray/sphere/box casts + layer mask, sensor enter/exit, landing impulse %.1f N s, "
                "compound/convex/mesh, character grounded + wall + shove",
                impact);
  *report = b;
  return 0;
}

}  // namespace

int run_physics_core_smoke() {
  std::uint64_t h1 = 0, h2 = 0;
  std::string why;
  if (stacking(&h1, &why) != 0) return fail("stacking: " + why);
  if (stacking(&h2, &why) != 0 || h1 != h2) return fail("stacking is not repeatable");
  const std::uint64_t s1 = scatter_hash(), s2 = scatter_hash();
  if (s1 != s2) return fail("determinism: same inputs gave different hashes");
  char hash[32];
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(s1));
  if (const char* want = std::getenv("TS_PHYSICS_HASH")) {
    if (std::string(want) != hash) return fail(std::string("determinism hash ") + hash + " != " + want);
  }
  std::string jr, qr;
  if (joints(&jr) != 0) return 1;
  if (queries_and_events(&qr) != 0) return 1;
  std::cout << "[smoke] physics 3D core OK (10-crate stack settles + sleeps, " << jr << "; " << qr
            << "; determinism hash " << hash << " repeatable)\n";
  return 0;
}

int run_physics_smoke() {
  if (run_physics_core_smoke() != 0) return 1;
  return 0;
}
