#include "SmokeVR.h"

#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "render3d/Scene3D.h"
#include "world3d/World3D.h"
#include "xr/XrSession.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;
namespace ed = ts::tombstone::editor;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] VR FAILED: " << what << '\n';
  return 1;
}

std::uint64_t id_of(const s3::Scene3D& s, const char* name) {
  const s3::Entity3D* e = s.find_named(name);
  return e ? e->id : 0;
}

double num(const w3::World3D& w, std::uint64_t id, const char* key) {
  const auto* v = w.state(id, key);
  return v ? v->number : 0.0;
}

// Orientation whose -Z points along dir.
r3::Quat look_along(r3::Vec3 dir) {
  dir = r3::normalize(dir);
  const float yaw = std::atan2(-dir.x, -dir.z);
  const float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, dir.y)));
  return r3::Quat::axis_angle({0, 1, 0}, yaw) * r3::Quat::axis_angle({1, 0, 0}, pitch);
}

// Drives the world in tracking space; hands are placed in world space.
struct Rig {
  w3::World3D& w;
  w3::VrInput in;

  explicit Rig(w3::World3D& world) : w(world) {
    in.head.position = {0.0f, 1.7f, 0.0f};
    in.head.valid = true;
    for (int h = 0; h < 2; ++h) {
      in.hands[h].grip.position = {h ? 0.2f : -0.2f, 1.1f, -0.3f};
      in.hands[h].grip.valid = in.hands[h].aim.valid = true;
      in.hands[h].aim = in.hands[h].grip;
    }
  }
  // World pose -> tracking pose (inverse of the rig transform).
  w3::VrPose tracked(r3::Vec3 world_pos, r3::Quat world_rot) const {
    const r3::Mat4 inv = w.rig_matrix().inverse();
    const r3::Quat rig_rot = r3::Quat::axis_angle({0, 1, 0}, r3::radians(w.rig_yaw()));
    w3::VrPose p;
    p.position = inv.transform_point(world_pos);
    p.orientation = rig_rot.conjugate() * world_rot;
    p.valid = true;
    return p;
  }
  void place(int h, r3::Vec3 world_pos, r3::Vec3 world_dir = {0, 0, -1}) {
    const w3::VrPose p = tracked(world_pos, look_along(world_dir));
    in.hands[h].grip = p;
    in.hands[h].aim = p;
  }
  void aim_at(int h, r3::Vec3 target) {
    const r3::Vec3 from = w.head().position + r3::Vec3{h ? 0.2f : -0.2f, -0.3f, 0.0f};
    place(h, from, target - from);
  }
  void tick(int n = 1) {
    for (int i = 0; i < n; ++i) {
      w.set_vr_input(in);
      w.step({});
    }
  }
};

int scene_io(const fs::path& dir) {
  std::string err;
  s3::Scene3D s;
  if (!s3::load_file(s3::scene_path(dir.string()), &s, &err)) return fail("load saloon: " + err);
  if (s.version != 8) return fail("saloon is v8");
  const s3::Entity3D* rig = s.find_named("VR Rig");
  const s3::Entity3D* hat = s.find_named("Stetson");
  if (!rig || !rig->vr_rig || !hat || !hat->grabbable) return fail("saloon has rig + grabbables");
  s3::Scene3D again;
  if (!s3::parse(s3::write(s), &again, &err)) return fail("reparse: " + err);
  if (s3::write(again) != s3::write(s)) return fail("v8 round trip is stable");
  const s3::Entity3D* rig2 = again.find_named("VR Rig");
  if (rig2->vr_rig->snap_turn_deg != rig->vr_rig->snap_turn_deg ||
      rig2->vr_rig->locomotion != rig->vr_rig->locomotion ||
      again.find_named("Stetson")->grabbable->throw_scale != hat->grabbable->throw_scale) {
    return fail("vr_rig / grabbable fields survive");
  }
  // v7 files (no VR keys) still load; a newer version is refused.
  std::string v7 = s3::write(s3::make_starter_scene());
  const std::string tag = "\"version\": 8";
  const auto at = v7.find(tag);
  if (at == std::string::npos) return fail("version key in the writer");
  v7.replace(at, tag.size(), "\"version\": 7");
  s3::Scene3D old;
  if (!s3::parse(v7, &old, &err)) return fail("v7 loads: " + err);
  std::string v9 = v7;
  v9.replace(v9.find("\"version\": 7"), tag.size(), "\"version\": 9");
  if (s3::parse(v9, &old, &err)) return fail("v9 refused");
  s3::Scene3D starter;
  if (!s3::parse(s3::write(s3::make_vr_starter_scene()), &starter, &err) ||
      !starter.find_named("VR Rig")) {
    return fail("VR starter scene");
  }
  return 0;
}

