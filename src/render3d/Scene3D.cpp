#include "render3d/Scene3D.h"

#include "core/JsonMini.h"
#include "render3d/Gltf.h"
#include "render3d/Json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace scene3d {

namespace fs = std::filesystem;
using json::Value;

namespace {

Value vec3(const Vec3& v) {
  const float f[3] = {v.x, v.y, v.z};
  return json::floats(f, 3);
}
Value quat(const Quat& q) {
  const float f[4] = {q.x, q.y, q.z, q.w};
  return json::floats(f, 4);
}
void read_vec3(const Value& o, const std::string& key, Vec3* v) {
  float f[3];
  if (o.floats(key, f, 3)) *v = {f[0], f[1], f[2]};
}
Value script_value(const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Bool: return Value::boolean(v.flag);
    case ScriptValue::Type::Text: return Value::string(v.text);
    case ScriptValue::Type::Number:
    default: return Value::number(v.number);
  }
}

Value entity_json(const Entity3D& e) {
  Value o = Value::object();
  o.set("id", Value::number(static_cast<double>(e.id)));
  o.set("name", Value::string(e.name));
  o.set("position", vec3(e.transform.position));
  o.set("rotation", quat(e.transform.rotation));
  o.set("scale", vec3(e.transform.scale));
  if (!e.visible) o.set("visible", Value::boolean(false));
  if (e.mesh) {
    Value m = Value::object();
    m.set("source", Value::string(e.mesh->source));
    if (!e.mesh->cast_shadows) m.set("cast_shadows", Value::boolean(false));
    o.set("mesh", std::move(m));
  }
  if (e.material) {
    const Material3D& mt = *e.material;
    Value m = Value::object();
    m.set("albedo", json::floats(mt.albedo, 4));
    m.set("metallic", Value::number(mt.metallic));
    m.set("roughness", Value::number(mt.roughness));
    if (mt.emissive[0] != 0 || mt.emissive[1] != 0 || mt.emissive[2] != 0) {
      m.set("emissive", json::floats(mt.emissive, 3));
    }
    if (!mt.albedo_texture.empty()) m.set("albedo_texture", Value::string(mt.albedo_texture));
    if (!mt.normal_texture.empty()) m.set("normal_texture", Value::string(mt.normal_texture));
    if (!mt.metal_rough_texture.empty()) {
      m.set("metal_rough_texture", Value::string(mt.metal_rough_texture));
    }
    if (mt.override_model) m.set("override_model", Value::boolean(true));
    o.set("material", std::move(m));
  }
  if (e.light) {
    Value l = Value::object();
    l.set("type", Value::string(e.light->type == Light3D::Type::Directional ? "directional" : "point"));
    l.set("color", json::floats(e.light->color, 3));
    l.set("intensity", Value::number(e.light->intensity));
    l.set("range", Value::number(e.light->range));
    o.set("light", std::move(l));
  }
  if (e.collider) {
    Value c = Value::object();
    c.set("shape", Value::string(e.collider->shape == Collider3D::Shape::Capsule ? "capsule" : "box"));
    c.set("size", vec3(e.collider->size));
    c.set("offset", vec3(e.collider->offset));
    if (e.collider->trigger) c.set("trigger", Value::boolean(true));
    o.set("collider", std::move(c));
  }
  if (e.player) {
    Value p = Value::object();
    p.set("speed", Value::number(e.player->speed));
    p.set("jump_speed", Value::number(e.player->jump_speed));
    p.set("camera_distance", Value::number(e.player->camera_distance));
    p.set("camera_height", Value::number(e.player->camera_height));
    o.set("player", std::move(p));
  }
  if (e.script) {
    Value s = Value::object();
    s.set("path", Value::string(e.script->path));
    if (!e.script->props.empty()) {
      Value props = Value::object();
      for (const ScriptProp& p : e.script->props) props.set(p.name, script_value(p.value));
      s.set("props", std::move(props));
    }
    o.set("script", std::move(s));
  }
  return o;
}

