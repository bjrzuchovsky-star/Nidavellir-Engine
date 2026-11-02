#include "SmokeAnim3D.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "render3d/Animator.h"
#include "render3d/ClipFile.h"
#include "render3d/GlRenderer3D.h"
#include "render3d/Gltf.h"
#include "render3d/HiddenGl.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "render3d/Skeleton.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] anim3d FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near3(const r3::Vec3& a, const r3::Vec3& b, float eps = 1e-4f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
std::string str(const r3::Vec3& v) {
  char b[96];
  std::snprintf(b, sizeof(b), "(%.4f, %.4f, %.4f)", v.x, v.y, v.z);
  return b;
}

// Two joints: "root" at the origin, "arm" one metre up.
r3::Skeleton two_bones() {
  r3::Skeleton sk;
  r3::Joint a;
  a.name = "root";
  r3::Joint b;
  b.name = "arm";
  b.parent = 0;
  b.t = {0, 1, 0};
  sk.joints = {a, b};
  std::vector<r3::Mat4> g;
  r3::joint_matrices(sk, r3::rest_pose(sk), &g);
  for (std::size_t i = 0; i < g.size(); ++i) sk.joints[i].inverse_bind = g[i].inverse();
  return sk;
}

int skin_math() {
  const r3::Skeleton sk = two_bones();
  r3::Pose pose = r3::rest_pose(sk);
  std::vector<r3::Mat4> pal;
  r3::skin_palette(sk, pose, &pal);
  const float j1[4] = {1, 0, 0, 0}, w1[4] = {1, 0, 0, 0};
  if (!near3(r3::skin_point(pal, j1, w1, {0, 2, 0}), {0, 2, 0})) return fail("rest pose palette is not identity");
  // Bend the arm 90 degrees about +Z: (0, 2, 0) on the arm swings to (-1, 1, 0).
  pose[1].r = r3::Quat::axis_angle({0, 0, 1}, r3::radians(90.0f));
  r3::skin_palette(sk, pose, &pal);
  const r3::Vec3 tip = r3::skin_point(pal, j1, w1, {0, 2, 0});
  if (!near3(tip, {-1, 1, 0})) return fail("rigid arm vertex " + str(tip));
  // Half / half at the elbow height 1.5: root keeps (0,1.5,0), arm gives
  // (-0.5,1,0) -> the linear blend (-0.25, 1.25, 0).
  const float jh[4] = {0, 1, 0, 0}, wh[4] = {0.5f, 0.5f, 0, 0};
  const r3::Vec3 mid = r3::skin_point(pal, jh, wh, {0, 1.5f, 0});
  if (!near3(mid, {-0.25f, 1.25f, 0})) return fail("blended vertex " + str(mid));
  // Unnormalised weights are normalised; normals rotate with the bone.
  const float wu[4] = {2.0f, 2.0f, 0, 0};
  if (!near3(r3::skin_point(pal, jh, wu, {0, 1.5f, 0}), mid)) return fail("weight normalisation");
  if (!near3(r3::skin_dir(pal, j1, w1, {1, 0, 0}), {0, 1, 0})) return fail("skinned normal");
  // Palette = joint matrix * inverse bind, explicitly.
  std::vector<r3::Mat4> g;
  r3::joint_matrices(sk, pose, &g);
  const r3::Mat4 expect = g[1] * sk.joints[1].inverse_bind;
  for (int i = 0; i < 16; ++i) {
    if (!near(expect.m[i], pal[1].m[i])) return fail("palette != joint * inverse bind");
  }
  // Masks: "arm" covers itself only; "" covers all.
  const std::vector<float> m = sk.mask_for({"arm"});
  if (m[0] != 0.0f || m[1] != 1.0f || sk.mask_for({})[0] != 1.0f) return fail("mask_for");
  return 0;
}

int sampling() {
  r3::Channel lin;
  lin.path = r3::ChannelPath::Translation;
  lin.times = {0, 1};
  lin.values = {0, 0, 0, 2, 4, 6};
  float v[4];
  r3::sample_channel(lin, 0.25f, v);
  if (!near(v[0], 0.5f) || !near(v[1], 1.0f) || !near(v[2], 1.5f)) return fail("linear sample");
  r3::sample_channel(lin, 5.0f, v);
  if (!near(v[0], 2.0f)) return fail("clamp past the end");
  r3::Channel step = lin;
  step.interp = r3::Interp::Step;
  r3::sample_channel(step, 0.99f, v);
  if (!near(v[0], 0.0f)) return fail("step holds the key");
  // Cubic spline: zero tangents -> smoothstep; s=0.5 is the midpoint,
  // s=0.25 is 0.15625 of the way (2s^3 - 3s^2 + 1 weights).
  r3::Channel cub;
  cub.path = r3::ChannelPath::Translation;
  cub.interp = r3::Interp::CubicSpline;
  cub.times = {0, 2};
  cub.values = {0, 0, 0, 0, 0, 0, 0, 0, 0, /* key 2 */ 0, 0, 0, 4, 0, 0, 0, 0, 0};
  r3::sample_channel(cub, 1.0f, v);
  if (!near(v[0], 2.0f)) return fail("cubic midpoint");
  r3::sample_channel(cub, 0.5f, v);
  if (!near(v[0], 4.0f * 0.15625f)) return fail("cubic smoothstep " + std::to_string(v[0]));
  // Out-tangent 1 (units/s) at key 0 over a 2 s span adds td*h10 at s=0.5.
  cub.values[6] = 1.0f;
  r3::sample_channel(cub, 1.0f, v);
  if (!near(v[0], 2.0f + 2.0f * 0.125f)) return fail("cubic tangent " + std::to_string(v[0]));
  // Rotation: slerp 0 -> 90 about Y at 0.5 = 45 degrees.
  r3::Channel rot;
  rot.path = r3::ChannelPath::Rotation;
  rot.times = {0, 1};
  const r3::Quat q90 = r3::Quat::axis_angle({0, 1, 0}, r3::radians(90.0f));
  rot.values = {0, 0, 0, 1, q90.x, q90.y, q90.z, q90.w};
  r3::sample_channel(rot, 0.5f, v);
  const r3::Quat q45 = r3::Quat::axis_angle({0, 1, 0}, r3::radians(45.0f));
  if (!near(v[1], q45.y) || !near(v[3], q45.w)) return fail("rotation slerp");
  // Short way round: q and -q are the same rotation.
  const r3::Quat s = r3::slerp(r3::Quat{}, r3::Quat{-q90.x, -q90.y, -q90.z, -q90.w}, 0.5f);
  if (!near(std::fabs(s.y), q45.y)) return fail("slerp takes the short way");
  // Clip loop vs clamp.
  r3::Clip clip;
  clip.duration = 1.0f;
  lin.joint = 0;
  clip.channels = {lin};
  r3::Pose pose(1);
  r3::sample_clip(clip, 1.25f, true, &pose);
  if (!near(pose[0].t.x, 0.5f)) return fail("looped clip time");
  r3::sample_clip(clip, 1.25f, false, &pose);
  if (!near(pose[0].t.x, 2.0f)) return fail("clamped clip time");
  return 0;
}

