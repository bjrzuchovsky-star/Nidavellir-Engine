#include "editor/templates/EmptyTemplates.h"

#include "render3d/Gltf.h"
#include "render3d/Scene3D.h"
#include "scene/SceneData.h"
#include "scene/SceneJson.h"
#include "store/Json.h"

#include <filesystem>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {
namespace empty_templates {

namespace fs = std::filesystem;
using render3d::Quat;
using render3d::Vec3;

namespace {

bool write_text(const fs::path& p, const std::string& text, std::string* error) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  return store::write_text_file(p.string(), text, error);
}

// project.json: name, dimension, VR flag and an explicitly empty plugin
// list (optional plugins come from the Trading Post). No timestamps:
// create_project stamps its own.
bool write_project(const fs::path& dir, const std::string& name, bool three_d, bool vr,
                   std::string* error) {
  store::Json j = store::Json::object();
  j.set("name", name);
  j.set("dimension", three_d ? "3d" : "2d");
  if (three_d) j.set("vr", vr);
  j.set("plugins", "");
  return write_text(dir / "project.json", j.dump(), error);
}

std::string readme(const std::string& title, const std::string& what, const std::string& play,
                   const std::string& next) {
  return "# " + title + "\n\n" + what + "\n\n## Play\n\n" + play + "\n\n## Next steps\n\n" + next +
         "\n- No optional plugins are on. Turn them on from the Trading Post (or the Outfitter's "
         "Wagon) when you need them.\n"
         "- Everything in this claim was made by the engine (CC0). Replace it freely.\n";
}

// --- 2D -----------------------------------------------------------------------

constexpr int kStone = 3;  // built-in palette: solid by default

Entity2D camera_2d(std::uint64_t id, std::uint64_t follow) {
  Entity2D cam;
  cam.id = id;
  cam.name = "Camera2D";
  cam.w = 48.0f;
  cam.h = 32.0f;
  cam.color[0] = 0.95f;
  cam.color[1] = 0.75f;
  cam.color[2] = 0.25f;
  cam.color[3] = 1.0f;
  cam.layer = 10;
  Camera2DData c;
  c.target = follow;
  c.smoothing = follow ? 0.15f : 0.0f;
  cam.camera = c;
  return cam;
}

Entity2D tilemap_2d(std::uint64_t id, int cols, int rows) {
  Entity2D t;
  t.id = id;
  t.name = "TileMap";
  t.color[0] = 0.45f;
  t.color[1] = 0.45f;
  t.color[2] = 0.55f;
  t.color[3] = 1.0f;
  t.layer = 0;
  t.tilemap = TileMapData(cols, rows, 32);
  sync_tilemap_extent(t);
  return t;
}

Entity2D player_2d(std::uint64_t id, float x, float y, bool platformer) {
  Entity2D p;
  p.id = id;
  p.name = "Player";
  p.x = x;
  p.y = y;
  p.w = 24.0f;
  p.h = 40.0f;
  p.color[0] = 0.86f;
  p.color[1] = 0.55f;
  p.color[2] = 0.28f;
  p.color[3] = 1.0f;
  p.layer = 5;
  PlayerControllerData pc;
  pc.speed = platformer ? 200.0f : 160.0f;
  if (platformer) pc.mode = PlayerControllerData::Mode::Platformer;
  p.player = pc;
  p.collider = default_collider(p);
  ScriptData s;
  s.path = "scripts/player.lua";
  p.script = s;
  return p;
}

bool save_2d(const fs::path& dir, scene_json::SceneDoc doc, std::string* error) {
  doc.lighting = fx::Lighting2D{};
  doc.lighting.enabled = false;  // the default 2D lighting stays off
  doc.show_grid = true;
  doc.grid_size = 32.0f;
  doc.snap = true;
  std::string err;
  if (!scene_json::save_file(scene_json::scene_path_for_project(dir.string()), doc, &err)) {
    if (error) *error = "scene.json: " + err;
    return false;
  }
  return true;
}

const char* kPlayer2DLua = R"(-- player.lua: the starter script on the Player.
-- Hooks: on_start(), on_tick(dt), on_trigger_enter(other),
-- on_trigger_exit(other), on_interact(rider), on_reload().
-- Movement comes from the Player Controller component (see the Inspector);
-- this script is where your game rules go.

props = { greeting = "Ready to ride.", fall_limit = 2000 }

local start_x, start_y = 0, 0

function on_start()
  start_x, start_y = self:pos()
  log(self:name() .. ": " .. props.greeting)
end

function on_tick(dt)
  -- Runs 60 times a second. Example: fell off the map? Back to the start.
  local x, y = self:pos()
  if y > start_y + props.fall_limit then self:set_pos(start_x, start_y) end
end
)";