bool entity_from(const Value& o, Entity3D* out, std::string* why) {
  if (!o.is_object()) {
    *why = "entity is not an object";
    return false;
  }
  Entity3D e;
  const double id = o.num("id", 0);
  e.id = id > 0 ? static_cast<std::uint64_t>(id + 0.5) : 0;
  e.name = o.str("name");
  if (e.id == 0 || e.name.empty()) {
    *why = "entity missing id/name";
    return false;
  }
  read_vec3(o, "position", &e.transform.position);
  float q[4];
  if (o.floats("rotation", q, 4)) {
    e.transform.rotation = render3d::normalize(Quat{q[0], q[1], q[2], q[3]});
  }
  read_vec3(o, "scale", &e.transform.scale);
  e.visible = o.flag("visible", true);
  if (o["mesh"].is_object()) {
    MeshRef m;
    m.source = o["mesh"].str("source", m.source);
    m.cast_shadows = o["mesh"].flag("cast_shadows", true);
    e.mesh = m;
  }
  if (o["material"].is_object()) {
    const Value& v = o["material"];
    Material3D m;
    v.floats("albedo", m.albedo, 4);
    m.metallic = std::clamp(v.flt("metallic", m.metallic), 0.0f, 1.0f);
    m.roughness = std::clamp(v.flt("roughness", m.roughness), 0.02f, 1.0f);
    v.floats("emissive", m.emissive, 3);
    m.albedo_texture = v.str("albedo_texture");
    m.normal_texture = v.str("normal_texture");
    m.metal_rough_texture = v.str("metal_rough_texture");
    m.override_model = v.flag("override_model", false);
    e.material = m;
  }
  if (o["light"].is_object()) {
    const Value& v = o["light"];
    Light3D l;
    l.type = v.str("type") == "directional" ? Light3D::Type::Directional : Light3D::Type::Point;
    v.floats("color", l.color, 3);
    l.intensity = std::max(0.0f, v.flt("intensity", l.intensity));
    l.range = std::max(0.1f, v.flt("range", l.range));
    e.light = l;
  }
  if (o["collider"].is_object()) {
    const Value& v = o["collider"];
    Collider3D c;
    c.shape = v.str("shape") == "capsule" ? Collider3D::Shape::Capsule : Collider3D::Shape::Box;
    read_vec3(v, "size", &c.size);
    read_vec3(v, "offset", &c.offset);
    c.trigger = v.flag("trigger", false);
    e.collider = c;
  }
  if (o["player"].is_object()) {
    const Value& v = o["player"];
    Player3D p;
    p.speed = std::clamp(v.flt("speed", p.speed), 0.0f, 100.0f);
    p.jump_speed = std::clamp(v.flt("jump_speed", p.jump_speed), 0.0f, 100.0f);
    p.camera_distance = std::clamp(v.flt("camera_distance", p.camera_distance), 1.0f, 100.0f);
    p.camera_height = std::clamp(v.flt("camera_height", p.camera_height), -20.0f, 50.0f);
    e.player = p;
  }
  if (o["script"].is_object()) {
    const Value& v = o["script"];
    ScriptData s;
    s.path = v.str("path");
    for (const auto& [k, pv] : v["props"].members()) {
      if (pv.is_bool()) s.set(k, ScriptValue::of_bool(pv.as_bool()));
      else if (pv.is_number()) s.set(k, ScriptValue::of_number(pv.as_number()));
      else if (pv.is_string()) s.set(k, ScriptValue::of_text(pv.as_string()));
    }
    normalize_script(s);
    e.script = s;
  }
  *out = std::move(e);
  return true;
}

}  // namespace

Entity3D* Scene3D::find(std::uint64_t id) {
  for (Entity3D& e : entities) {
    if (e.id == id) return &e;
  }
  return nullptr;
}
const Entity3D* Scene3D::find(std::uint64_t id) const {
  for (const Entity3D& e : entities) {
    if (e.id == id) return &e;
  }
  return nullptr;
}
const Entity3D* Scene3D::find_named(const std::string& name) const {
  for (const Entity3D& e : entities) {
    if (e.name == name) return &e;
  }
  return nullptr;
}
std::uint64_t Scene3D::next_id() const {
  std::uint64_t m = 0;
  for (const Entity3D& e : entities) m = std::max(m, e.id);
  return m + 1;
}

std::string scene_path(const std::string& project_dir) {
  return (fs::path(project_dir) / "scene.json").string();
}

