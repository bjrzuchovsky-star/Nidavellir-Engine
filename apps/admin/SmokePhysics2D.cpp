// --smoke-physics, 2D part: Box2D rigid bodies in the 2D World. Turned
// static colliders block riders by their real (rotated) outline, a stack
// of crates settles, a revolute joint stops at its limit, a rider pushes a
// crate body, a body sets off a trigger, the scene.json v10 fields round-
// trip, and two identical runs hash the same.

#include "runtime/Collision.h"
#include "runtime/World.h"
#include "scene/SceneData.h"
#include "scene/SceneJson.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace ts::tombstone;

int fail(const std::string& what) {
  std::cerr << "[smoke] physics2d FAILED: " << what << '\n';
  return 1;
}

std::string f2(float v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

runtime::InputFrame ride(float x, float y) {
  runtime::InputFrame f;
  f.slot(0).move_x = x;
  f.slot(0).move_y = y;
  f.slot(0).connected = true;
  return f;
}

Entity2D box(std::uint64_t id, const char* name, float x, float y, float w, float h) {
  Entity2D e;
  e.id = id;
  e.name = name;
  e.x = x;
  e.y = y;
  e.w = w;
  e.h = h;
  e.collider = default_collider(e);
  return e;
}

Entity2D rider(std::uint64_t id, float x, float y) {
  Entity2D e = box(id, "Rider", x, y, 32.0f, 32.0f);
  e.player = PlayerControllerData{0, 160.0f};
  return e;
}

Entity2D body(Entity2D e, RigidBody2DData::Motion m, float gravity = 1.0f) {
  RigidBody2DData b;
  b.motion = m;
  b.gravity_scale = gravity;
  e.body = b;
  return e;
}

// Ride right for 2 s at centre height cy; the rider's right edge after.
float ride_into(float rotation, float cy) {
  runtime::World w;
  Entity2D wall = box(1, "Wall", 200.0f, 200.0f, 64.0f, 64.0f);
  wall.rotation = rotation;
  w.build({wall, rider(2, 40.0f, cy - 16.0f)});
  for (int i = 0; i < 120; ++i) w.step(ride(1.0f, 0.0f));
  return runtime::collider_box(w.find(2)->data).x1;
}

int rotated_colliders(std::string* report) {
  const float flat = ride_into(0.0f, 232.0f);
  const float diamond = ride_into(45.0f, 232.0f);
  const float high = ride_into(45.0f, 192.0f);
  const float half_diag = 32.0f * std::sqrt(2.0f);
  if (std::fabs(flat - 200.0f) > 0.01f) return fail("unturned wall: right edge " + f2(flat));
  if (std::fabs(diamond - (232.0f - half_diag)) > 0.05f) {
    return fail("45 deg wall: right edge " + f2(diamond) + ", want " + f2(232.0f - half_diag));
  }
  // 24 px above the centre the diamond's left face is 24 px further right.
  if (std::fabs(high - (232.0f - half_diag + 24.0f)) > 0.05f) {
    return fail("45 deg wall, high: right edge " + f2(high));
  }
  // A turned trigger: inside its bounding box but outside the diamond.
  {
    runtime::World w;
    Entity2D t = box(1, "Gate", 200.0f, 200.0f, 64.0f, 64.0f);
    t.collider->trigger = true;
    t.rotation = 45.0f;
    w.build({t, rider(2, 174.0f, 174.0f)});  // in its bounds, outside the diamond
    w.step(ride(0.0f, 0.0f));
    if (w.inside_trigger(1, 2)) return fail("turned trigger fired in its empty corner");
    for (int i = 0; i < 20; ++i) w.step(ride(1.0f, 1.0f));
    if (!w.inside_trigger(1, 2)) return fail("turned trigger never fired");
  }
  *report += "rotated wall stops at " + f2(diamond) + " px (flat " + f2(flat) + ")";
  return 0;
}

std::vector<Entity2D> stack_scene() {
  std::vector<Entity2D> s;
  s.push_back(body(box(1, "Floor", 0.0f, 400.0f, 640.0f, 32.0f), RigidBody2DData::Motion::Static));
  for (int i = 0; i < 6; ++i) {
    Entity2D c = body(box(10 + i, "Crate", 300.0f + (i % 2) * 2.0f, 368.0f - 32.0f * i, 32.0f, 32.0f),
                      RigidBody2DData::Motion::Dynamic);
    s.push_back(c);
  }
  return s;
}

int stacking(std::uint64_t* hash, std::string* report) {
  runtime::World w;
  if (!w.build(stack_scene())) return fail("stack build");
  if (w.rigid_body_count() != 7) return fail("rigid bodies " + std::to_string(w.rigid_body_count()));
  for (int i = 0; i < 240; ++i) w.step(ride(0.0f, 0.0f));
  const Entity2D& top = w.find(15)->data;
  if (std::fabs(top.y - (368.0f - 160.0f)) > 1.0f || std::fabs(top.rotation) > 2.0f) {
    return fail("stack top at y " + f2(top.y) + " rot " + f2(top.rotation));
  }
  *hash = w.physics2d()->state_hash();
  *report += ", 6-stack settled (top y " + f2(top.y) + ")";
  return 0;
}

int joint_limit(std::string* report) {
  runtime::World w;
  Entity2D bar = body(box(1, "Arm", 100.0f, 100.0f, 64.0f, 8.0f), RigidBody2DData::Motion::Dynamic);
  bar.collider->trigger = false;
  Joint2DData j;
  j.type = Joint2DData::Type::Revolute;
  j.anchor_x = -32.0f;  // the left end, pinned to the world
  j.limits = true;
  j.min = -30.0f;
  j.max = 30.0f;
  bar.joint = j;
  w.build({bar});
  if (!w.joint2d_of(1)) return fail("no joint");
  for (int i = 0; i < 180; ++i) w.step(ride(0.0f, 0.0f));
  const Entity2D& e = w.find(1)->data;
  // Swings down (clockwise) and stops at +30 deg; the pinned end stays.
  const float rad = e.rotation * 3.14159265f / 180.0f;
  const float pin_x = e.x + 32.0f - 32.0f * std::cos(rad);
  const float pin_y = e.y + 4.0f - 32.0f * std::sin(rad);
  if (e.rotation < 26.0f || e.rotation > 32.0f) return fail("revolute limit: " + f2(e.rotation) + " deg");
  if (std::fabs(pin_x - 100.0f) > 0.5f || std::fabs(pin_y - 104.0f) > 0.5f) {
    return fail("pin drifted to " + f2(pin_x) + "," + f2(pin_y));
  }
  *report += ", revolute stops at " + f2(e.rotation) + " deg";
  return 0;
}

int push_and_trigger(std::string* report) {
  runtime::World w;
  Entity2D crate = body(box(3, "Crate", 140.0f, 100.0f, 32.0f, 32.0f),
                        RigidBody2DData::Motion::Dynamic, 0.0f);  // top-down
  crate.body->linear_damping = 2.0f;
  Entity2D gate = box(4, "Gate", 260.0f, 80.0f, 32.0f, 72.0f);
  gate.collider->trigger = true;
  w.build({rider(2, 60.0f, 100.0f), crate, gate});
  bool fired = false;
  for (int i = 0; i < 120 && !fired; ++i) {
    w.step(ride(1.0f, 0.0f));
    for (const auto& ev : w.trigger_events()) {
      fired = fired || (ev.trigger == 4 && ev.other == 3);
    }
  }
  const Entity2D& c = w.find(3)->data;
  if (c.x < 200.0f) return fail("rider pushed the crate only to x " + f2(c.x));
  if (!fired) return fail("pushed crate never set off the gate");
  *report += ", rider shoves crate to x " + f2(c.x) + " into a trigger";
  return 0;
}

int round_trip() {
  scene_json::SceneDoc doc;
  Entity2D a = body(box(5, "Ball", 0.0f, 0.0f, 20.0f, 20.0f), RigidBody2DData::Motion::Kinematic);
  a.collider->shape = ColliderData::Shape::Circle;
  a.body->restitution = 0.5f;
  a.body->bullet = true;
  Joint2DData j;
  j.type = Joint2DData::Type::Prismatic;
  j.target = 123456789012ull;
  j.axis_x = 0.0f;
  j.axis_y = 1.0f;
  j.limits = true;
  j.max = 40.0f;
  a.joint = j;
  doc.entities.push_back(a);
  scene_json::SceneDoc back;
  std::string err;
  if (!scene_json::parse(scene_json::write(doc), &back, &err)) return fail("parse: " + err);
  if (back.entities.size() != 1 || !(back.entities[0] == a)) return fail("v10 body / joint / shape round trip");
  return 0;
}

}  // namespace

int run_physics2d_smoke() {
  std::string report;
  if (rotated_colliders(&report) != 0) return 1;
  std::uint64_t h1 = 0, h2 = 0;
  if (stacking(&h1, &report) != 0) return 1;
  std::string again;
  if (stacking(&h2, &again) != 0) return 1;
  if (h1 != h2) return fail("two runs differ");
  if (joint_limit(&report) != 0) return 1;
  if (push_and_trigger(&report) != 0) return 1;
  if (round_trip() != 0) return 1;
  char hx[24];
  std::snprintf(hx, sizeof hx, "%016llx", static_cast<unsigned long long>(h1));
  std::cout << "[smoke] physics2d ok: " << report << ", hash " << hx << '\n';
  return 0;
}
