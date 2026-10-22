#include "render3d/ModelImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string ext_of(const std::string& path) {
  return lower(fs::u8path(path).extension().string());
}

bool is_gltf_ext(const std::string& e) { return e == ".gltf" || e == ".glb"; }

// Formats routed to Assimp (importers only; glTF stays on cgltf, .blend
// goes through Blender, .mdl through our Source reader first).
const std::vector<std::string>& assimp_exts() {
  static const std::vector<std::string> v = {
      ".obj", ".fbx", ".dae", ".3ds", ".ply", ".stl", ".x",   ".lwo", ".lws", ".ms3d",
      ".off", ".ase", ".ac",  ".md2", ".md3", ".md5mesh", ".smd", ".b3d", ".irrmesh",
      ".cob", ".xgl", ".zgl", ".nff", ".dxf", ".3mf", ".amf", ".ogex", ".q3o", ".q3s"};
  return v;
}

bool is_assimp_ext(const std::string& e) {
  const auto& v = assimp_exts();
  return std::find(v.begin(), v.end(), e) != v.end();
}

std::vector<std::string> split_paths(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ';' || c == '\n') {
      while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else if (!(cur.empty() && std::isspace(static_cast<unsigned char>(c)))) {
      cur += c;
    }
  }
  while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

const std::vector<std::string>& model_extensions() {
  static const std::vector<std::string> v = [] {
    std::vector<std::string> o = {".gltf", ".glb", ".mdl", ".blend"};
    for (const std::string& e : assimp_exts()) o.push_back(e);
    return o;
  }();
  return v;
}

bool is_model_file(const std::string& path) {
  const std::string e = ext_of(path);
  if (e.empty()) return false;
  const auto& v = model_extensions();
  return std::find(v.begin(), v.end(), e) != v.end();
}

std::string model_format_name(const std::string& path) {
  const std::string e = ext_of(path);
  if (is_gltf_ext(e)) return "glTF";
  if (e == ".mdl") return "Source MDL";
  if (e == ".blend") return "Blender";
  if (e == ".obj") return "Wavefront OBJ";
  if (e == ".dae") return "Collada";
  if (e == ".smd") return "Valve SMD";
  std::string n = e.size() > 1 ? e.substr(1) : e;
  std::transform(n.begin(), n.end(), n.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return n;
}

std::vector<std::string> model_dependencies(const std::string& path) {
  std::vector<std::string> out;
  const fs::path p = fs::u8path(path);
  const std::string e = ext_of(path);
  std::error_code ec;
  auto add = [&](const fs::path& q) {
    if (fs::is_regular_file(q, ec)) out.push_back(q.string());
  };
  if (e == ".mdl") {
    fs::path base = p;
    base.replace_extension();
    for (const char* x : {".vvd", ".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx", ".phy"}) {
      add(fs::u8path(base.string() + x));
    }
  } else if (e == ".obj") {
    // mtllib lines name the material libraries.
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("mtllib", 0) == 0 && line.size() > 7) {
        std::string name = line.substr(7);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        add(p.parent_path() / fs::u8path(name));
      }
    }
    fs::path mtl = p;
    mtl.replace_extension(".mtl");
    if (std::find(out.begin(), out.end(), mtl.string()) == out.end()) add(mtl);
  } else if (e == ".gltf") {
    fs::path bin = p;
    bin.replace_extension(".bin");
    add(bin);
  }
  return out;
}

json::Value default_model_settings(const std::string& path) {
  using json::Value;
  const bool source = ext_of(path) == ".mdl";
  Value s = Value::object();
  s.set("scale", Value::number(1));  // placement scale (entity transform)
  s.set("unit_scale", Value::number(source ? 0.0254 : 1.0));
  s.set("up_axis", Value::string(source ? "z" : "auto"));
  s.set("material_path", Value::string(""));
  s.set("lod", Value::number(0));
  s.set("generate_collider", Value::boolean(true));
  s.set("import_animations", Value::boolean(true));
  if (source) s.set("skin", Value::number(0));
  if (ext_of(path) == ".blend") s.set("blender_path", Value::string(""));
  return s;
}

ModelImportOptions model_options_from_settings(const json::Value& s, const std::string& path) {
  ModelImportOptions o;
  const json::Value d = default_model_settings(path);
  auto num = [&](const char* k) { return s.has(k) ? s.num(k, d.num(k, 0)) : d.num(k, 0); };
  o.unit_scale = static_cast<float>(num("unit_scale"));
  if (!(o.unit_scale > 0.0f) || !std::isfinite(o.unit_scale)) o.unit_scale = 1.0f;
  const std::string up = lower(s.has("up_axis") ? s.str("up_axis") : d.str("up_axis"));
  o.up = up == "z" ? UpAxis::Z : up == "y" ? UpAxis::Y : UpAxis::Auto;
  o.material_paths = split_paths(s.str("material_path"));
  o.lod = std::max(0, static_cast<int>(num("lod")));
  o.animations = s.flag("import_animations", true);
  o.collision = s.flag("generate_collider", true);
  o.skin = std::max(0, static_cast<int>(s.num("skin", 0)));
  const json::Value& bg = s["body_groups"];
  if (bg.is_object()) {
    for (const auto& [k, v] : bg.members()) o.body_groups[k] = static_cast<int>(v.as_number(0));
  }
  o.blender_path = s.str("blender_path");
  return o;
}