const char* kPlayer3DLua = R"(-- player.lua: the starter script on the player.
-- Hooks: on_start(), on_tick(dt), on_trigger_enter(other),
-- on_trigger_exit(other), on_interact(rider), on_reload().
-- 3D adds pos3 / set_pos3 / move3 / yaw / set_yaw on entities.
-- Movement and the camera come from the Player component (see the
-- Inspector); this script is where your game rules go.

props = { greeting = "Ready to ride." }

function on_start()
  log(self:name() .. ": " .. props.greeting)
end

function on_tick(dt)
  -- Runs 60 times a second.
end
)";

const char* kRigLua = R"(-- player.lua: the starter script on the VR Rig.
-- Hooks: on_start(), on_tick(dt), on_grab(hand), on_release(hand, speed),
-- on_point / on_unpoint / on_vr_trigger on the things you point at.
-- The `vr` table (vr.active, vr.head, vr.hand, vr.teleport, vr.haptic)
-- reaches the headset; on the desktop the rig rides as a regular rider.

props = { greeting = "Ready to ride." }

function on_start()
  log(self:name() .. ": " .. props.greeting)
end

function on_tick(dt)
  -- Runs 60 times a second.
end
)";

bool write_2d(const std::string& id, const fs::path& dir, const std::string& name,
              std::string* error) {
  scene_json::SceneDoc doc;
  std::string title, what, play, next;
  if (id == "empty-2d") {
    doc.entities.push_back(camera_2d(1, 0));
    doc.entities.push_back(tilemap_2d(2, 32, 18));
    title = "Empty 2D";
    what = "A 2D claim with a camera (Camera2D), one empty tilemap layer (TileMap, 32 x 18 "
           "tiles of 32 px) and the 2D lighting off. Nothing else.";
    play = "There is no player yet, so Play shows the camera's view of the empty map.";
    next = "- Paint tiles on the TileMap (Tiles panel) or set a tileset image.\n"
           "- Add an entity with a Player Controller and point the Camera2D's Follow at it.\n"
           "- Turn lighting on in Scene settings when you want lights and day / night.";
  } else if (id == "empty-2d-topdown") {
    doc.entities.push_back(camera_2d(1, 3));
    doc.entities.push_back(tilemap_2d(2, 32, 18));
    doc.entities.push_back(player_2d(3, 496.0f, 268.0f, false));
    doc.selected_id = 3;
    doc.selection = {3};
    title = "Empty 2D Top-down";
    what = "A top-down 2D claim: a Player with top-down movement (Player Controller, slot P1) "
           "and a collider, a follow camera (Camera2D) and an empty tilemap layer.";
    play = "WASD / arrow keys / the left stick move the player on both axes. Solid tiles and "
           "static colliders block it.";
    next = "- Paint walls with solid tiles (Stone, Brick, Water...) on the TileMap.\n"
           "- Give the Player a sprite and an animator (idle_/walk_ clips per direction).\n"
           "- Write your rules in scripts/player.lua.";
  } else {  // empty-2d-platformer
    Entity2D map = tilemap_2d(2, 40, 12);
    for (int col = 0; col < map.tilemap->cols; ++col) {
      map.tilemap->set(col, 10, kStone);
      map.tilemap->set(col, 11, kStone);
    }
    doc.entities.push_back(camera_2d(1, 3));
    doc.entities.push_back(map);
    doc.entities.push_back(player_2d(3, 96.0f, 240.0f, true));
    doc.selected_id = 3;
    doc.selection = {3};
    title = "Empty 2D Platformer";
    what = "A side-on 2D claim: a Player with platformer movement (gravity 1400 px/s2, jump "
           "560 px/s), two rows of solid ground tiles (Stone) and a follow camera.";
    play = "A / D, the arrows or the left stick walk; W / Up / Space (action) jump while "
           "standing on something solid.";
    next = "- Paint platforms with any solid tile; tune Gravity and Jump Speed in the "
           "Player Controller.\n"
           "- Give the Player a sprite and an animator.\n"
           "- Write your rules in scripts/player.lua.";
  }
  if (!write_project(dir, name, false, false, error)) return false;
  if (!save_2d(dir, doc, error)) return false;
  bool has_player = false;
  for (const Entity2D& e : doc.entities) has_player = has_player || e.player.has_value();
  if (has_player && !write_text(dir / "scripts" / "player.lua", kPlayer2DLua, error)) return false;
  std::error_code ec;
  fs::create_directories(dir / "assets", ec);
  return write_text(dir / "README.md", readme(title, what, play, next), error);
}

