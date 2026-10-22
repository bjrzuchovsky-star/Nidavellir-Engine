// --smoke-templates, part 8: every engine template becomes a claim in a temp
// folder, opens in its editor (headless: null renderer, no window) and
// rides Play for a few frames. The Empty templates are checked for what
// they promise (entities, components, README, starter script, no plugins)
// and driven a little (walk, jump, look, teleport, snap turn).
#include "SmokeTemplates.h"

#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/projects/ProjectStore.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/templates/TemplateStore.h"
#include "render3d/Scene3D.h"
#include "runtime/Input.h"
#include "runtime/World.h"
#include "store/Json.h"
#include "world3d/World3D.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;
namespace ed = ts::tombstone::editor;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;

namespace {

int fail(const std::string& why) {
  std::cerr << "[smoke] templates play: " << why << '\n';
  return 1;
}

constexpr int kFrames = 20;  // "a few frames" of Play per claim

std::size_t count_if(const std::vector<Entity2D>& es, bool (*pred)(const Entity2D&)) {
  std::size_t n = 0;
  for (const Entity2D& e : es) n += pred(e) ? 1 : 0;
  return n;
}

// 2D: open in the 2D editor (no seeding: the scene is the template's) and
// ride. drive(world tick, input) can steer player 0 per frame.
struct Ride2D {
  std::uint64_t ticks = 0;
  std::size_t errors = 0;
};

template <typename Drive>
bool ride_2d(ed::Editor2DScreen& scr, int frames, Drive drive, Ride2D* out, std::string* why) {
  if (!scr.start_play()) {
    *why = "Play did not start";
    return false;
  }
  for (int i = 0; i < frames; ++i) {
    runtime::InputFrame in;
    drive(i, in.slot(0));
    scr.update_play(1.0 / 60.0, in);
  }
  out->ticks = scr.play_session().world().tick();
  return true;
}

// VR tracking-space pose for a world point / direction (inverse rig).
w3::VrPose tracked(const w3::World3D& w, r3::Vec3 world_pos, r3::Vec3 dir) {
  dir = r3::normalize(dir);
  const float yaw = std::atan2(-dir.x, -dir.z);
  const float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, dir.y)));
  const r3::Quat world_rot = r3::Quat::axis_angle({0, 1, 0}, yaw) * r3::Quat::axis_angle({1, 0, 0}, pitch);
  const r3::Quat rig_rot = r3::Quat::axis_angle({0, 1, 0}, r3::radians(w.rig_yaw()));
  w3::VrPose p;
  p.position = w.rig_matrix().inverse().transform_point(world_pos);
  p.orientation = rig_rot.conjugate() * world_rot;
  p.valid = true;
  return p;
}