int blend_weights() {
  const std::vector<float> at{0.0f, 1.8f, 4.5f};
  auto check = [&](float v, float a, float b, float c) {
    const auto w = r3::blend_weights_1d(at, v);
    return near(w[0], a) && near(w[1], b) && near(w[2], c) && near(w[0] + w[1] + w[2], 1.0f);
  };
  if (!check(0.0f, 1, 0, 0) || !check(-3.0f, 1, 0, 0) || !check(0.9f, 0.5f, 0.5f, 0) ||
      !check(3.15f, 0, 0.5f, 0.5f) || !check(9.0f, 0, 0, 1) || !check(1.8f, 0, 1, 0)) {
    return fail("1D blend weights");
  }
  // Unsorted points work the same.
  const auto w = r3::blend_weights_1d({4.5f, 0.0f, 1.8f}, 0.9f);
  if (!near(w[1], 0.5f) || !near(w[2], 0.5f) || w[0] != 0.0f) return fail("unsorted blend points");
  if (!r3::blend_weights_1d({}, 1.0f).empty()) return fail("empty blend space");
  // Pose blend: half way between rest and a 90 degree bend = 45 degrees;
  // a mask of 0 keeps the first pose.
  const r3::Skeleton sk = two_bones();
  r3::Pose a = r3::rest_pose(sk), b = a, out;
  b[1].r = r3::Quat::axis_angle({0, 0, 1}, r3::radians(90.0f));
  b[0].t = {2, 0, 0};
  r3::blend_poses(a, b, 0.5f, &out);
  const r3::Quat q45 = r3::Quat::axis_angle({0, 0, 1}, r3::radians(45.0f));
  if (!near(out[1].r.z, q45.z) || !near(out[0].t.x, 1.0f)) return fail("pose blend");
  const std::vector<float> mask = sk.mask_for({"arm"});
  r3::blend_poses(a, b, 1.0f, &out, &mask);
  if (!near(out[0].t.x, 0.0f) || !near(out[1].r.z, b[1].r.z)) return fail("masked pose blend");
  return 0;
}