// --- 3D -----------------------------------------------------------------------

using scene3d::Entity3D;

Entity3D ground_3d(std::uint64_t id) {
  Entity3D g;
  g.id = id;
  g.name = "Ground";
  g.transform.position = {0.0f, -0.25f, 0.0f};
  g.transform.scale = {40.0f, 0.5f, 40.0f};
  g.mesh = scene3d::MeshRef{"primitive:cube", true};
  scene3d::Material3D m;
  m.albedo[0] = 0.52f;
  m.albedo[1] = 0.47f;
  m.albedo[2] = 0.40f;
  m.roughness = 0.95f;
  g.material = m;
  scene3d::Collider3D c;
  c.shape = scene3d::Collider3D::Shape::Box;
  c.size = {1.0f, 1.0f, 1.0f};
  g.collider = c;
  return g;
}

Entity3D sun_3d(std::uint64_t id) {
  Entity3D s;
  s.id = id;
  s.name = "Sun";
  s.transform.position = {0.0f, 10.0f, 0.0f};
  s.transform.rotation = Quat::from_euler_deg({-55.0f, 30.0f, 0.0f});
  scene3d::Light3D l;
  l.type = scene3d::Light3D::Type::Directional;
  l.color[0] = 1.0f;
  l.color[1] = 0.92f;
  l.color[2] = 0.80f;
  l.intensity = 2.6f;
  s.light = l;
  return s;
}

Entity3D camera_3d(std::uint64_t id, Vec3 pos, float pitch_deg, float yaw_deg) {
  Entity3D c;
  c.id = id;
  c.name = "Camera";
  c.transform.position = pos;
  c.transform.rotation = Quat::from_euler_deg({pitch_deg, yaw_deg, 0.0f});
  c.camera = scene3d::PlayCamera3D{};
  return c;
}

// A clear daytime sky (the environment's sky gradient) over the dusk default.
void daylight(scene3d::Scene3D& s) {
  render3d::Environment& env = s.environment;
  env.sky_top[0] = 0.28f; env.sky_top[1] = 0.45f; env.sky_top[2] = 0.72f;
  env.sky_horizon[0] = 0.82f; env.sky_horizon[1] = 0.80f; env.sky_horizon[2] = 0.74f;
  env.ground[0] = 0.30f; env.ground[1] = 0.27f; env.ground[2] = 0.22f;
  env.ambient = 0.4f;
}

Entity3D player_3d(std::uint64_t id, bool first_person) {
  Entity3D p;
  p.id = id;
  p.name = "Player";
  p.transform.position = {0.0f, 1.0f, 3.0f};
  if (!first_person) {
    p.mesh = scene3d::MeshRef{"primitive:capsule", true};
    scene3d::Material3D m;
    m.albedo[0] = 0.86f;
    m.albedo[1] = 0.55f;
    m.albedo[2] = 0.28f;
    m.roughness = 0.6f;
    p.material = m;
  }
  scene3d::Collider3D c;
  c.shape = scene3d::Collider3D::Shape::Capsule;
  c.size = {0.4f, 2.0f, 0.4f};
  p.collider = c;
  scene3d::Player3D pl;
  if (first_person) {
    pl.view = scene3d::Player3D::View::FirstPerson;
    pl.eye_height = 1.6f;
  } else {
    pl.camera_distance = 5.0f;
    pl.camera_height = 2.0f;
  }
  p.player = pl;
  ScriptData s;
  s.path = "scripts/player.lua";
  p.script = s;
  return p;
}