int check_empty_2d(const ed::ProjectTemplate& t, const ed::ProjectInfo& p, std::string* note) {
  ed::Editor2DScreen scr(p);
  scr.on_enter();
  const auto& es = scr.workspace().entities();
  const fs::path dir(p.path);
  if (fs::exists(dir / "assets" / "rider.png")) return fail(t.id + ": opening seeded the sample rider");
  const std::size_t cams = count_if(es, [](const Entity2D& e) { return e.camera.has_value(); });
  const std::size_t maps = count_if(es, [](const Entity2D& e) { return e.tilemap.has_value(); });
  const std::size_t players = count_if(es, [](const Entity2D& e) { return e.player.has_value(); });
  if (scr.workspace().lighting().enabled) return fail(t.id + ": 2D lighting is off");
  const Entity2D* player = find_entity_named(es, "Player");
  const Entity2D* cam = find_entity_named(es, "Camera2D");
  const Entity2D* map = find_entity_named(es, "TileMap");
  Ride2D r;
  std::string why;
  if (t.id == "empty-2d") {
    if (es.size() != 2 || cams != 1 || maps != 1 || !map || map->tilemap->count_nonempty() != 0) {
      return fail("empty-2d: a camera and one empty tilemap layer, nothing else");
    }
    if (fs::exists(dir / "scripts" / "player.lua")) return fail("empty-2d has no player script");
    if (!ride_2d(scr, kFrames, [](int, runtime::PlayerInput&) {}, &r, &why)) return fail(t.id + ": " + why);
  } else if (t.id == "empty-2d-topdown") {
    if (es.size() != 3 || cams != 1 || maps != 1 || players != 1 || !player || !cam ||
        player->player->mode != PlayerControllerData::Mode::TopDown || cam->camera->target != player->id ||
        map->tilemap->count_nonempty() != 0 || !player->script || player->script->path != "scripts/player.lua") {
      return fail("empty-2d-topdown: top-down player + script, follow camera, empty tilemap");
    }
    const std::uint64_t pid = player->id;
    float x0 = 0, y0 = 0;
    if (!ride_2d(scr, kFrames,
                 [&](int i, runtime::PlayerInput& in) {
                   if (i == 0) {
                     const runtime::Actor* a = scr.play_session().world().find(pid);
                     x0 = a->data.x;
                     y0 = a->data.y;
                   }
                   in = runtime::digital_input(false, true, false, true);  // right + down
                 },
                 &r, &why)) {
      return fail(t.id + ": " + why);
    }
    const runtime::Actor* a = scr.play_session().world().find(pid);
    if (!a || a->data.x <= x0 + 20.0f || a->data.y <= y0 + 20.0f) {
      return fail("empty-2d-topdown: the player walks on both axes");
    }
  } else if (t.id == "empty-2d-platformer") {
    if (es.size() != 3 || cams != 1 || maps != 1 || players != 1 || !player || !cam ||
        player->player->mode != PlayerControllerData::Mode::Platformer || cam->camera->target != player->id ||
        map->tilemap->count_nonempty() == 0 || !player->script) {
      return fail("empty-2d-platformer: platformer player + script, ground tiles, follow camera");
    }
    const TileSolidity& sol = scr.workspace().tile_solidity();
    bool solid_ground = false;
    for (int c = 0; c < map->tilemap->cols; ++c) {
      const int id = map->tilemap->at(c, map->tilemap->rows - 1);
      solid_ground = solid_ground || (id != 0 && sol.solid(map->tilemap->tileset, id));
    }
    if (!solid_ground) return fail("empty-2d-platformer: the ground tiles are solid");
    const std::uint64_t pid = player->id;
    // Fall and land, then jump (up), then walk right.
    float landed_y = 0, peak_y = 1e9f, x_before_walk = 0;
    if (!ride_2d(scr, 150,
                 [&](int i, runtime::PlayerInput& in) {
                   const runtime::Actor* a = scr.play_session().world().find(pid);
                   if (i == 60) landed_y = a->data.y;
                   if (i > 60 && i < 100) peak_y = std::min(peak_y, a->data.y);
                   if (i == 100) x_before_walk = a->data.x;
                   if (i >= 61 && i <= 63) {  // jump: up + action
                     in = runtime::digital_input(false, false, true, false);
                     in.buttons |= runtime::kButtonAction;
                   }
                   if (i >= 100) in = runtime::digital_input(false, true, false, false);
                 },
                 &r, &why)) {
      return fail(t.id + ": " + why);
    }
    const runtime::Actor* a = scr.play_session().world().find(pid);
    const float ground_top = map->y + (map->tilemap->rows - 2) * map->tilemap->tile_size;
    if (std::fabs(landed_y + a->data.h - ground_top) > 1.0f) {
      return fail("empty-2d-platformer: the player lands on the ground (feet " +
                  std::to_string(landed_y + a->data.h) + ", ground " + std::to_string(ground_top) + ")");
    }
    if (peak_y > landed_y - 40.0f) return fail("empty-2d-platformer: up jumps");
    if (a->data.x <= x_before_walk + 20.0f || std::fabs(a->data.y - landed_y) > 1.0f) {
      return fail("empty-2d-platformer: walks right on the ground after the jump");
    }
    *note += " platformer jump " + std::to_string(static_cast<int>(landed_y - peak_y)) + " px;";
  } else {  // truly-blank (2D)
    if (!es.empty()) return fail("truly-blank 2D has no entities");
    if (!ride_2d(scr, kFrames, [](int, runtime::PlayerInput&) {}, &r, &why)) return fail(t.id + ": " + why);
  }
  if (r.ticks < 10) return fail(t.id + ": Play ticked " + std::to_string(r.ticks) + " times");
  if (scr.telegraph().count(runtime::LogLevel::Error) != 0) {
    return fail(t.id + ": errors on the Telegraph during Play");
  }
  scr.stop_play();
  if (scr.workspace().entities().size() != es.size()) return fail(t.id + ": Stop restores the scene");
  scr.on_exit();
  return 0;
}