int assets(const fs::path& dir, r3::Model* drifter) {
  std::string err;
  const std::string path = (dir / "drifter.gltf").string();
  if (!r3::write_sample_character_gltf(path, &err)) return fail(err);
  if (!r3::load_gltf(path, drifter, &err) || !err.empty()) return fail("drifter load: " + err);
  if (!drifter->skinned() || drifter->skeleton.joints.size() != 15) return fail("drifter skeleton");
  for (const char* c : {"idle", "walk", "run", "jump", "wave"}) {
    if (drifter->find_clip(c) < 0) return fail(std::string("drifter clip ") + c);
  }
  bool cubic = false, step = false;
  for (const r3::Channel& ch : drifter->clips[static_cast<std::size_t>(drifter->find_clip("run"))].channels) {
    cubic = cubic || ch.interp == r3::Interp::CubicSpline;
  }
  for (const r3::Channel& ch : drifter->clips[static_cast<std::size_t>(drifter->find_clip("jump"))].channels) {
    step = step || ch.interp == r3::Interp::Step;
  }
  if (!cubic || !step) return fail("cubic spline / step channels survive the glTF round trip");
  std::size_t skinned = 0, verts = 0;
  for (const r3::ModelPrimitive& p : drifter->primitives) {
    skinned += p.mesh.skinned() ? 1 : 0;
    verts += p.mesh.vertices.size();
  }
  if (skinned != drifter->primitives.size() || skinned != 5) return fail("drifter primitives skinned");
  // Rest = bind: the palette is identity, bounds are a ~2 m figure.
  std::vector<r3::Mat4> pal;
  r3::skin_palette(drifter->skeleton, r3::rest_pose(drifter->skeleton), &pal);
  for (const r3::Mat4& m : pal) {
    for (int i = 0; i < 16; ++i) {
      if (!near(m.m[i], r3::Mat4{}.m[i], 1e-4f)) return fail("drifter bind pose palette");
    }
  }
  if (!near(drifter->bounds.max.y, 2.04f, 0.05f) || !near(drifter->bounds.min.y, 0.0f, 0.01f)) {
    return fail("drifter bounds " + str(drifter->bounds.min) + " - " + str(drifter->bounds.max));
  }
  // A walking pose moves the feet (skinned on the CPU).
  r3::Pose walk = r3::rest_pose(drifter->skeleton);
  r3::sample_clip(drifter->clips[static_cast<std::size_t>(drifter->find_clip("walk"))], 0.25f, true, &walk);
  r3::skin_palette(drifter->skeleton, walk, &pal);
  const int foot = drifter->skeleton.find("lower_leg.L");
  const float jf[4] = {static_cast<float>(foot), 0, 0, 0}, wf[4] = {1, 0, 0, 0};
  const r3::Vec3 toe = r3::skin_point(pal, jf, wf, {-0.1f, 0.06f, -0.15f});
  if (near3(toe, {-0.1f, 0.06f, -0.15f}, 0.02f)) return fail("walk pose did not move the foot");
  // Hand.
  r3::Model hand;
  if (!r3::write_sample_hand_gltf((dir / "hand.gltf").string(), &err) ||
      !r3::load_gltf((dir / "hand.gltf").string(), &hand, &err) || !hand.skinned() ||
      hand.find_clip("grip") < 0 || hand.skeleton.joints.size() != 11) {
    return fail("hand glTF: " + err);
  }
  const auto size = fs::file_size(dir / "drifter.gltf");
  std::cout << "[smoke] anim3d assets: drifter.gltf " << size / 1024 << " KB, " << verts << " verts, "
            << drifter->skeleton.joints.size() << " joints, " << drifter->clips.size() << " clips; hand "
            << hand.skeleton.joints.size() << " joints\n";
  if (size > 400 * 1024) return fail("drifter.gltf too big");
  return 0;
}

int animator(const r3::Model& m) {
  r3::AnimatorRuntime rt;
  std::string problems;
  if (!rt.bind(s3::default_locomotion_animator(), &m, &problems) || !problems.empty()) {
    return fail("bind default locomotion: " + problems);
  }
  if (rt.state(0) != "locomotion" || rt.layer_count() != 2) return fail("entry state");
  auto weight = [&](const char* clip) {
    for (const auto& [n, w] : rt.state_weights(0)) {
      if (n == clip) return w;
    }
    return 0.0f;
  };
  rt.update(0.1f);
  if (!near(weight("idle"), 1.0f)) return fail("speed 0 = idle");
  rt.set_float("speed", 3.15f);
  rt.update(0.1f);
  if (!near(weight("walk"), 0.5f) || !near(weight("run"), 0.5f)) return fail("speed 3.15 = walk/run half");
  // Footsteps from the dominant clip while walking: two per cycle.
  rt.set_float("speed", 1.8f);
  rt.take_events();
  const float walk = m.clips[static_cast<std::size_t>(m.find_clip("walk"))].duration;
  for (int i = 0; i < 60; ++i) rt.update(walk / 60.0f);
  int steps = 0;
  for (const auto& e : rt.take_events()) steps += e.name == "footstep" ? 1 : 0;
  if (steps != 2) return fail("footstep events per walk cycle: " + std::to_string(steps));
  // Jump trigger: crossfade (0.1 s), stays airborne at the end, lands.
  rt.set_bool("grounded", false);
  rt.trigger("jump");
  rt.update(1.0f / 60.0f);
  if (rt.state(0) != "jump" || !(rt.fade_progress(0) < 1.0f)) return fail("jump trigger crossfade");
  if (rt.param("jump") != 0.0f) return fail("trigger consumed");
  for (int i = 0; i < 12; ++i) rt.update(1.0f / 60.0f);
  if (rt.fade_progress(0) != 1.0f) return fail("crossfade settles");
  for (int i = 0; i < 60; ++i) rt.update(1.0f / 60.0f);
  if (rt.state(0) != "jump") return fail("jump holds while airborne");
  rt.set_bool("grounded", true);
  rt.update(1.0f / 60.0f);
  if (rt.state(0) != "locomotion") return fail("lands back to locomotion");
  // Upper-body layer: "wave" only moves the right arm; the legs keep the
  // base pose.
  for (int i = 0; i < 30; ++i) rt.update(1.0f / 60.0f);
  if (!rt.play("wave", 0.1f)) return fail("play wave");
  if (rt.state(1) != "clip:wave" || rt.state(0) != "locomotion") return fail("wave goes to the Upper Body layer");
  for (int i = 0; i < 30; ++i) rt.update(1.0f / 60.0f);
  r3::AnimatorRuntime base;
  base.bind(s3::default_locomotion_animator(), &m);
  base.set_float("speed", 1.8f);
  // Same base timeline as rt: compare a leg (layer leaves it) and the arm.
  const int leg = m.skeleton.find("upper_leg.L"), arm = m.skeleton.find("upper_arm.R");
  const r3::Pose with = rt.pose();
  rt.stop_layer(1, 0.0f);
  rt.update(0.0f);
  const r3::Pose without = rt.pose();
  if (!near(with[static_cast<std::size_t>(leg)].r.x, without[static_cast<std::size_t>(leg)].r.x, 1e-5f)) {
    return fail("upper-body layer touched the legs");
  }
  if (near(with[static_cast<std::size_t>(arm)].r.z, without[static_cast<std::size_t>(arm)].r.z, 0.05f)) {
    return fail("upper-body layer did not raise the arm");
  }
  // play() of an unknown name; ad-hoc clip returns to the entry state.
  if (rt.play("moonwalk")) return fail("unknown clip plays");
  rt.play("jump", 0.0f);  // a state
  if (rt.state(0) != "jump") return fail("play(state)");
  rt.play("walk", 0.0f);  // a full-body clip goes on the base layer: once, then back to entry
  if (rt.state(0) != "clip:walk") return fail("play(clip) on the base layer: " + rt.state(0));
  for (int i = 0; i < 200; ++i) rt.update(1.0f / 60.0f);
  if (rt.state(0) != "locomotion") return fail("one-shot returns to the entry state: " + rt.state(0));
  rt.play("walk", 0.0f, true);
  for (int i = 0; i < 200; ++i) rt.update(1.0f / 60.0f);
  if (rt.state(0) != "clip:walk") return fail("hold keeps the clip");

  // Root motion: a clip that walks the root 1 m along -z per second.
  r3::Model rm = m;
  r3::Clip stroll;
  stroll.name = "stroll";
  stroll.duration = 1.0f;
  r3::Channel ch;
  ch.joint = 0;
  ch.path = r3::ChannelPath::Translation;
  ch.times = {0, 1};
  ch.values = {0, 0.95f, 0, 0, 0.95f, -1.0f};
  stroll.channels = {ch};
  rm.clips.push_back(stroll);
  s3::Animator3D cfg;
  s3::AnimLayer3D l;
  s3::AnimState3D st;
  st.name = "stroll";
  st.clip = "stroll";
  l.states = {st};
  cfg.layers = {l};
  cfg.root_motion = true;
  r3::AnimatorRuntime walker;
  if (!walker.bind(cfg, &rm)) return fail("bind root motion");
  r3::Vec3 travel;
  for (int i = 0; i < 90; ++i) {
    walker.update(1.0f / 60.0f);
    travel += walker.take_root_motion();
  }
  if (!near(travel.z, -1.5f, 0.02f) || !near(travel.x, 0.0f) || !near(walker.pose()[0].t.z, 0.0f)) {
    return fail("root motion travel " + str(travel) + " (root stays put z=" +
                std::to_string(walker.pose()[0].t.z) + ")");
  }
  return 0;
}