bool is_3d_scene_text(const std::string& text) {
  Value v;
  if (!json::parse(text, &v, nullptr)) return false;
  return v.str("dimension") == "3d";
}

std::string write(const Scene3D& s) {
  Value root = Value::object();
  root.set("version", Value::number(kScene3DVersion));
  root.set("dimension", Value::string("3d"));
  const render3d::Environment& env = s.environment;
  Value e = Value::object();
  e.set("sky_top", json::floats(env.sky_top, 3));
  e.set("sky_horizon", json::floats(env.sky_horizon, 3));
  e.set("ground", json::floats(env.ground, 3));
  e.set("ambient", Value::number(env.ambient));
  e.set("sun_direction", vec3(env.sun_direction));
  e.set("sun_color", json::floats(env.sun_color, 3));
  e.set("sun_intensity", Value::number(env.sun_intensity));
  e.set("shadows", Value::boolean(env.shadows));
  e.set("shadow_distance", Value::number(env.shadow_distance));
  root.set("environment", std::move(e));
  Value ents = Value::array();
  for (const Entity3D& en : s.entities) ents.push(entity_json(en));
  root.set("entities", std::move(ents));
  Value ed = Value::object();
  ed.set("camera_target", vec3(s.editor_camera.target));
  ed.set("camera_yaw", Value::number(s.editor_camera.yaw));
  ed.set("camera_pitch", Value::number(s.editor_camera.pitch));
  ed.set("camera_distance", Value::number(s.editor_camera.distance));
  ed.set("show_grid", Value::boolean(s.show_grid));
  ed.set("snap", Value::boolean(s.snap));
  ed.set("snap_translate", Value::number(s.snap_translate));
  ed.set("snap_rotate", Value::number(s.snap_rotate));
  ed.set("snap_scale", Value::number(s.snap_scale));
  Value sel = Value::array();
  for (std::uint64_t id : s.selection) sel.push(Value::number(static_cast<double>(id)));
  ed.set("selection", std::move(sel));
  root.set("editor", std::move(ed));
  return json::write(root);
}

bool parse(const std::string& text, Scene3D* out, std::string* error) {
  Value root;
  std::string err;
  if (!json::parse(text, &root, &err)) {
    if (error) *error = "Invalid scene.json: " + err;
    return false;
  }
  if (!root.is_object()) {
    if (error) *error = "Invalid scene.json: expected an object";
    return false;
  }
  if (root.str("dimension") != "3d") {
    if (error) {
      *error = "scene.json is a 2D scene (no \"dimension\": \"3d\"); open it in a 2D claim";
    }
    return false;
  }
  Scene3D s;
  s.version = static_cast<int>(root.num("version", kScene3DVersion));
  if (s.version > kScene3DVersion) {
    if (error) {
      *error = "scene.json v" + std::to_string(s.version) +
               " is newer than this build reads (v" + std::to_string(kScene3DVersion) + ")";
    }
    return false;
  }
  const Value& env = root["environment"];
  if (env.is_object()) {
    render3d::Environment& e = s.environment;
    env.floats("sky_top", e.sky_top, 3);
    env.floats("sky_horizon", e.sky_horizon, 3);
    env.floats("ground", e.ground, 3);
    e.ambient = std::clamp(env.flt("ambient", e.ambient), 0.0f, 4.0f);
    read_vec3(env, "sun_direction", &e.sun_direction);
    if (render3d::length(e.sun_direction) < 1e-4f) e.sun_direction = {0, -1, 0};
    env.floats("sun_color", e.sun_color, 3);
    e.sun_intensity = std::max(0.0f, env.flt("sun_intensity", e.sun_intensity));
    e.shadows = env.flag("shadows", e.shadows);
    e.shadow_distance = std::clamp(env.flt("shadow_distance", e.shadow_distance), 2.0f, 500.0f);
  }
  for (const Value& v : root["entities"].items()) {
    Entity3D en;
    std::string why;
    if (!entity_from(v, &en, &why)) {
      if (error) *error = "Invalid scene.json: " + why;
      return false;
    }
    if (s.find(en.id)) {
      if (error) *error = "Invalid scene.json: duplicate entity id " + std::to_string(en.id);
      return false;
    }
    s.entities.push_back(std::move(en));
  }
  const Value& ed = root["editor"];
  if (ed.is_object()) {
    read_vec3(ed, "camera_target", &s.editor_camera.target);
    s.editor_camera.yaw = ed.flt("camera_yaw", s.editor_camera.yaw);
    s.editor_camera.pitch = std::clamp(ed.flt("camera_pitch", s.editor_camera.pitch), -89.0f, 89.0f);
    s.editor_camera.distance = std::clamp(ed.flt("camera_distance", s.editor_camera.distance), 0.5f, 500.0f);
    s.show_grid = ed.flag("show_grid", s.show_grid);
    s.snap = ed.flag("snap", s.snap);
    s.snap_translate = std::max(0.01f, ed.flt("snap_translate", s.snap_translate));
    s.snap_rotate = std::max(1.0f, ed.flt("snap_rotate", s.snap_rotate));
    s.snap_scale = std::max(0.01f, ed.flt("snap_scale", s.snap_scale));
    for (const Value& id : ed["selection"].items()) {
      const double d = id.as_number(0);
      if (d > 0 && s.find(static_cast<std::uint64_t>(d + 0.5))) {
        s.selection.push_back(static_cast<std::uint64_t>(d + 0.5));
      }
    }
  }
  s.version = kScene3DVersion;
  *out = std::move(s);
  return true;
}