std::size_t errors_in(const std::deque<runtime::LogEntry>& log) {
  std::size_t n = 0;
  for (const auto& e : log) n += e.level == runtime::LogLevel::Error ? 1 : 0;
  return n;
}

int check_empty_3d(const ed::ProjectTemplate& t, const ed::ProjectInfo& p, int dimension,
                   std::string* note) {
  ed::Editor3DScreen e(p);
  e.on_enter();
  const s3::Scene3D& s = e.scene();
  const s3::Entity3D* ground = s.find_named("Ground");
  const s3::Entity3D* sun = s.find_named("Sun");
  const s3::Entity3D* cam = s.find_named("Camera");
  const s3::Entity3D* player = s.find_named("Player");
  const bool ground_ok = ground && ground->mesh && ground->collider && !ground->collider->trigger;
  const bool sun_ok = sun && sun->light && sun->light->type == s3::Light3D::Type::Directional;
  if (t.id == "truly-blank") {
    (void)dimension;
    if (!s.entities.empty()) return fail("truly-blank 3D has no entities (opening must not seed)");
  } else if (!ground_ok || !sun_ok) {
    return fail(t.id + ": a sun light and a ground plane");
  }
  std::size_t props = 0;
  for (const s3::Entity3D& x : s.entities) {
    props += (x.mesh && &x != ground && &x != player) || x.grabbable ? 1 : 0;
  }
  if (props) return fail(t.id + ": no props");
  if (t.id == "empty-3d") {
    if (s.entities.size() != 3 || !cam || !cam->camera || player) return fail("empty-3d: camera, sun, ground");
  } else if (t.id == "empty-3d-fps") {
    if (!player || !player->player || player->player->view != s3::Player3D::View::FirstPerson ||
        !cam || !cam->camera || cam->parent != player->id || !player->script) {
      return fail("empty-3d-fps: first-person controller with its camera child + script");
    }
  } else if (t.id == "empty-3d-tps") {
    if (!player || !player->player || player->player->view != s3::Player3D::View::ThirdPerson ||
        !player->mesh || !player->script) {
      return fail("empty-3d-tps: third-person controller + script");
    }
  }
  if (!e.play_start()) return fail(t.id + ": Play did not start");
  w3::World3D& w = e.world();
  e.play_ticks(kFrames, {});
  if (w.tick() < static_cast<std::uint64_t>(kFrames)) return fail(t.id + ": Play ticks");
  if (t.id == "empty-3d") {
    const r3::Vec3 d = w.camera().position - cam->transform.position;
    if (r3::length(d) > 1e-3f || w.player_id() != 0) return fail("empty-3d: Play looks through the Camera");
  } else if (t.id == "empty-3d-fps" || t.id == "empty-3d-tps") {
    const r3::Vec3 start = w.find(w.player_id())->transform.position;
    w3::Input3D walk;
    walk.move_forward = 1.0f;
    if (t.id == "empty-3d-fps") walk.look = 1.0f;
    e.play_ticks(40, walk);
    const s3::Entity3D* rider = w.find(w.player_id());
    const r3::Vec3 end = rider->transform.position;
    const float moved = r3::length(r3::Vec3{end.x - start.x, 0.0f, end.z - start.z});
    if (moved < 1.0f || !w.grounded()) return fail(t.id + ": the rider walks on the ground");
    const r3::Camera3D& c = w.camera();
    if (t.id == "empty-3d-fps") {
      const s3::Entity3D* eye = w.find(cam->id);
      if (!eye || r3::length(c.position - eye->transform.position) > 1e-3f || w.camera_pitch() < 30.0f ||
          std::fabs(c.position.y - 1.6f) > 0.1f) {
        return fail("empty-3d-fps: the camera rides at eye height and looks up");
      }
    } else {
      const float back = r3::length(r3::Vec3{c.position.x - end.x, 0.0f, c.position.z - end.z});
      if (back < 3.0f || c.position.y < end.y + 1.0f) return fail("empty-3d-tps: the orbit camera sits behind");
    }
  }
  if (errors_in(w.log())) return fail(t.id + ": errors during Play");
  e.play_stop();
  e.on_exit();
  (void)note;
  return 0;
}