int scene_roundtrip() {
  s3::Scene3D s;
  s3::Entity3D e;
  e.id = 7;
  e.name = "Drifter";
  e.mesh = s3::MeshRef{"assets/drifter.gltf", true};
  e.animator = s3::default_locomotion_animator();
  e.animator->root_motion = true;
  e.animator->root_bone = "hips";
  e.animator->speed = 1.25f;
  e.animator->layers[1].weight = 0.75f;
  e.animator->layers[0].transitions[0].op = s3::AnimTransition3D::Op::Less;
  e.animator->layers[0].transitions[0].value = 0.5f;
  s3::Entity3D rig;
  rig.id = 8;
  rig.name = "Rig";
  rig.vr_rig = s3::VrRig3D{};
  rig.vr_rig->hand_model = "assets/hand.gltf";
  s.entities = {e, rig};
  const std::string text = s3::write(s);
  s3::Scene3D back;
  std::string err;
  if (!s3::parse(text, &back, &err)) return fail("v12 parse: " + err);
  if (!(back.entities == s.entities)) return fail("animator / hand_model round trip");
  if (text.find("\"version\": " + std::to_string(s3::kScene3DVersion)) == std::string::npos) return fail("written as the current version");
  // A v11 scene (no animator) still loads; a newer one is refused.
  const std::string v11 =
      R"({"version": 11, "dimension": "3d", "entities": [{"id": 1, "name": "Crate", "position": [0,0,0],
      "rotation": [0,0,0,1], "scale": [1,1,1], "mesh": {"source": "primitive:cube"}}]})";
  if (!s3::parse(v11, &back, &err) || back.entities.size() != 1 || back.entities[0].animator) {
    return fail("v11 scene loads: " + err);
  }
  std::string newer = v11;
  newer.replace(newer.find("11"), 2, std::to_string(s3::kScene3DVersion + 1));
  if (s3::parse(newer, &back, &err)) return fail("a newer scene version should be refused");
  // Runtime palettes never reach the file.
  s.entities[0].skin_palette = std::make_shared<std::vector<r3::Mat4>>(3);
  if (s3::write(s) != text) return fail("skin_palette leaked into scene.json");
  return 0;
}