bool load_file(const std::string& path, Scene3D* out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "Could not open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse(ss.str(), out, error);
}

bool save_file(const std::string& path, const Scene3D& scene,
               std::string* error) {
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "Could not write " + tmp;
      return false;
    }
    out << write(scene);
    if (!out) {
      if (error) *error = "Failed while writing " + tmp;
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    // Windows will not rename over an open / existing file on some setups.
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
      if (error) *error = "Could not replace " + path + ": " + ec.message();
      return false;
    }
  }
  return true;
}

namespace {

Entity3D prim(std::uint64_t id, const std::string& name, const std::string& shape,
              Vec3 pos, Vec3 scale, const float rgb[3], float rough = 0.8f,
              float metal = 0.0f) {
  Entity3D e;
  e.id = id;
  e.name = name;
  e.transform.position = pos;
  e.transform.scale = scale;
  e.mesh = MeshRef{"primitive:" + shape, true};
  Material3D m;
  m.albedo[0] = rgb[0];
  m.albedo[1] = rgb[1];
  m.albedo[2] = rgb[2];
  m.roughness = rough;
  m.metallic = metal;
  e.material = m;
  if (shape != "plane") {
    Collider3D c;
    c.shape = Collider3D::Shape::Box;
    c.size = {1, 1, 1};
    e.collider = c;
  }
  return e;
}

Entity3D rider(std::uint64_t id, Vec3 pos) {
  Entity3D r;
  r.id = id;
  r.name = "Rider";
  r.transform.position = pos;
  r.mesh = MeshRef{"primitive:capsule", true};
  Material3D m;
  m.albedo[0] = 0.62f;
  m.albedo[1] = 0.27f;
  m.albedo[2] = 0.18f;
  m.roughness = 0.6f;
  r.material = m;
  Collider3D c;
  c.shape = Collider3D::Shape::Capsule;
  c.size = {0.4f, 2.0f, 0.4f};
  r.collider = c;
  r.player = Player3D{};
  return r;
}

Entity3D ground(std::uint64_t id, float size, const std::string& texture) {
  const float sand[3] = {0.71f, 0.58f, 0.42f};
  Entity3D g = prim(id, "Ground", "cube", {0, -0.25f, 0}, {size, 0.5f, size}, sand, 0.95f);
  g.material->albedo_texture = texture;
  return g;
}

}  // namespace

Scene3D make_starter_scene() {
  Scene3D s;
  s.entities.push_back(ground(1, 30.0f, ""));
  const float wood[3] = {0.55f, 0.36f, 0.2f};
  s.entities.push_back(prim(2, "Crate", "cube", {2.0f, 0.5f, -1.0f}, {1, 1, 1}, wood, 0.7f));
  Entity3D lamp;
  lamp.id = 3;
  lamp.name = "Lamp";
  lamp.transform.position = {-2.0f, 2.5f, 1.0f};
  lamp.light = Light3D{};
  s.entities.push_back(lamp);
  s.entities.push_back(rider(4, {0.0f, 1.0f, 3.0f}));
  return s;
}