int world_drive(const fs::path& dir) {
  std::string err;
  s3::Scene3D s;
  if (!s3::load_file(s3::scene_path(dir.string()), &s, &err)) return fail("load: " + err);
  // A Lua probe of the vr table rides on the bar.
  s3::Entity3D probe;
  probe.id = 9000;
  probe.name = "VR Probe";
  probe.transform.position = {0.0f, -5.0f, 0.0f};
  probe.script = ts::tombstone::ScriptData{};
  probe.script->path = "scripts/_vr_probe.lua";
  s.entities.push_back(probe);
  const std::string before = s3::write(s);

  w3::World3D w;
  w.scripts().put("scripts/_vr_probe.lua", R"lua(
function on_tick(dt)
  self:set("active", vr.active())
  local _, hy = vr.head()
  self:set("head_y", hy)
  local _, ry = vr.hand("right")
  self:set("hand_y", ry)
  local held = vr.held("right")
  self:set("held", held and held:name() or "")
  local pointed = vr.pointed("right")
  self:set("pointed", pointed and pointed:name() or "")
end
)lua");
  w.set_vr(true);
  if (!w.start(s, dir.string(), &err)) return fail("start: " + err);
  Rig r(w);
  r.tick(2);
  if (w.vr_rig_id() != id_of(s, "VR Rig")) return fail("rig found");
  if (std::fabs(w.head().position.y - 1.7f) > 0.01f || std::fabs(w.head().position.z - 3.5f) > 0.01f) {
    return fail("head over the rig");
  }
  const auto* active = w.state(9000, "active");
  if (!active || !active->flag || std::fabs(num(w, 9000, "head_y") - 1.7) > 0.01) {
    return fail("lua vr.active / vr.head");
  }

  const std::uint64_t ghost = id_of(s, "Ghost"), piano = id_of(s, "Piano"),
                      spit = id_of(s, "Spittoon"), hat = id_of(s, "Stetson"),
                      b1 = id_of(s, "Bottle 1"), b2 = id_of(s, "Bottle 2");

  // Laser + trigger on the ghost.
  const s3::Entity3D* g = w.find(ghost);
  r.aim_at(w3::kRightHand, g->transform.position);
  r.tick(2);
  if (w.hand(w3::kRightHand).pointed != ghost) return fail("laser points at the ghost");
  w.take_haptics();
  r.in.hands[1].trigger = 1.0f;
  r.tick(2);
  r.in.hands[1].trigger = 0.0f;
  r.tick(2);
  if (num(w, ghost, "spooks") < 1) return fail("trigger spooks the ghost");
  if (w.take_haptics().empty()) return fail("ghost buzzes the hand");
  if (w.state(9000, "pointed") == nullptr) return fail("lua vr.pointed");

  // And the piano (on_vr_trigger).
  r.aim_at(w3::kRightHand, w.find(piano)->transform.position);
  r.tick(2);
  if (w.hand(w3::kRightHand).pointed != piano) return fail("laser points at the piano");
  r.in.hands[1].trigger = 1.0f;
  r.tick(2);
  r.in.hands[1].trigger = 0.0f;
  r.tick(2);
  if (num(w, piano, "tunes") < 1) return fail("piano plays");

  // Comfort menu: toggle vignette, cycle snap angle.
  r.in.hands[0].menu = true;
  r.tick(1);
  r.in.hands[0].menu = false;
  r.tick(1);
  if (!w.comfort_menu_open() || w.comfort_menu().size() != 4) return fail("menu opens");
  const bool vig = w.vr_settings().vignette;
  const float snap0 = w.vr_settings().snap_turn_deg;
  for (int b = 0; b < 2; ++b) {
    const r3::Aabb box = w.comfort_menu()[b].box;
    r.aim_at(w3::kRightHand, (box.min + box.max) * 0.5f);
    r.tick(2);
    if (w.hand(w3::kRightHand).menu_button != b) return fail("laser on menu button");
    r.in.hands[1].trigger = 1.0f;
    r.tick(1);
    r.in.hands[1].trigger = 0.0f;
    r.tick(1);
  }
  if (w.vr_settings().vignette == vig || w.vr_settings().snap_turn_deg == snap0) {
    return fail("menu toggles vignette / snap angle");
  }
  w.set_comfort_menu(false);

  // Snap turn.
  const float yaw0 = w.rig_yaw();
  r.in.hands[1].stick_x = 1.0f;
  r.tick(3);
  r.in.hands[1].stick_x = 0.0f;
  r.tick(3);
  const float turned = w.rig_yaw() - yaw0;
  if (std::fabs(std::fabs(turned) - w.vr_settings().snap_turn_deg) > 0.5f || w.vr_stats().snap_turns != 1) {
    return fail("one snap turn");
  }
  // Face +X (right wall) whatever the angle, then walk into it.
  s3::VrRig3D cfg = w.vr_settings();
  w.set_vr_settings(cfg);
  r.in.head.orientation = r3::Quat::axis_angle({0, 1, 0}, r3::radians(-90.0f - w.rig_yaw()));
  r.tick(1);
  if (w.head().forward().x < 0.99f) return fail("head faces +X");
  r.in.hands[0].stick_y = 1.0f;
  r.tick(6 * 60);
  r.in.hands[0].stick_y = 0.0f;
  r.tick(1);
  const float hx = w.head().position.x;
  if (w.vr_stats().smooth_moved < 3.0f || hx < 4.0f || hx > 5.9f) {
    return fail("smooth walk stops at the wall (x=" + std::to_string(hx) + ")");
  }

  // Teleport back toward the room (arc down-left).
  const r3::Vec3 h0 = w.head().position;
  r.place(w3::kRightHand, h0 + r3::Vec3{0.0f, -0.3f, 0.0f}, {-0.87f, -0.5f, 0.0f});
  r.in.hands[1].stick_y = 1.0f;
  r.tick(3);
  if (!w.teleport().aiming || !w.teleport().valid) return fail("teleport arc lands");
  const r3::Vec3 target = w.teleport().target;
  r.in.hands[1].stick_y = 0.0f;
  r.tick(2);
  if (w.vr_stats().teleports != 1 || std::fabs(w.head().position.x - target.x) > 0.05f ||
      w.head().position.x > h0.x - 0.5f) {
    return fail("teleported to the arc target");
  }

  // Grab bottle 1, carry it over the spittoon, drop it.
  const r3::Vec3 bottle = w.find(b1)->transform.position;
  r.place(w3::kRightHand, bottle);
  r.in.hands[1].squeeze = 1.0f;
  r.tick(2);
  if (w.hand(w3::kRightHand).held != b1) return fail("grab bottle");
  if (num(w, b1, "grabs") < 1) return fail("on_grab");
  const auto* held = w.state(9000, "held");
  if (!held || held->text != "Bottle 1") return fail("lua vr.held");
  const r3::Vec3 over = w.find(spit)->transform.position + r3::Vec3{0.0f, 0.8f, 0.0f};
  r.place(w3::kRightHand, over);
  r.tick(10);
  r.in.hands[1].squeeze = 0.0f;
  r.tick(90);
  if (w.vr_bodies() != 0 || num(w, spit, "dings") < 1) return fail("bottle drops into the spittoon");

  // Throw bottle 2: a quick swing then let go.
  r.place(w3::kRightHand, w.find(b2)->transform.position);
  r.in.hands[1].squeeze = 1.0f;
  r.tick(2);
  if (w.hand(w3::kRightHand).held != b2) return fail("grab bottle 2");
  r3::Vec3 p = w.find(b2)->transform.position;
  for (int i = 0; i < 6; ++i) {
    p = p + r3::Vec3{0.0f, 0.02f, 0.1f};
    r.place(w3::kRightHand, p);
    r.tick(1);
  }
  r.in.hands[1].squeeze = 0.0f;
  r.tick(1);
  if (w.vr_stats().throws < 1 || w.vr_bodies() != 1) return fail("bottle thrown");
  bool yeehaw = false;
  for (const auto& t : w.take_toasts()) yeehaw |= t.text.rfind("Yeehaw", 0) == 0;
  if (!yeehaw) return fail("on_release(speed) toast");
  r.tick(300);
  if (w.vr_bodies() != 0 || w.find(b2)->transform.position.y < -0.1f) return fail("bottle lands");

  // Force grab the Stetson from across the room.
  w.vr_teleport_to({1.0f, 0.0f, 3.0f});
  r.tick(1);
  r.aim_at(w3::kRightHand, w.find(hat)->transform.position);
  r.tick(2);
  if (w.hand(w3::kRightHand).pointed != hat) return fail("laser on the Stetson");
  r.in.hands[1].squeeze = 1.0f;
  r.tick(30);
  if (w.hand(w3::kRightHand).held != hat ||
      r3::length(w.find(hat)->transform.position - w.hand(w3::kRightHand).grip.position) > 0.3f) {
    return fail("force grab pulls the hat into the hand");
  }
  r.in.hands[1].squeeze = 0.0f;
  r.tick(120);

  for (const auto& e : w.log()) {
    if (e.level == ts::tombstone::runtime::LogLevel::Error) return fail("script error: " + e.text);
  }
  w.stop();
  if (s3::write(s) != before) return fail("authored scene untouched");
  std::cout << "[smoke] VR world: ghost spooked, piano, menu, snap " << turned << " deg, walked "
            << w.vr_stats().smooth_moved << " m, teleport, drop + throw + force grab OK\n";
  return 0;
}