int world(const fs::path& dir) {
  std::string err;
  if (!s3::write_ghost_town_sample(dir.string(), "Anim Town", &err)) return fail(err);
  s3::Scene3D scene;
  if (!s3::load_file(s3::scene_path(dir.string()), &scene, &err)) return fail(err);
  scene.editor_camera.yaw = 0.0f;
  for (s3::Entity3D& e : scene.entities) {
    if (e.name == "Boulder") {
      e.script = ts::tombstone::ScriptData{};
      e.script->path = "scripts/animtest.lua";
    }
  }
  w3::World3D w;
  std::vector<std::string> warnings;
  w.set_log_sink([&](const ts::tombstone::runtime::LogEntry& e) {
    if (e.level != ts::tombstone::runtime::LogLevel::Info) warnings.push_back(e.text);
  });
  // A test script on the boulder drives Hank through the Lua anim API.
  w.scripts().put("scripts/animtest.lua", R"lua(
function on_start()
  local hank = find("Old Hank")
  self:set("played", anim.play(hank, "wave", 0.1) and 1 or 0)
  anim.set_param(hank, "speed", 1.8)
  self:set("speed", anim.param(hank, "speed"))
  self:set("bogus", anim.play(hank, "no such clip") and 1 or 0)
end
function on_tick(dt)
  self:set("hank_state", anim.state(find("Old Hank"), 2))
end
)lua");
  if (!w.start(scene, dir.string(), &err)) return fail("start: " + err);
  w3::Input3D idle;
  w.run_ticks(10, idle);
  const std::uint64_t rider = w.player_id(), hank = w.script_find("Old Hank");
  const std::uint64_t boulder = w.script_find("Boulder");
  if (w.animators() != 2) return fail("two animators (rider + Hank), got " + std::to_string(w.animators()));
  const s3::Entity3D* r = w.find(rider);
  if (!r || !r->skin_palette || r->skin_palette->size() != 15) return fail("rider palette");
  auto num = [&](const char* k) {
    const ts::tombstone::ScriptValue* v = w.state(boulder, k);
    return v ? v->number : -1.0;
  };
  if (num("played") != 1.0 || !near(static_cast<float>(num("speed")), 1.8f) || num("bogus") != 0.0) {
    return fail("Lua anim.play / set_param / param: " + std::to_string(num("played")) + " " +
                std::to_string(num("speed")) + " " + std::to_string(num("bogus")) +
                (warnings.empty() ? std::string() : " / " + warnings.front()));
  }
  const ts::tombstone::ScriptValue* hs = w.state(boulder, "hank_state");
  if (!hs || hs->text != "clip:wave") return fail("anim.state(hank, 2) = clip:wave");
  // Graph / C++ side: the same table through the plugin bridge.
  ts::tombstone::ScriptValue res;
  if (!w.script_plugin_call("anim", "set_param",
                            {ts::tombstone::ScriptValue::of_number(static_cast<double>(hank)),
                             ts::tombstone::ScriptValue::of_text("speed"),
                             ts::tombstone::ScriptValue::of_number(4.5)},
                            &res, &err) ||
      w.animator(hank)->param("speed") != 4.5f) {
    return fail("graph bridge anim.set_param: " + err);
  }
  // Ride north: the controller feeds speed; the blend space runs.
  w3::Input3D north;
  north.move_forward = 1.0f;
  w.run_ticks(60, north);
  r3::AnimatorRuntime* ra = w.animator(rider);
  if (!ra || !(ra->param("speed") > 4.0f) || ra->state(0) != "locomotion") return fail("rider speed param");
  const float run_speed = ra->param("speed");
  float run_w = 0.0f;
  for (const auto& [n, k] : ra->state_weights(0)) run_w += n == "run" ? k : 0.0f;
  if (!(run_w > 0.9f)) return fail("riding at full speed runs");
  const ts::tombstone::ScriptValue* steps = w.state(rider, "steps");
  if (!steps || steps->number < 2) return fail("footstep events reached on_anim_event");
  // Jump: the takeoff trips the trigger.
  w3::Input3D jump;
  jump.jump = true;
  w.step(jump);
  w.run_ticks(3, idle);
  if (ra->state(0) != "jump") return fail("jump state on takeoff: " + ra->state(0));
  w.run_ticks(90, idle);
  if (ra->state(0) != "locomotion" || !w.grounded()) return fail("landed back in locomotion");
  if (!warnings.empty()) return fail("warnings: " + warnings.front());
  // Lua handle method play() (2D API) also works on 3D animators.
  if (!w.script_play(hank, "jump", false) || w.animator(hank)->state(0) != "jump") return fail("script_play");
  std::cout << "[smoke] anim3d world OK (2 animators, rider speed " << run_speed
            << " m/s ran, " << steps->number << " footsteps via on_anim_event, jump + landing, "
            << "Lua anim.play/set_param/param/state, graph bridge set_param)\n";
  return 0;
}

struct Px {
  int r, g, b;
};
Px pixel(const std::vector<unsigned char>& rgba, int w, int h, float sx, float sy) {
  const int x = std::clamp(static_cast<int>(sx), 0, w - 1);
  const int y = std::clamp(h - 1 - static_cast<int>(sy), 0, h - 1);
  const unsigned char* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
  return {p[0], p[1], p[2]};
}
// Saturated red (the warm default sky is orange-ish, so use a ratio).
bool red(const Px& p) { return p.r > 60 && p.r > 3 * p.g && p.r > 3 * p.b; }