namespace {

bool write_text(const fs::path& p, const std::string& text, std::string* error) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "Could not write " + p.string();
    return false;
  }
  out << text;
  return static_cast<bool>(out);
}

// Uncompressed 32-bit TGA, top-left origin (stb_image reads it).
bool write_tga(const fs::path& p, const render3d::Image& img, std::string* error) {
  std::string bytes(18, '\0');
  bytes[2] = 2;
  bytes[12] = static_cast<char>(img.width & 0xFF);
  bytes[13] = static_cast<char>((img.width >> 8) & 0xFF);
  bytes[14] = static_cast<char>(img.height & 0xFF);
  bytes[15] = static_cast<char>((img.height >> 8) & 0xFF);
  bytes[16] = 32;
  bytes[17] = 0x28;  // top-left, 8 alpha bits
  for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
    bytes.push_back(static_cast<char>(img.rgba[i + 2]));
    bytes.push_back(static_cast<char>(img.rgba[i + 1]));
    bytes.push_back(static_cast<char>(img.rgba[i + 0]));
    bytes.push_back(static_cast<char>(img.rgba[i + 3]));
  }
  return write_text(p, bytes, error);
}

constexpr const char* kWindmillLua = R"lua(-- Windmill: turns its sails on the 60 Hz tick. 3D entities have the
-- usual handle methods (name, get / set, show / hide, toast...) plus
-- pos3 / set_pos3 / move3 / yaw / set_yaw / spin(deg, axis) / scale3 /
-- set_scale3.
props = {
  speed = 45,  -- degrees per second
}

function on_start()
  log(self:name() .. " catches the wind")
end

function on_tick(dt)
  self:spin(props.speed * dt, "z")
end
)lua";

constexpr const char* kBellLua = R"lua(-- Church bell: rings when a rider walks under it (trigger collider).
function on_trigger_enter(other)
  toast(other:name() .. " rang the bell", 2)
  self:set("rings", self:get("rings", 0) + 1)
end
)lua";

}  // namespace