int editor_preview(const fs::path& dir) {
  ed::ProjectInfo info;
  info.id = "_vr_smoke";
  info.name = "Haunted Saloon VR";
  info.kind = ed::ProjectKind::VR;
  info.path = dir.string();
  ed::Editor3DScreen e(info);
  e.on_enter();
  if (!e.vr_project() || !e.scene().find_named("VR Rig")) return fail("editor opens the saloon");
  const std::string before = s3::write(e.scene());
  if (!e.vr_preview_start(true) || !e.vr_previewing() || !e.vr_sim()) return fail("simulator preview");
  for (int i = 0; i < 20; ++i) e.vr_preview_frame(1.0 / 60.0);
  if (e.world().tick() == 0) return fail("preview ticks the world");
  e.vr_preview_stop();
  if (e.vr_previewing() || e.playing()) return fail("preview stops");
  if (s3::write(e.scene()) != before) return fail("preview leaves the scene alone");
  return 0;
}

}  // namespace

int run_vr_smoke() {
  const fs::path dir = fs::path("./TombStoneProjects") / "_vr_saloon_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::string err;
  if (!s3::write_haunted_saloon_sample(dir.string(), "Haunted Saloon VR", &err)) {
    return fail("write sample: " + err);
  }
  if (scene_io(dir) != 0 || world_drive(dir) != 0 || editor_preview(dir) != 0) return 1;
  fs::remove_all(dir, ec);
  std::cout << "[smoke] VR OK\n";
  return 0;
}