int gpu_render(const fs::path& dir) {
  r3::HiddenGl ctx;
  std::string why;
  if (!ctx.open(&why)) {
    if (std::getenv("TS_SMOKE_REQUIRE_GL")) return fail("GL context: " + why);
    std::cout << "[smoke] anim3d render SKIPPED (" << why << ")\n";
    return 0;
  }
  auto renderer = r3::make_gl_renderer3d();
  if (!renderer->init(&why)) return fail("GL init: " + why);
  renderer->set_offscreen(true);
  // A red bar: lower half on "root", upper half on "arm" (blended at the
  // joint), written as a skinned glTF and drawn through SceneView.
  r3::Skeleton sk = two_bones();
  r3::GltfPart part;
  part.mesh = r3::make_cube(1.0f);
  part.mesh.name = "bar";
  part.material.name = "red";
  part.material.base_color[0] = 0.9f;
  part.material.base_color[1] = 0.08f;
  part.material.base_color[2] = 0.06f;
  part.material.double_sided = true;
  // Stretch the cube into a 0.3 x 2 x 0.3 bar standing on the origin.
  for (r3::Vertex3D& v : part.mesh.vertices) v.position = r3::mul(v.position, {0.3f, 2.0f, 0.3f}) + r3::Vec3{0, 1, 0};
  part.mesh.skin.resize(part.mesh.vertices.size());
  for (std::size_t i = 0; i < part.mesh.vertices.size(); ++i) {
    const bool top = part.mesh.vertices[i].position.y > 1.5f;
    part.mesh.skin[i].joints[0] = top ? 1.0f : 0.0f;
    part.mesh.skin[i].weights[0] = 1.0f;
  }
  std::string err;
  if (!r3::write_skinned_gltf((dir / "bar.gltf").string(), {part}, sk, {}, &err)) return fail(err);
  s3::Entity3D bar;
  bar.id = 1;
  bar.name = "Bar";
  bar.mesh = s3::MeshRef{"bar.gltf", false};
  r3::Environment env;
  env.shadows = false;
  r3::Camera3D cam;
  cam.position = {0, 1.2f, 6};
  cam.target = {0, 1.2f, 0};
  const int W = 200, H = 150;
  r3::SceneView view(renderer.get());
  view.set_project_dir(dir.string());
  std::vector<s3::Entity3D> ents{bar};
  // Rest pose: the bar stands; (0, 1.9) is red, (-0.9, 1.0) is sky.
  view.begin(env, ents, cam, W, H);
  r3::FrameStats st = view.end();
  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!renderer->read_pixels(&px, &w, &h)) return fail("read_pixels");
  float sx, sy, tx, ty;
  cam.project({0, 1.9f, 0}, W, H, &sx, &sy);
  cam.project({-0.85f, 1.0f, 0}, W, H, &tx, &ty);
  if (!red(pixel(px, w, h, sx, sy)) || red(pixel(px, w, h, tx, ty)) || st.skinned != 1) {
    const Px a = pixel(px, w, h, sx, sy), c = pixel(px, w, h, tx, ty);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "rest pose render: top (%.0f,%.0f) rgb(%d,%d,%d) side (%.0f,%.0f) rgb(%d,%d,%d) %dx%d",
                  sx, sy, a.r, a.g, a.b, tx, ty, c.r, c.g, c.b, w, h);
    return fail(std::string(buf) + " skinned draws " + std::to_string(st.skinned));
  }
  // Bend the arm 90 degrees about +Z: the top half lies along -X at y=1.
  r3::Pose pose = r3::rest_pose(sk);
  pose[1].r = r3::Quat::axis_angle({0, 0, 1}, r3::radians(90.0f));
  auto pal = std::make_shared<std::vector<r3::Mat4>>();
  r3::skin_palette(sk, pose, pal.get());
  ents[0].skin_palette = pal;
  view.begin(env, ents, cam, W, H);
  st = view.end();
  renderer->read_pixels(&px, &w, &h);
  // Where the CPU reference puts the bent tip.
  const float j1[4] = {1, 0, 0, 0}, w1[4] = {1, 0, 0, 0};
  const r3::Vec3 tip = r3::skin_point(*pal, j1, w1, {0, 1.85f, 0});
  float cx, cy;
  cam.project(tip, W, H, &cx, &cy);
  const Px at_tip = pixel(px, w, h, cx, cy), old_top = pixel(px, w, h, sx, sy);
  if (!red(at_tip) || red(old_top)) {
    char b[200];
    std::snprintf(b, sizeof(b), "bent render: tip %s rgb(%d,%d,%d), old top rgb(%d,%d,%d)", str(tip).c_str(),
                  at_tip.r, at_tip.g, at_tip.b, old_top.r, old_top.g, old_top.b);
    return fail(b);
  }
  // The Drifter renders posed too (all five parts skinned, 15 bones).
  r3::Model drifter;
  s3::Entity3D d;
  d.id = 2;
  d.name = "Drifter";
  d.mesh = s3::MeshRef{"drifter.gltf", true};
  view.begin(env, {d}, cam, W, H);
  st = view.end();
  if (st.skinned != 5 || !view.take_problems().empty()) return fail("drifter skinned draws");
  std::cout << "[smoke] anim3d GPU skinning OK (bone UBO, bent bar tip at " << str(tip)
            << " matches the CPU reference, drifter 5 skinned draws)\n";
  return 0;
}