bool write_ghost_town_sample(const std::string& project_dir,
                             const std::string& display_name,
                             std::string* error) {
  const fs::path dir(project_dir);
  std::error_code ec;
  fs::create_directories(dir / "assets", ec);
  fs::create_directories(dir / "scripts", ec);

  // Boardwalk texture: warm planks with dark seams.
  render3d::Image boards;
  boards.width = boards.height = 64;
  boards.rgba.resize(64 * 64 * 4);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const bool seam = (y % 16) == 0 || ((x + (y / 16) * 21) % 64) == 0;
      const int grain = ((x * 7 + y * 3) % 11) * 3;
      unsigned char* px = &boards.rgba[(static_cast<std::size_t>(y) * 64 + x) * 4];
      px[0] = static_cast<unsigned char>(seam ? 70 : 170 + grain);
      px[1] = static_cast<unsigned char>(seam ? 48 : 128 + grain);
      px[2] = static_cast<unsigned char>(seam ? 30 : 88 + grain / 2);
      px[3] = 255;
    }
  }
  if (!write_tga(dir / "assets" / "boards.tga", boards, error)) return false;
  if (!render3d::write_sample_lantern_gltf((dir / "assets" / "lantern.gltf").string(), error)) {
    return false;
  }
  if (!write_text(dir / "scripts" / "windmill.lua", kWindmillLua, error)) return false;
  if (!write_text(dir / "scripts" / "bell.lua", kBellLua, error)) return false;

  Scene3D s;
  s.environment.sky_top[0] = 0.13f;
  s.environment.sky_top[1] = 0.16f;
  s.environment.sky_top[2] = 0.30f;
  std::uint64_t id = 1;
  s.entities.push_back(ground(id++, 40.0f, ""));
  const float wood[3] = {0.47f, 0.31f, 0.19f};
  const float grey[3] = {0.42f, 0.38f, 0.34f};
  const float paint[3] = {0.62f, 0.22f, 0.16f};
  const float iron[3] = {0.55f, 0.55f, 0.58f};
  const float white[3] = {0.86f, 0.82f, 0.74f};
  // Boardwalk down main street (textured).
  Entity3D walk = prim(id++, "Boardwalk", "cube", {0, 0.05f, 0}, {4.0f, 0.1f, 24.0f}, white, 0.9f);
  walk.material->albedo_texture = "assets/boards.tga";
  walk.collider.reset();
  s.entities.push_back(walk);
  // Saloon, bank, jail: big boxes with false fronts.
  struct B { const char* name; float x, z, w, h, d; const float* col; };
  const B bs[] = {{"Saloon", -6.0f, -4.0f, 5.0f, 4.0f, 6.0f, wood},
                  {"Bank", -6.0f, 5.0f, 5.0f, 3.5f, 5.0f, grey},
                  {"Jail", 6.0f, -5.0f, 4.5f, 3.0f, 5.0f, grey},
                  {"General Store", 6.0f, 4.0f, 5.0f, 3.6f, 6.0f, paint}};
  for (const B& b : bs) {
    s.entities.push_back(prim(id++, b.name, "cube", {b.x, b.h * 0.5f, b.z}, {b.w, b.h, b.d}, b.col, 0.85f));
  }
  // Water tower: sphere tank on a post.
  s.entities.push_back(prim(id++, "Tower Post", "cube", {10.0f, 2.0f, 10.0f}, {0.6f, 4.0f, 0.6f}, wood));
  s.entities.push_back(prim(id++, "Water Tank", "sphere", {10.0f, 5.0f, 10.0f}, {3, 3, 3}, iron, 0.35f, 0.8f));
  // Windmill with spinning sails (script).
  s.entities.push_back(prim(id++, "Windmill Tower", "cube", {-11.0f, 3.0f, 10.0f}, {1.0f, 6.0f, 1.0f}, grey));
  Entity3D sails = prim(id++, "Windmill Sails", "cube", {-11.0f, 6.2f, 10.6f}, {4.0f, 0.4f, 0.1f}, white, 0.6f);
  sails.collider.reset();
  sails.script = ScriptData{};
  sails.script->path = "scripts/windmill.lua";
  sails.script->set("speed", ScriptValue::of_number(90));
  sails.transform.rotation = Quat::axis_angle({0, 0, 1}, 0.0f);
  s.entities.push_back(sails);
  // Tumbleweed-ish boulders.
  s.entities.push_back(prim(id++, "Boulder", "sphere", {3.5f, 0.6f, 9.0f}, {1.2f, 1.2f, 1.2f}, grey, 0.95f));
  // Bell (trigger) under the church arch.
  Entity3D bell = prim(id++, "Bell", "sphere", {0.0f, 3.2f, -10.0f}, {0.8f, 0.8f, 0.8f}, iron, 0.3f, 0.9f);
  bell.collider->trigger = true;
  bell.collider->size = {3.0f, 8.0f, 3.0f};
  bell.collider->offset = {0.0f, -2.5f, 0.0f};
  bell.script = ScriptData{};
  bell.script->path = "scripts/bell.lua";
  s.entities.push_back(bell);
  // Lanterns from the glTF model, each with a warm point light.
  const float lz[3] = {-8.0f, 0.0f, 8.0f};
  for (int i = 0; i < 3; ++i) {
    Entity3D l;
    l.id = id++;
    l.name = "Lantern " + std::to_string(i + 1);
    l.transform.position = {2.6f, 0.0f, lz[i]};
    l.mesh = MeshRef{"assets/lantern.gltf", true};
    Light3D light;
    light.intensity = 5.0f;
    light.range = 7.0f;
    l.light = light;
    Collider3D c;
    c.size = {0.4f, 2.6f, 0.4f};
    c.offset = {0.0f, 1.3f, 0.0f};
    l.collider = c;
    s.entities.push_back(l);
  }
  s.entities.push_back(rider(id++, {0.0f, 1.0f, 6.0f}));
  s.editor_camera.target = {0, 1, 0};
  s.editor_camera.distance = 26.0f;
  if (!save_file(scene_path(project_dir), s, error)) return false;

  const std::string pj = json_mini::write_object({{"name", display_name},
                                                  {"dimension", "3d"},
                                                  {"created", "sample"}});
  return write_text(dir / "project.json", pj, error);
}

}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