int check_empty_vr(const ed::ProjectInfo& p, std::string* note) {
  // The rig in the editor, then the simulator preview.
  {
    ed::Editor3DScreen e(p);
    e.on_enter();
    const s3::Entity3D* rig = e.scene().find_named("VR Rig");
    const s3::Entity3D* ground = e.scene().find_named("Ground");
    if (!e.vr_project() || !rig || !rig->vr_rig || rig->vr_rig->hand_model.empty() ||
        !fs::exists(fs::path(p.path) / rig->vr_rig->hand_model) ||
        rig->vr_rig->locomotion != s3::VrRig3D::Locomotion::Teleport || rig->vr_rig->snap_turn_deg <= 0.0f ||
        !ground || !ground->collider || !rig->script) {
      return fail("empty-vr: XR rig with hands, teleport + snap turn, ground, starter script");
    }
    if (!e.vr_preview_start(true) || !e.vr_sim()) return fail("empty-vr: simulator preview");
    for (int i = 0; i < kFrames; ++i) e.vr_preview_frame(1.0 / 60.0);
    if (e.world().tick() == 0 || errors_in(e.world().log())) return fail("empty-vr: the preview rides");
    e.vr_preview_stop();
    // Desktop fallback: flat Play rides the rig as a rider.
    if (!e.play_start()) return fail("empty-vr: desktop Play");
    w3::Input3D walk;
    walk.move_forward = 1.0f;
    e.play_ticks(40, walk);
    if (e.world().player_id() != rig->id || e.world().tick() < 40) {
      return fail("empty-vr: on the desktop the rig rides as the rider");
    }
    e.play_stop();
    e.on_exit();
  }
  // Snap turn + teleport on the world with VR on.
  s3::Scene3D s;
  std::string err;
  if (!s3::load_file(s3::scene_path(p.path), &s, &err)) return fail("empty-vr: " + err);
  w3::World3D w;
  w.set_vr(true);
  if (!w.start(s, p.path, &err)) return fail("empty-vr: start " + err);
  w3::VrInput in;
  in.head.position = {0.0f, 1.7f, 0.0f};
  in.head.valid = true;
  for (int h = 0; h < 2; ++h) {
    in.hands[h].grip.position = {h ? 0.2f : -0.2f, 1.1f, -0.3f};
    in.hands[h].grip.valid = true;
    in.hands[h].aim = in.hands[h].grip;
  }
  auto tick = [&](int n) {
    for (int i = 0; i < n; ++i) {
      w.set_vr_input(in);
      w.step({});
    }
  };
  tick(5);
  const float yaw0 = w.rig_yaw();
  in.hands[1].stick_x = 1.0f;
  tick(3);
  in.hands[1].stick_x = 0.0f;
  tick(3);
  if (w.vr_stats().snap_turns != 1 || std::fabs(std::fabs(w.rig_yaw() - yaw0) - 45.0f) > 0.5f) {
    return fail("empty-vr: one 45 degree snap turn");
  }
  const r3::Vec3 h0 = w.head().position;
  const r3::Vec3 fwd = w.head().forward();
  in.hands[1].aim = tracked(w, h0 + r3::Vec3{0.0f, -0.3f, 0.0f},
                            r3::Vec3{fwd.x, 0.0f, fwd.z} * 0.87f + r3::Vec3{0.0f, -0.5f, 0.0f});
  in.hands[1].stick_y = 1.0f;
  tick(10);
  const bool aimed = w.teleport().valid;
  in.hands[1].stick_y = 0.0f;
  tick(3);
  if (!aimed || w.vr_stats().teleports != 1) return fail("empty-vr: aim the arc at the ground and teleport");
  const float hop = r3::length(r3::Vec3{w.head().position.x - h0.x, 0.0f, w.head().position.z - h0.z});
  if (hop < 0.5f) return fail("empty-vr: the teleport moved the rig");
  if (errors_in(w.log())) return fail("empty-vr: errors during the ride");
  *note += " VR snap + teleport " + std::to_string(hop).substr(0, 4) + " m;";
  return 0;
}