// Authored clips (.anim3d.json) merge onto a rigged import, and bone lines
// follow the same palette the GPU skins with.
int sidecar_and_bones(const fs::path& dir) {
  const r3::Skeleton sk = two_bones();
  const std::vector<r3::BoneSegment> rest = r3::bone_segments(sk, r3::rest_pose(sk));
  if (rest.size() != 2 || rest[1].parent != 0 || !near3(rest[1].from, {0, 0, 0}) ||
      !near3(rest[1].to, {0, 1, 0})) {
    return fail("rest bone line");
  }
  r3::Pose lifted = r3::rest_pose(sk);
  lifted[1].t = {0, 2, 0};
  const std::vector<r3::Vec3> up = r3::joint_positions(sk, lifted);
  if (!near3(up[1], {0, 2, 0})) return fail("lifted joint " + str(up[1]));
  std::vector<r3::Mat4> pal;
  r3::skin_palette(sk, lifted, &pal);
  const std::vector<r3::Vec3> from_pal = r3::joint_positions(sk, pal);
  if (!near3(from_pal[0], up[0]) || !near3(from_pal[1], up[1])) return fail("palette joint origin");

  r3::Clip bend;
  bend.name = "bend";
  r3::Channel rot;
  rot.joint = 1;
  rot.path = r3::ChannelPath::Rotation;
  rot.interp = r3::Interp::Linear;
  const r3::Quat q90 = r3::Quat::axis_angle({0, 0, 1}, r3::radians(90.0f));
  const float ident[4] = {0, 0, 0, 1};
  const float bent[4] = {q90.x, q90.y, q90.z, q90.w};
  if (r3::upsert_key(&rot, 0.0f, ident) != 0 || r3::upsert_key(&rot, 1.0f, bent) != 1) return fail("upsert");
  r3::set_channel_interp(&rot, r3::Interp::Step);
  float held[4];
  r3::sample_channel(rot, 0.9f, held);
  if (!near(held[3], 1.0f) || !near(held[2], 0.0f)) return fail("step holds the first key");
  r3::set_channel_interp(&rot, r3::Interp::Linear);
  if (r3::move_key(&rot, 1, 0.5f) != 1) return fail("move key");
  if (!r3::erase_key(&rot, 1) || rot.times.size() != 1) return fail("erase key");
  r3::upsert_key(&rot, 1.0f, bent);
  bend.channels.push_back(rot);
  r3::clip_duration(&bend);
  if (!near(bend.duration, 1.0f)) return fail("clip duration");

  const std::string json = r3::clips_to_json(sk, {bend});
  std::vector<r3::Clip> back;
  std::string err;
  if (!r3::clips_from_json(json, sk, &back, &err) || back.size() != 1 || back[0].channels.size() != 1) {
    return fail("clip json round trip: " + err);
  }
  r3::Pose pose = r3::rest_pose(sk);
  r3::sample_clip(back[0], 1.0f, false, &pose);
  if (!near(pose[1].r.z, q90.z, 1e-4f) || !near(pose[1].r.w, q90.w, 1e-4f)) return fail("sampled bend");

  // A rigged glTF plus a sidecar: "bend" overrides the file, "extra" is new,
  // and an unknown joint is reported without dropping the good channel.
  r3::GltfPart part;
  part.mesh = r3::make_cube(1.0f);
  part.mesh.name = "bar";
  part.material.name = "red";
  part.material.base_color[0] = 0.9f;
  part.material.base_color[1] = 0.08f;
  part.material.base_color[2] = 0.06f;
  for (r3::Vertex3D& v : part.mesh.vertices) v.position = r3::mul(v.position, {0.3f, 2.0f, 0.3f}) + r3::Vec3{0, 1, 0};
  part.mesh.skin.resize(part.mesh.vertices.size());
  for (std::size_t i = 0; i < part.mesh.vertices.size(); ++i) {
    const bool top = part.mesh.vertices[i].position.y > 1.5f;
    part.mesh.skin[i].joints[0] = top ? 1.0f : 0.0f;
    part.mesh.skin[i].weights[0] = 1.0f;
  }
  r3::Clip keep;
  keep.name = "keep";
  r3::Channel hold;
  hold.joint = 1;
  hold.path = r3::ChannelPath::Translation;
  hold.times = {0.0f, 1.0f};
  hold.values = {0, 1, 0, 0, 1, 0};
  keep.channels = {hold};
  keep.duration = 1.0f;
  r3::Clip file_bend = bend;
  file_bend.channels[0].values = {0, 0, 0, 1, 0, 0, 0, 1};  // stays identity
  const fs::path gltf = dir / "bar.gltf";
  if (!r3::write_skinned_gltf(gltf.string(), {part}, sk, {keep, file_bend}, &err)) return fail(err);
  r3::Clip extra;
  extra.name = "extra";
  r3::Channel slide = hold;
  slide.values = {0, 1, 0, 0, 1, 1};
  extra.channels = {slide};
  extra.duration = 1.0f;
  r3::Clip side_bend = bend;
  const char* ghost_json =
      R"({"version":1,"clips":[{"name":"nod","channels":[{"joint":"ghost","path":"scale","interp":"linear","keys":[{"t":0,"v":[2,2,2]}]},{"joint":"arm","path":"translation","interp":"linear","keys":[{"t":0,"v":[0,1,0]},{"t":1,"v":[0,2,0]}]}]}]})";
  std::vector<r3::Clip> ghost_clips;
  if (!r3::clips_from_json(ghost_json, sk, &ghost_clips, &err) || ghost_clips.size() != 1 ||
      ghost_clips[0].channels.size() != 1 || err.find("ghost") == std::string::npos) {
    return fail("unknown joint should be dropped and named: " + err);
  }
  pose = r3::rest_pose(sk);
  r3::sample_clip(ghost_clips[0], 1.0f, false, &pose);
  if (!near3(pose[1].t, {0, 2, 0})) return fail("kept channel after an unknown joint");
  if (!r3::save_clip_file(r3::clip_sidecar_path(gltf.string()), sk, {side_bend, extra}, &err)) {
    return fail("save sidecar: " + err);
  }
  r3::Model loaded;
  err.clear();
  if (!r3::load_model(gltf.string(), &loaded, &err)) return fail("load sidecar model: " + err);
  if (!err.empty()) return fail("clean sidecar warned: " + err);
  if (loaded.find_clip("keep") < 0 || loaded.find_clip("extra") < 0 || loaded.find_clip("bend") < 0) {
    return fail("merged clip names");
  }
  pose = r3::rest_pose(loaded.skeleton);
  r3::sample_clip(loaded.clips[static_cast<std::size_t>(loaded.find_clip("bend"))], 1.0f, false, &pose);
  r3::skin_palette(loaded.skeleton, pose, &pal);
  const float j1[4] = {1, 0, 0, 0}, w1[4] = {1, 0, 0, 0};
  const r3::Vec3 tip = r3::skin_point(pal, j1, w1, {0, 2, 0});
  if (!near3(tip, {-1, 1, 0}, 1e-3f)) return fail("sidecar bend skin " + str(tip));
  // The file's own "keep" clip is still the imported one (arm stays at y=1).
  pose = r3::rest_pose(loaded.skeleton);
  r3::sample_clip(loaded.clips[static_cast<std::size_t>(loaded.find_clip("keep"))], 1.0f, false, &pose);
  if (!near3(pose[1].t, {0, 1, 0})) return fail("imported clip was replaced");

  // A sidecar that does not parse leaves the imported clips alone.
  {
    std::ofstream out(r3::clip_sidecar_path(gltf.string()), std::ios::binary);
    out << "{ not json";
  }
  r3::Model broken;
  err.clear();
  if (!r3::load_model(gltf.string(), &broken, &err)) return fail("bad sidecar should still import: " + err);
  if (broken.find_clip("keep") < 0 || broken.find_clip("bend") < 0 || broken.find_clip("extra") >= 0) {
    return fail("bad sidecar changed the imported clips");
  }
  if (err.empty()) return fail("bad sidecar should warn");

  std::cout << "[smoke] anim3d rigged sidecar OK (bone lines, palette origins, "
               "clip round trip, import merge, GPU skinning palette)\n";
  return 0;
}

