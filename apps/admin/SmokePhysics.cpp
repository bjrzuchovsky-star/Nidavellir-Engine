#include "SmokePhysics.h"

#include "physics3d/Physics3D.h"
#include "render3d/Scene3D.h"
#include "world3d/World3D.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace p3 = ts::tombstone::physics3d;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;
namespace fs = std::filesystem;

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


// ---- World3D on Jolt ----------------------------------------------------------

r3::Quat look_along(r3::Vec3 dir) {
  dir = r3::normalize(dir);
  const float yaw = std::atan2(-dir.x, -dir.z);
  const float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, dir.y)));
  return r3::Quat::axis_angle({0, 1, 0}, yaw) * r3::Quat::axis_angle({1, 0, 0}, pitch);
}

// Drives VR input in world space (the desktop simulator's job in the editor).
struct Hands {
  w3::World3D& w;
  w3::VrInput in;
  explicit Hands(w3::World3D& world) : w(world) {
    in.head.position = {0.0f, 1.7f, 0.0f};
    in.head.valid = true;
    for (int h = 0; h < 2; ++h) {
      in.hands[h].grip.position = {h ? 0.2f : -0.2f, 1.1f, -0.3f};
      in.hands[h].grip.valid = in.hands[h].aim.valid = true;
      in.hands[h].aim = in.hands[h].grip;
    }
  }
  void place(int h, r3::Vec3 p, r3::Vec3 dir = {0, 0, -1}) {
    const r3::Mat4 inv = w.rig_matrix().inverse();
    const r3::Quat rig = r3::Quat::axis_angle({0, 1, 0}, r3::radians(w.rig_yaw()));
    w3::VrPose pose;
    pose.position = inv.transform_point(p);
    pose.orientation = rig.conjugate() * look_along(dir);
    pose.valid = true;
    in.hands[h].grip = in.hands[h].aim = pose;
  }
  void tick(int n = 1) {
    for (int i = 0; i < n; ++i) {
      w.set_vr_input(in);
      w.step({});
    }
  }
};

double num(const w3::World3D& w, std::uint64_t id, const char* key) {
  const auto* v = w.state(id, key);
  return v ? v->number : 0.0;
}

std::uint64_t named(const s3::Scene3D& s, const std::string& n) {
  const s3::Entity3D* e = s.find_named(n);
  return e ? e->id : 0;
}