bool read_model_options(const std::string& path, ModelImportOptions* out) {
  std::ifstream in(fs::u8path(path + ".meta"), std::ios::binary);
  if (!in) {
    *out = model_options_from_settings(json::Value::object(), path);
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  json::Value root;
  std::string err;
  if (!json::parse(ss.str(), &root, &err) || !root["settings"].is_object()) {
    *out = model_options_from_settings(json::Value::object(), path);
    return false;
  }
  *out = model_options_from_settings(root["settings"], path);
  return true;
}

std::shared_ptr<const Image> missing_texture_image() {
  static const std::shared_ptr<const Image> img = [] {
    auto im = std::make_shared<Image>();
    im->width = im->height = 8;
    im->rgba.resize(8 * 8 * 4);
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        unsigned char* p = &im->rgba[(static_cast<std::size_t>(y) * 8 + x) * 4];
        const bool on = ((x / 4) + (y / 4)) % 2 == 0;
        p[0] = on ? 255 : 0;
        p[1] = 0;
        p[2] = on ? 255 : 0;
        p[3] = 255;
      }
    }
    return std::shared_ptr<const Image>(std::move(im));
  }();
  return img;
}

void update_bounds(Model* m) {
  Aabb b;
  bool any = false;
  for (const ModelPrimitive& p : m->primitives) {
    for (const Vertex3D& v : p.mesh.vertices) {
      if (!any) {
        b.min = b.max = v.position;
        any = true;
      }
      b.min = {std::min(b.min.x, v.position.x), std::min(b.min.y, v.position.y),
               std::min(b.min.z, v.position.z)};
      b.max = {std::max(b.max.x, v.position.x), std::max(b.max.y, v.position.y),
               std::max(b.max.z, v.position.z)};
    }
  }
  m->bounds = any ? b : Aabb{};
}

void bake_import_transform(Model* m, float scale, bool z_up) {
  if (scale == 1.0f && !z_up) return;
  // Z up -> Y up: -90 degrees about X, (x, y, z) -> (x, z, -y).
  const Quat r = z_up ? Quat::axis_angle({1, 0, 0}, -kPi * 0.5f) : Quat{};
  const Mat4 c = Mat4::rotate(r) * Mat4::scale({scale, scale, scale});
  const Mat4 c_inv = c.inverse();
  for (ModelPrimitive& p : m->primitives) {
    for (Vertex3D& v : p.mesh.vertices) {
      v.position = c.transform_point(v.position);
      v.normal = normalize(r.rotate(v.normal));
      const Vec3 t = r.rotate(Vec3{v.tangent.x, v.tangent.y, v.tangent.z});
      v.tangent = {t.x, t.y, t.z, v.tangent.w};
    }
  }
  // Skinned: palette = C * joint * inverse_bind * C^-1 (root joints get C
  // in front through Joint::base; every inverse bind gets C^-1 behind).
  for (Joint& j : m->skeleton.joints) {
    if (j.parent < 0) j.base = c * j.base;
    j.inverse_bind = j.inverse_bind * c_inv;
  }
  for (auto& hull : m->collision_hulls) {
    for (Vec3& p : hull) p = c.transform_point(p);
  }
  update_bounds(m);
}

bool load_model(const std::string& path, Model* out, std::string* error) {
  ModelImportOptions opt;
  read_model_options(path, &opt);
  return load_model(path, opt, out, error);
}

bool load_model(const std::string& path, const ModelImportOptions& opt, Model* out,
                std::string* error) {
  const std::string e = ext_of(path);
  *out = Model{};
  std::string err;
  bool ok = false;
  bool z_up = opt.up == UpAxis::Z;
  if (is_gltf_ext(e)) {
    ok = load_gltf(path, out, &err);
    out->format = "gltf";
  } else if (e == ".mdl") {
    ok = load_source_mdl(path, opt, out, &err);
    if (ok) {
      out->format = "source-mdl";
      if (opt.up == UpAxis::Auto) z_up = true;  // Source is Z up
    } else if (err.rfind("not a Source", 0) == 0 && assimp_available()) {
      // Quake / Half-Life 1 .mdl: Assimp reads those.
      *out = Model{};
      err.clear();
      ok = load_assimp_model(path, opt, out, &err);
      if (ok) out->format = "mdl";
    }
  } else if (e == ".blend") {
    ok = load_blend_model(path, opt, out, &err);
    out->format = "blend";
  } else if (is_assimp_ext(e)) {
    if (!assimp_available()) {
      err = model_format_name(path) +
            " needs the Assimp importers (this build has TS_WITH_ASSIMP off); export to glTF";
    } else {
      // (Auto up axis: the loader applies the file's own axis metadata.)
      ok = load_assimp_model(path, opt, out, &err);
      out->format = e.substr(1);
    }
  } else {
    err = "no model importer for '" + e + "'";
  }
  if (!ok) {
    if (error) *error = err.empty() ? path + ": import failed" : err;
    return false;
  }
  out->source = path;
  if (!opt.animations) out->clips.clear();
  if (!opt.collision) out->collision_hulls.clear();
  bake_import_transform(out, opt.unit_scale, z_up);
  if (out->primitives.empty()) {
    if (error) *error = path + ": no meshes";
    return false;
  }
  if (error) *error = err;
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