bool write_3d(const std::string& id, const fs::path& dir, const std::string& name,
              std::string* error) {
  scene3d::Scene3D s;
  daylight(s);
  std::string title, what, play, next;
  bool vr = false;
  const char* script = kPlayer3DLua;
  if (id == "empty-3d") {
    s.entities.push_back(camera_3d(1, {0.0f, 3.0f, 8.0f}, -14.0f, 0.0f));
    s.entities.push_back(sun_3d(2));
    s.entities.push_back(ground_3d(3));
    script = nullptr;
    title = "Empty 3D";
    what = "A 3D claim with a camera (Camera component), a sun (directional light), a clear "
           "sky (Scene environment) and a 40 x 40 m ground plane. No props.";
    play = "There is no player yet, so Play looks through the Camera.";
    next = "- Create > Primitive or drop glTF models from the asset browser.\n"
           "- Add a Player component to an entity to ride it (first or third person).\n"
           "- Tune the sky, fog and sun in the Environment panel.";
  } else if (id == "empty-3d-fps") {
    Entity3D player = player_3d(1, true);
    s.entities.push_back(player);
    Entity3D eye = camera_3d(2, {0.0f, 1.6f, 3.0f}, 0.0f, 0.0f);
    eye.parent = player.id;
    eye.camera->fov = 70.0f;
    s.entities.push_back(eye);
    s.entities.push_back(sun_3d(3));
    s.entities.push_back(ground_3d(4));
    s.selection = {1};
    title = "Empty 3D First-person";
    what = "A first-person 3D claim: an FPS character controller (Player component, View: "
           "First person, capsule collider), its Camera at eye height (a child of the player), a "
           "sun, a sky and a ground plane.";
    play = "WASD move, Space jumps, Q / Z or right-drag turn, right-drag / Page Up / Page Down "
           "look up and down, E interacts.";
    next = "- Move the Camera child to change the eye point; its FOV is the view's FOV.\n"
           "- Add level geometry with Create > Primitive or glTF models.\n"
           "- Write your rules in scripts/player.lua.";
  } else if (id == "empty-3d-tps") {
    s.entities.push_back(player_3d(1, false));
    s.entities.push_back(sun_3d(2));
    s.entities.push_back(ground_3d(3));
    s.selection = {1};
    title = "Empty 3D Third-person";
    what = "A third-person 3D claim: a character controller (Player component, capsule mesh "
           "and collider) with the orbit camera behind it (Cam distance 5 m, Cam height 2 m), a "
           "sun, a sky and a ground plane.";
    play = "WASD move relative to the camera, Space jumps, Q / Z or right-drag orbit the "
           "camera, E interacts.";
    next = "- Swap the capsule for a skinned glTF and add an Animator (idle / walk / run).\n"
           "- Add level geometry with Create > Primitive or glTF models.\n"
           "- Write your rules in scripts/player.lua.";
  } else {  // empty-vr
    vr = true;
    Entity3D rig;
    rig.id = 1;
    rig.name = "VR Rig";
    rig.transform.position = {0.0f, 0.0f, 2.0f};
    scene3d::VrRig3D r;
    r.locomotion = scene3d::VrRig3D::Locomotion::Teleport;
    r.snap_turn_deg = 45.0f;
    r.hand_model = "assets/hand.gltf";
    rig.vr_rig = r;
    ScriptData sc;
    sc.path = "scripts/player.lua";
    rig.script = sc;
    s.entities.push_back(rig);
    s.entities.push_back(sun_3d(2));
    s.entities.push_back(ground_3d(3));
    s.selection = {1};
    s.editor_camera.target = {0.0f, 1.0f, 2.0f};
    s.editor_camera.distance = 7.0f;
    script = kRigLua;
    title = "Empty VR";
    what = "A 3D claim with VR on: an XR rig (VR Rig component) with skinned hands "
           "(assets/hand.gltf), teleport locomotion and 45 degree snap turn, a sun, a sky and a "
           "ground plane.";
    play = "In a headset: push the right stick forward to aim the teleport arc and release to "
           "go; flick it left / right to snap turn; squeeze to grab; the menu button opens the "
           "comfort menu.\n\nDesktop fallback: with no headset (or ts_game --desktop) the rig "
           "rides as a regular third-person rider (WASD, Space, Q / Z), and ts_game --sim drives "
           "the rig from the desktop simulator.";
    next = "- Add Grabbable to props to pick them up and throw them.\n"
           "- Switch locomotion to Teleport + smooth or tune snap turn on the VR Rig.\n"
           "- Write your rules in scripts/player.lua.";
    if (!render3d::write_sample_hand_gltf((dir / "assets" / "hand.gltf").string(), error)) {
      return false;
    }
  }
  if (!write_project(dir, name, true, vr, error)) return false;
  std::string err;
  if (!scene3d::save_file(scene3d::scene_path(dir.string()), s, &err)) {
    if (error) *error = "scene.json: " + err;
    return false;
  }
  if (script && !write_text(dir / "scripts" / "player.lua", script, error)) return false;
  std::error_code ec;
  fs::create_directories(dir / "assets", ec);
  return write_text(dir / "README.md", readme(title, what, play, next), error);
}