int vr_knock(const fs::path& dir, std::string* report) {
  s3::Scene3D s;
  std::string err;
  if (!s3::load_file(s3::scene_path(dir.string()), &s, &err)) return fail("load saloon: " + err);
  w3::World3D w;
  std::vector<std::string> errors;
  w.set_log_sink([&](const ts::tombstone::runtime::LogEntry& e) {
    if (e.level == ts::tombstone::runtime::LogLevel::Error) errors.push_back(e.text);
  });
  w.set_vr(true);
  if (!w.start(s, dir.string(), &err)) return fail("start: " + err);
  Hands r(w);
  r.tick(30);  // settle: nothing on the shelves should move
  std::vector<std::uint64_t> cans;
  std::vector<r3::Vec3> can0;
  for (int i = 1; i <= 6; ++i) {
    cans.push_back(named(s, "Tin Can " + std::to_string(i)));
    if (!cans.back() || !w.body_of(cans.back())) return fail("tin can bodies");
    can0.push_back(w.find(cans.back())->transform.position);
  }
  for (std::size_t i = 0; i < cans.size(); ++i) {
    const s3::Entity3D* a = s.find(cans[i]);
    if (r3::length(can0[i] - a->transform.position) > 0.02f) {
      return fail("can pyramid stands still before it is hit (can " + std::to_string(i + 1) + ")");
    }
  }
  const std::uint64_t b4 = named(s, "Bottle 4"), b3 = named(s, "Bottle 3");
  // Grab bottle 4, bring it beside the pyramid.
  r.place(w3::kRightHand, w.find(b4)->transform.position);
  r.in.hands[1].squeeze = 1.0f;
  r.tick(2);
  if (w.hand(w3::kRightHand).held != b4) return fail("grab bottle 4");
  r3::Vec3 hand{-2.25f, 1.02f, 1.5f};
  r.place(w3::kRightHand, hand, {-1, 0, 0});
  r.tick(40);
  if (r3::length(w.find(b4)->transform.position - hand) > 0.25f) {
    return fail("held bottle follows the hand (at " + f3(w.find(b4)->transform.position) + ")");
  }
  w.take_sounds();
  // Swing it west through the cans at 3 m/s; the bottle stays in the hand.
  const auto hits0 = w.impact_stats().held_hits;
  for (int i = 0; i < 30; ++i) {
    hand.x -= 0.05f;
    r.place(w3::kRightHand, hand, {-1, 0, 0});
    r.tick(1);
  }
  r.tick(120);
  if (w.hand(w3::kRightHand).held != b4) return fail("bottle still in the hand after the swing");
  int moved = 0;
  for (std::size_t i = 0; i < cans.size(); ++i) {
    if (r3::length(w.find(cans[i])->transform.position - can0[i]) > 0.1f) ++moved;
  }
  const float top_y = w.find(cans[5])->transform.position.y;
  if (moved < 3 || top_y > can0[5].y - 0.1f) {
    return fail("held bottle knocks the cans over (" + std::to_string(moved) + " moved, top can y " +
                std::to_string(top_y) + ")");
  }
  if (w.impact_stats().held_hits <= hits0) return fail("impacts from the held bottle");
  bool clang = false;
  for (const auto& ev : w.take_sounds()) clang |= ev.clip == "builtin:impact" && ev.positional;
  if (!clang) return fail("impact sound");
  double knocks = 0;
  for (std::uint64_t c : cans) knocks += num(w, c, "knocks");
  if (knocks < 1 || num(w, b4, "hits") < 1) return fail("on_impact(other, speed) in Lua");
  bool buzz = false;
  for (const auto& h : w.take_haptics()) buzz |= h.hand == w3::kRightHand;
  if (!buzz) return fail("the holding hand buzzes on impact");
  // Two held things hit each other: bottle 3 in the left hand swung into
  // bottle 4 in the right.
  r.place(w3::kLeftHand, w.find(b3)->transform.position);
  r.in.hands[0].squeeze = 1.0f;
  r.tick(2);
  if (w.hand(w3::kLeftHand).held != b3) return fail("grab bottle 3 (left)");
  r3::Vec3 right{-1.0f, 1.3f, 2.0f}, left{-1.0f, 1.3f, 2.6f};
  r.place(w3::kRightHand, right);
  r.place(w3::kLeftHand, left);
  r.tick(40);
  bool clash = false;
  float closest = 10.0f;
  for (int i = 0; i < 30; ++i) {
    left.z -= 0.04f;  // through where the right-hand bottle is
    r.place(w3::kLeftHand, left);
    r.tick(1);
    const auto& st = w.impact_stats();
    clash |= (st.last_a == b3 && st.last_b == b4) || (st.last_a == b4 && st.last_b == b3);
    closest = std::min(closest, r3::length(w.find(b3)->transform.position - w.find(b4)->transform.position));
  }
  if (!clash || closest < 0.05f) {
    return fail("held bottles collide instead of passing through (closest " + std::to_string(closest) + ")");
  }
  if (!errors.empty()) return fail("script error: " + errors.front());
  char b[220];
  std::snprintf(b, sizeof(b),
                "held bottle knocked %d/6 cans over (top can %.2f -> %.2f m), %llu held hits, "
                "clang + dust + buzz + on_impact, two held bottles collide",
                moved, can0[5].y, top_y,
                static_cast<unsigned long long>(w.impact_stats().held_hits));
  *report = b;
  return 0;
}

int vr_throw(const fs::path& dir, std::string* report) {
  s3::Scene3D s;
  std::string err;
  if (!s3::load_file(s3::scene_path(dir.string()), &s, &err)) return fail(err);
  w3::World3D w;
  w.set_vr(true);
  if (!w.start(s, dir.string(), &err)) return fail(err);
  Hands r(w);
  r.tick(5);
  const std::uint64_t b1 = named(s, "Bottle 1");
  r3::Vec3 p = w.find(b1)->transform.position;
  r.place(w3::kRightHand, p);
  r.in.hands[1].squeeze = 1.0f;
  r.tick(2);
  if (w.hand(w3::kRightHand).held != b1) return fail("grab bottle 1");
  // Lift clear of the bar, then swing at a steady (0, 1.2, 4.8) m/s.
  p = p + r3::Vec3{0.0f, 0.3f, 0.3f};
  r.place(w3::kRightHand, p);
  r.tick(20);
  const r3::Vec3 v{0.0f, 1.2f, 4.8f};
  for (int i = 0; i < 10; ++i) {
    p = p + v * static_cast<float>(w3::World3D::kTickSeconds);
    r.place(w3::kRightHand, p);
    r.tick(1);
  }
  // Let go mid-swing (the hand keeps moving as it opens).
  p = p + v * static_cast<float>(w3::World3D::kTickSeconds);
  r.place(w3::kRightHand, p);
  r.in.hands[1].squeeze = 0.0f;
  r.tick(1);
  const r3::Vec3 got = w.phys_velocity(b1);
  const float err_frac = r3::length(got - v) / r3::length(v);
  if (w.hand(w3::kRightHand).held != 0 || err_frac > 0.15f || w.vr_bodies() != 1) {
    return fail("throw keeps the hand's velocity: hand " + f3(v) + " bottle " + f3(got));
  }
  r.tick(300);
  if (w.vr_bodies() != 0) return fail("thrown bottle comes to rest");
  char b[160];
  std::snprintf(b, sizeof(b), "throw: hand %.2f m/s -> bottle %.2f m/s (%.0f%% off), lands",
                r3::length(v), r3::length(got), err_frac * 100.0f);
  *report = b;
  return 0;
}