// The editor's animation preview poses the selection in the edit view (null
// renderer: counts skinned draws) without touching the authored scene.
int editor_preview(const fs::path& dir) {
  namespace ed = ts::tombstone::editor;
  std::string err;
  if (!s3::write_ghost_town_sample(dir.string(), "Anim Editor", &err)) return fail(err);
  ed::ProjectInfo info;
  info.id = "_anim3d_editor";
  info.name = "Anim Editor";
  info.kind = ed::ProjectKind::ThreeD;
  info.path = dir.string();
  ed::Editor3DScreen e(info);
  e.on_enter();
  const s3::Entity3D* hank = e.scene().find_named("Old Hank");
  if (!hank || !hank->animator) return fail("editor: Old Hank with an animator");
  const r3::Model* m = e.entity_model(*hank);
  if (!m || !m->skinned() || m->clips.size() != 5) return fail("editor: entity_model");
  const s3::Scene3D before = e.scene();
  e.select(hank->id);
  e.anim_preview().open = true;
  e.anim_preview().clip = "wave";
  e.anim_preview().playing = true;
  e.on_update(0.25f);
  if (!near(e.anim_preview().time, 0.25f, 1e-3f)) return fail("editor: preview clock");
  const r3::FrameStats st = e.render_frame(160, 120);
  if (st.skinned < 10) return fail("editor: skinned draws " + std::to_string(st.skinned));
  s3::Entity3D copy = *e.scene().find(hank->id);
  if (!e.anim_preview_pose(&copy) || !copy.skin_palette) return fail("editor: preview pose");
  const int arm = m->skeleton.find("upper_arm.R");
  const r3::Mat4& pa = (*copy.skin_palette)[static_cast<std::size_t>(arm)];
  if (near(pa.m[0], 1.0f, 1e-3f) && near(pa.m[5], 1.0f, 1e-3f)) return fail("editor: wave did not move the arm");
  if (!(e.scene().entities == before.entities) || e.scene().find(hank->id)->skin_palette) {
    return fail("editor: preview changed the authored scene");
  }
  // Bones follow the preview palette (the overlay draws these positions).
  const int hand = m->skeleton.find("hand.R");
  e.set_show_bones(true);
  e.set_bone_joint(hand);
  if (!e.show_bones() || e.bone_joint() != hand) return fail("editor: bone overlay toggle");
  const r3::FrameStats boned = e.render_frame(160, 120);
  if (boned.skinned < 10) return fail("editor: bones overlay skinned draws");
  const std::vector<r3::Vec3> posed = r3::joint_positions(m->skeleton, *copy.skin_palette);
  const std::vector<r3::Vec3> rest = r3::joint_positions(m->skeleton, r3::rest_pose(m->skeleton));
  if (hand < 0 || near3(posed[static_cast<std::size_t>(hand)], rest[static_cast<std::size_t>(hand)], 1e-3f)) {
    return fail("editor: preview bones did not follow the wave");
  }
  e.on_exit();
  std::cout << "[smoke] anim3d editor preview OK (wave at 0.25 s, " << st.skinned << " skinned draws)\n";
  return 0;
}

}  // namespace

int run_anim3d_smoke() {
  const fs::path dir = fs::absolute("./TombStoneProjects/_anim3d_smoke");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (skin_math() != 0 || sampling() != 0 || blend_weights() != 0) return 1;
  if (sidecar_and_bones(dir) != 0) return 1;
  r3::Model drifter;
  if (assets(dir, &drifter) != 0) return 1;
  if (animator(drifter) != 0) return 1;
  if (scene_roundtrip() != 0) return 1;
  if (gpu_render(dir) != 0) return 1;
  if (world(dir / "town") != 0) return 1;
  if (editor_preview(dir / "editor") != 0) return 1;
  std::cout << "[smoke] anim3d OK (skin math vs reference, linear/step/cubic + slerp, 1D blend weights, "
               "animator: blend space, crossfade, jump trigger, upper-body mask, events, root motion; "
               "scene v12 round trip)\n";
  fs::remove_all(dir, ec);
  return 0;
}