bool write_truly_blank(bool three_d, const fs::path& dir, const std::string& name,
                       std::string* error) {
  if (!write_project(dir, name, three_d, false, error)) return false;
  std::string err;
  if (three_d) {
    scene3d::Scene3D s;  // no entities: the default sky, nothing else
    if (!scene3d::save_file(scene3d::scene_path(dir.string()), s, &err)) {
      if (error) *error = "scene.json: " + err;
      return false;
    }
  } else {
    scene_json::SceneDoc doc;
    if (!save_2d(dir, doc, error)) return false;
  }
  std::error_code ec;
  fs::create_directories(dir / "assets", ec);
  return write_text(
      dir / "README.md",
      readme(std::string("Truly Blank (") + (three_d ? "3D" : "2D") + ")",
             std::string("A ") + (three_d ? "3D" : "2D") +
                 " claim with no entities at all: an empty scene.json, an empty assets/ "
                 "folder and this README.",
             "Play runs an empty world (nothing moves until you add something).",
             three_d ? "- Create > Primitive, Light, Terrain or a glTF model to start.\n"
                       "- Add a Player component to ride something."
                     : "- Create a TileMap, a Camera2D and an entity with a Player Controller.\n"
                       "- Or start again from Empty 2D / Top-down / Platformer."),
      error);
}

}  // namespace

const std::vector<std::string>& ids() {
  static const std::vector<std::string> kIds = {
      "empty-2d", "empty-2d-topdown", "empty-2d-platformer", "empty-3d", "empty-3d-fps",
      "empty-3d-tps", "empty-vr", "truly-blank-2d", "truly-blank-3d"};
  return kIds;
}

bool handles(const std::string& id) {
  for (const std::string& i : ids()) {
    if (i == id) return true;
  }
  return false;
}

bool write(const std::string& id, const std::string& dir_text, const std::string& name,
           std::string* error) {
  const fs::path dir(dir_text);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (id == "empty-2d" || id == "empty-2d-topdown" || id == "empty-2d-platformer") {
    return write_2d(id, dir, name, error);
  }
  if (id == "empty-3d" || id == "empty-3d-fps" || id == "empty-3d-tps" || id == "empty-vr") {
    return write_3d(id, dir, name, error);
  }
  if (id == "truly-blank-2d" || id == "truly-blank-3d") {
    return write_truly_blank(id == "truly-blank-3d", dir, name, error);
  }
  if (error) *error = "unknown empty template '" + id + "'";
  return false;
}

}  // namespace empty_templates
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