s3::Entity3D cube(std::uint64_t id, const std::string& name, r3::Vec3 pos, r3::Vec3 scale) {
  s3::Entity3D e;
  e.id = id;
  e.name = name;
  e.transform.position = pos;
  e.transform.scale = scale;
  e.mesh = s3::MeshRef{};
  e.material = s3::Material3D{};
  e.collider = s3::Collider3D{};
  return e;
}

int desktop_carry(std::string* report) {
  s3::Scene3D s;
  s.editor_camera.yaw = 0.0f;  // forward is -z
  s.entities.push_back(cube(1, "Ground", {0, -0.5f, 0}, {40, 1, 40}));
  s3::Entity3D rider;
  rider.id = 2;
  rider.name = "Rider";
  rider.transform.position = {0, 1, 0};
  rider.player = s3::Player3D{};
  s.entities.push_back(rider);
  s3::Entity3D crate = cube(3, "Crate", {0, 0.25f, -1.6f}, {0.5f, 0.5f, 0.5f});
  crate.grabbable = s3::Grabbable3D{};
  s.entities.push_back(crate);
  // Three crates stacked ahead (rigid bodies).
  for (int i = 0; i < 3; ++i) {
    s3::Entity3D t = cube(10 + i, "Stack " + std::to_string(i + 1), {0, 0.3f + 0.6f * i, -5.0f},
                          {0.6f, 0.6f, 0.6f});
    s3::RigidBody3D rb;
    rb.mass = 4.0f;
    t.rigid_body = rb;
    s.entities.push_back(t);
  }
  // A hinged gate (joint to the world, +-45 degrees).
  s3::Entity3D gate = cube(20, "Gate", {4.5f, 1.0f, -2.0f}, {1.0f, 1.6f, 0.08f});
  gate.rigid_body = s3::RigidBody3D{};
  s3::Joint3D j;
  j.type = s3::Joint3D::Type::Hinge;
  j.anchor = {-0.5f, 0, 0};
  j.limits = true;
  j.min = -45.0f;
  j.max = 45.0f;
  gate.joint = j;
  s.entities.push_back(gate);
  // The scene round-trips with its physics.
  s.physics.gravity = {0, -9.0f, 0};
  s.physics.collide[3] &= static_cast<std::uint8_t>(~(1u << 6));
  s.physics.collide[6] &= static_cast<std::uint8_t>(~(1u << 3));
  s.entities[2].collider->parts.push_back({s3::ColliderShape3D::Sphere, {0.1f, 0, 0}, {0, 0.3f, 0}, {}});
  s.entities[2].collider->parts.clear();
  s3::Scene3D back;
  std::string err;
  if (!s3::parse(s3::write(s), &back, &err) || !(back == s) || s3::write(back) != s3::write(s)) {
    return fail("rigid_body / joint / physics settings round trip " + err);
  }
  w3::World3D w;
  if (!w.start(s, "", &err)) return fail(err);
  if (w.physics_bodies() != 6 || w.physics_joints() != 1) {
    return fail("bodies (" + std::to_string(w.physics_bodies()) + ") / joints (" +
                std::to_string(w.physics_joints()) + ")");
  }
  w3::Input3D idle;
  w.run_ticks(30, idle);
  const float stack_top0 = w.find(12)->transform.position.y;
  // Pick the crate up.
  w3::Input3D grab;
  grab.grab = true;
  w.run_ticks(30, grab);
  if (w.carried() != 3 || w.find(3)->transform.position.y < 0.9f) {
    return fail("physics gun picks the crate up (carried " + std::to_string(w.carried()) + ", y " +
                std::to_string(w.find(3)->transform.position.y) + ")");
  }
  // Walk into the stack with it.
  w3::Input3D walk = grab;
  walk.move_forward = 1.0f;
  const auto hits0 = w.impact_stats().held_hits;
  w.run_ticks(80, walk);
  w.run_ticks(90, grab);
  const float stack_top = w.find(12)->transform.position.y;
  if (w.impact_stats().held_hits <= hits0 || stack_top > stack_top0 - 0.3f) {
    return fail("carried crate knocks the stack over (top " + std::to_string(stack_top0) + " -> " +
                std::to_string(stack_top) + ")");
  }
  // Throw.
  w3::Input3D toss = grab;
  toss.throw_held = true;
  w.step(toss);
  const float speed = r3::length(w.phys_velocity(3));
  if (w.carried() != 0 || speed < 8.0f) return fail("throw (" + std::to_string(speed) + " m/s)");
  // Shove the gate past its limit with impulses; it stops at 45 degrees.
  for (int i = 0; i < 40; ++i) {
    w.phys_apply(20, "impulse", {0, 0, -6.0f});
    w.step(idle);
  }
  const float yaw = std::fabs(w.find(20)->transform.rotation.to_euler_deg().y);
  if (yaw < 35.0f || yaw > 50.0f) return fail("gate hinge stops at 45 deg, at " + std::to_string(yaw));
  char b[200];
  std::snprintf(b, sizeof(b),
                "desktop physics gun: carried crate toppled the stack (top %.2f -> %.2f m), thrown %.1f m/s; "
                "scene hinge gate stops at %.0f deg",
                stack_top0, stack_top, speed, yaw);
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

// The 3D physics API through the plugin bridge (the Lua physics table that
// graphs, C++ and C# call): impulse, velocity, raycast, joint.
int script_api3d(const fs::path& dir, std::string* report) {
  s3::Scene3D s;
  std::string err;
  if (!s3::load_file(s3::scene_path(dir.string()), &s, &err)) return fail("load saloon: " + err);
  w3::World3D w;
  if (!w.start(s, dir.string(), &err)) return fail("start: " + err);
  for (int i = 0; i < 10; ++i) w.step(w3::Input3D{});
  const std::uint64_t can = named(s, "Tin Can 6");
  const r3::Vec3 p = w.find(can)->transform.position;
  using SV = ts::tombstone::ScriptValue;
  auto call = [&](const char* fn, std::vector<SV> args, SV* r) {
    return w.script_plugin_call("physics", fn, args, r, &err);
  };
  auto n = [](double v) { return SV::of_number(v); };
  SV r;
  if (!call("raycast_xyz", {n(p.x), n(p.y + 2), n(p.z), n(0), n(-1), n(0), n(5)}, &r) ||
      static_cast<std::uint64_t>(r.number) != can) {
    return fail("bridge raycast_xyz onto the top can: " + err + " got " + std::to_string(r.number));
  }
  if (!call("apply_impulse", {n(static_cast<double>(can)), n(0), n(1.5), n(0)}, &r) || !r.flag) {
    return fail("bridge apply_impulse: " + err);
  }
  w.step(w3::Input3D{});
  if (!call("velocity_axis", {n(static_cast<double>(can)), n(1)}, &r) || r.number < 1.0) {
    return fail("impulse should launch the can upward (vy " + std::to_string(r.number) + ")");
  }
  if (!call("joint_xyz", {n(0), n(static_cast<double>(named(s, "Tin Can 1"))), SV::of_text("ball"), n(0), n(0.1),
                          n(0)}, &r) || r.number <= 0) {
    return fail("bridge joint_xyz: " + err);
  }
  if (!call("unjoint", {r}, &r)) return fail("bridge unjoint: " + err);
  *report = "bridge raycast / impulse / velocity / joint";
  return 0;
}

int run_physics_world_smoke() {
  const fs::path dir = fs::path("./TombStoneProjects") / "_physics_saloon_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::string err;
  if (!s3::write_haunted_saloon_sample(dir.string(), "Haunted Saloon VR", &err)) {
    return fail("write sample: " + err);
  }
  std::string knock, toss, carry, api;
  if (vr_knock(dir, &knock) != 0 || vr_throw(dir, &toss) != 0 || desktop_carry(&carry) != 0 ||
      script_api3d(dir, &api) != 0) {
    return 1;
  }
  fs::remove_all(dir, ec);
  std::cout << "[smoke] physics world OK (VR saloon: " << knock << "; " << toss << "; " << carry << "; " << api << ")\n";
  return 0;
}

int run_physics2d_smoke();  // SmokePhysics2D.cpp
int run_physics_editor_smoke();  // SmokePhysicsEditor.cpp

int run_physics_smoke() {
  if (run_physics_core_smoke() != 0) return 1;
  if (run_physics_world_smoke() != 0) return 1;
  if (run_physics2d_smoke() != 0) return 1;
  if (run_physics_editor_smoke() != 0) return 1;
  return 0;
}