int check_sample(const ed::ProjectTemplate& t, const ed::ProjectInfo& p, std::size_t* errors) {
  if (p.kind == ed::ProjectKind::TwoD) {
    ed::Editor2DScreen scr(p);
    scr.on_enter();
    if (scr.workspace().entities().empty()) return fail(t.id + ": the sample scene opens");
    Ride2D r;
    std::string why;
    if (!ride_2d(scr, kFrames, [](int, runtime::PlayerInput& in) { in = runtime::digital_input(false, true, false, false); },
                 &r, &why)) {
      return fail(t.id + ": " + why);
    }
    if (r.ticks < 10) return fail(t.id + ": Play ticks");
    *errors += scr.telegraph().count(runtime::LogLevel::Error);
    scr.stop_play();
    scr.on_exit();
    return 0;
  }
  ed::Editor3DScreen e(p);
  e.on_enter();
  if (e.scene().entities.empty()) return fail(t.id + ": the sample scene opens");
  if (!e.play_start()) return fail(t.id + ": Play did not start");
  w3::Input3D walk;
  walk.move_forward = 1.0f;
  e.play_ticks(kFrames, walk);
  if (e.world().tick() < static_cast<std::uint64_t>(kFrames)) return fail(t.id + ": Play ticks");
  *errors += errors_in(e.world().log());
  e.play_stop();
  e.on_exit();
  return 0;
}

}  // namespace

int run_template_play_smoke(std::string* note) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path base = fs::temp_directory_path() / ("ts_template_play_" + std::to_string(stamp));
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);

  ed::TemplateStore ts;
  ts.refresh_dirs(ed::TemplateStore::engine_template_dirs(), {});
  int claims = 0;
  std::size_t sample_errors = 0;
  std::string extra;
  for (const ed::ProjectTemplate& t : ts.templates()) {
    for (int dim = 0; dim < (t.has_alt ? 2 : 1); ++dim) {
      // One temp folder (projects root) per claim.
      const fs::path root = base / (t.id + (dim ? "-alt" : ""));
      fs::create_directories(root, ec);
      ed::ProjectStore store(root.string());
      ed::ProjectInfo p;
      std::string err;
      const int dimension = dim ? (t.alt_kind == ed::ProjectKind::TwoD ? 0 : 1) : -1;
      if (!ed::TemplateStore::create_project(store, t, "Claim " + t.id, &p, &err, -1, dimension)) {
        return fail("create from " + t.id + ": " + err);
      }
      ++claims;
      const fs::path dir(p.path);
      if (dir.parent_path() != fs::path(root)) return fail(t.id + ": the claim lives in its temp folder");
      if (t.empty_group()) {
        // project.json: name, type, VR flag, no plugins; README; starter script with a player.
        store::Json pj;
        store::read_json_file((dir / "project.json").string(), &pj, &err);
        const ed::ProjectKind want = dim ? t.alt_kind : t.kind;
        if (pj.string_at("name") != "Claim " + t.id || p.kind != want || p.vr != (t.vr && !dim) ||
            !p.plugins_set || !p.plugins.empty() || pj.string_at("plugins") != "") {
          return fail(t.id + ": project.json " + pj.dump());
        }
        if (!fs::is_regular_file(dir / "README.md") || !fs::is_regular_file(dir / "scene.json")) {
          return fail(t.id + ": README.md + scene.json in the claim");
        }
        const bool has_player = t.id != "empty-2d" && t.id != "empty-3d" && t.id != "truly-blank";
        if (has_player != fs::is_regular_file(dir / "scripts" / "player.lua")) {
          return fail(t.id + ": a starter script exactly when there is a player");
        }
        int rc = 0;
        if (t.id == "empty-vr") {
          rc = check_empty_vr(p, &extra);
        } else if (p.kind == ed::ProjectKind::TwoD) {
          rc = check_empty_2d(t, p, &extra);
        } else {
          rc = check_empty_3d(t, p, dimension, &extra);
        }
        if (rc != 0) return rc;
      } else if (check_sample(t, p, &sample_errors) != 0) {
        return 1;
      }
    }
  }
  fs::remove_all(base, ec);
  if (claims < 14) return fail("every template rode (" + std::to_string(claims) + " claims)");
  *note = std::to_string(claims) + " claims opened + played in temp folders;" + extra +
          " sample Telegraph errors " + std::to_string(sample_errors);
  return 0;
}
