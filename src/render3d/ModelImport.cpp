#include "render3d/ClipFile.h"
#include "render3d/HullFit.h"
#include "render3d/ModelImport.h"

#include "core/AssetMeta.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
  // Collider a placed model gets (render3d/ColliderFit.h): auto / box /
  // capsule / convex / mesh / none.
  s.set("collider", Value::string("auto"));
  s.set("hull_vertices", Value::number(64));
  s.set("decompose_max", Value::number(8));
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
  if (const json::Value& mo = s["material_overrides"]; mo.is_object()) {
    for (const auto& [k, v] : mo.members()) {
      if (v.is_string() && !v.as_string().empty()) o.material_overrides[lower(k)] = v.as_string();
    }
  }
  return o;
}

bool read_model_options(const std::string& path, ModelImportOptions* out) {
  // The claim's metadata folder (.tombstone/meta, core/AssetMeta.h), else a
  // sidecar "<path>.meta" (shipped games, claims from before the folder).
  const std::string meta = core::asset_meta::find_for_file(path);
  std::ifstream in(fs::u8path(meta.empty() ? path + ".meta" : meta), std::ios::binary);
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

void primitive_palette(const ModelPrimitive& p, const std::vector<Mat4>& full,
                       std::vector<Mat4>* out) {
  if (p.joint_map.empty()) {
    *out = full;
    return;
  }
  out->resize(p.joint_map.size());
  for (std::size_t i = 0; i < p.joint_map.size(); ++i) {
    const int j = p.joint_map[i];
    (*out)[i] = (j >= 0 && static_cast<std::size_t>(j) < full.size()) ? full[static_cast<std::size_t>(j)] : Mat4{};
  }
}

namespace {

// Rest-pose model-space positions of one primitive (rigid: as stored).
void posed_positions(const Model& m, const ModelPrimitive& p, const std::vector<Mat4>& rest,
                     std::vector<Vec3>* out) {
  out->resize(p.mesh.vertices.size());
  if (!p.mesh.skinned() || m.skeleton.empty()) {
    for (std::size_t v = 0; v < p.mesh.vertices.size(); ++v) (*out)[v] = p.mesh.vertices[v].position;
    return;
  }
  std::vector<Mat4> pal;
  primitive_palette(p, rest, &pal);
  for (std::size_t v = 0; v < p.mesh.vertices.size(); ++v) {
    (*out)[v] = skin_point(pal, p.mesh.skin[v].joints, p.mesh.skin[v].weights, p.mesh.vertices[v].position);
  }
}

std::vector<Mat4> rest_palette(const Model& m) {
  std::vector<Mat4> rest;
  if (m.skinned()) skin_palette(m.skeleton, rest_pose(m.skeleton), &rest);
  return rest;
}

}  // namespace

void update_bounds(Model* m) {
  Aabb b;
  bool any = false;
  const std::vector<Mat4> rest = rest_palette(*m);
  std::vector<Vec3> pos;
  for (const ModelPrimitive& p : m->primitives) {
    posed_positions(*m, p, rest, &pos);
    // Only vertices a triangle uses (stray / unused vertices, often at the
    // origin or far away in exported files, do not count).
    for (std::uint32_t i : p.mesh.indices) {
      if (i >= pos.size()) continue;
      const Vec3& q = pos[i];
      if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) continue;
      if (!any) {
        b.min = b.max = q;
        any = true;
      }
      b.min = vmin(b.min, q);
      b.max = vmax(b.max, q);
    }
  }
  m->bounds = any ? b : Aabb{};
}

Aabb legacy_vertex_bounds(const Model& m) {
  Aabb b;
  bool any = false;
  for (const ModelPrimitive& p : m.primitives) {
    for (const Vertex3D& v : p.mesh.vertices) {
      if (!any) {
        b.min = b.max = v.position;
        any = true;
      }
      b.min = vmin(b.min, v.position);
      b.max = vmax(b.max, v.position);
    }
  }
  return any ? b : Aabb{};
}

void rest_pose_mesh(const Model& m, MeshData* out) {
  *out = MeshData{};
  const std::vector<Mat4> rest = rest_palette(m);
  std::vector<Vec3> pos;
  for (const ModelPrimitive& p : m.primitives) {
    posed_positions(m, p, rest, &pos);
    const std::uint32_t base = static_cast<std::uint32_t>(out->vertices.size());
    for (std::size_t v = 0; v < pos.size(); ++v) {
      Vertex3D vx = p.mesh.vertices[v];
      vx.position = pos[v];
      out->vertices.push_back(vx);
    }
    for (std::uint32_t i : p.mesh.indices) out->indices.push_back(base + i);
  }
}

namespace {

std::vector<std::string> name_parts(const ModelPrimitive& p) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : p.node) {
    if (c == '/') {
      if (!cur.empty()) parts.push_back(lower(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) parts.push_back(lower(cur));
  if (!p.mesh.name.empty()) parts.push_back(lower(p.mesh.name));
  return parts;
}

bool ends_with(const std::string& s, const std::string& t) {
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

bool collision_name(const std::string& n) {
  for (const char* pre : {"ucx_", "ubx_", "ucp_", "usp_", "umb_"}) {
    if (n.rfind(pre, 0) == 0) return true;
  }
  for (const char* suf : {"-colonly", "_colonly", "_collision", "-collision", ".collision", "_collider"}) {
    if (ends_with(n, suf)) return true;
  }
  return n == "collision" || n == "collisions" || n == "collider" || n == "colliders";
}

// "body_lod1" / "lod2" / "body.lod0" / "body lod3" -> 1 / 2 / 0 / 3; -1 = none.
int lod_level(const std::string& n) {
  const std::size_t at = n.rfind("lod");
  if (at == std::string::npos) return -1;
  if (at > 0) {
    const char c = n[at - 1];
    if (c != '_' && c != '-' && c != '.' && c != ' ' && c != ':') return -1;
  }
  std::size_t i = at + 3;
  if (i < n.size() && (n[i] == '_' || n[i] == ' ')) ++i;
  if (i >= n.size() || !std::isdigit(static_cast<unsigned char>(n[i]))) return -1;
  int v = 0;
  while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i])) && v < 100) v = v * 10 + (n[i++] - '0');
  return i == n.size() ? v : -1;  // digits must end the name
}

}  // namespace

int split_special_meshes(Model* m, int lod, bool keep_hulls) {
  const std::vector<Mat4> rest = rest_palette(*m);
  std::vector<int> level(m->primitives.size(), -1);
  std::vector<bool> collision(m->primitives.size(), false);
  int max_lod = -1;
  std::vector<bool> present(101, false);
  for (std::size_t i = 0; i < m->primitives.size(); ++i) {
    for (const std::string& part : name_parts(m->primitives[i])) {
      if (collision_name(part)) collision[i] = true;
      const int l = lod_level(part);
      if (l >= 0) level[i] = l;  // the deepest tag wins
    }
    if (!collision[i] && level[i] >= 0) {
      max_lod = std::max(max_lod, level[i]);
      present[static_cast<std::size_t>(level[i])] = true;
    }
  }
  // The requested LOD when it exists, else the finest one there is.
  int keep = -1;
  if (max_lod >= 0) {
    keep = std::clamp(lod, 0, max_lod);
    if (!present[static_cast<std::size_t>(keep)]) {
      for (int l = 0; l <= max_lod; ++l) {
        if (present[static_cast<std::size_t>(l)]) {
          keep = l;
          break;
        }
      }
    }
    m->lods = std::max(m->lods, max_lod + 1);
  }
  std::vector<ModelPrimitive> out;
  int removed = 0;
  std::vector<Vec3> pos;
  for (std::size_t i = 0; i < m->primitives.size(); ++i) {
    ModelPrimitive& p = m->primitives[i];
    if (collision[i]) {
      if (keep_hulls) {
        posed_positions(*m, p, rest, &pos);
        if (pos.size() >= 4) m->collision_hulls.push_back(pos);
      }
      ++removed;
      continue;
    }
    if (level[i] >= 0 && level[i] != keep) {
      ++removed;
      continue;
    }
    out.push_back(std::move(p));
  }
  // Never leave a model with nothing to draw.
  if (out.empty()) return 0;
  m->primitives = std::move(out);
  return removed;
}

void limit_skin_joints(Model* m) {
  if (!m->skinned()) return;
  const std::size_t limit = static_cast<std::size_t>(kMaxBones);
  bool needed = m->skeleton.joints.size() > limit;
  if (!needed) return;
  std::vector<ModelPrimitive> out;
  for (ModelPrimitive& p : m->primitives) {
    if (!p.mesh.skinned() || !p.joint_map.empty()) {
      out.push_back(std::move(p));
      continue;
    }
    auto used_of = [&](std::uint32_t v, std::vector<int>* js) {
      const SkinVertex& s = p.mesh.skin[v];
      for (int k = 0; k < 4; ++k) {
        if (s.weights[k] > 0.0f || k == 0) js->push_back(static_cast<int>(s.joints[k]));
      }
    };
    // Greedy: triangles in order, a new chunk when the joint set would
    // pass the limit.
    std::vector<std::vector<std::uint32_t>> chunks(1);
    std::vector<std::set<int>> sets(1);
    for (std::size_t t = 0; t + 2 < p.mesh.indices.size(); t += 3) {
      std::vector<int> js;
      for (int c = 0; c < 3; ++c) used_of(p.mesh.indices[t + c], &js);
      std::set<int> merged = sets.back();
      merged.insert(js.begin(), js.end());
      if (merged.size() > limit && !chunks.back().empty()) {
        chunks.emplace_back();
        sets.emplace_back();
        merged = std::set<int>(js.begin(), js.end());
      }
      sets.back() = std::move(merged);
      for (int c = 0; c < 3; ++c) chunks.back().push_back(p.mesh.indices[t + c]);
    }
    for (std::size_t c = 0; c < chunks.size(); ++c) {
      if (chunks[c].empty()) continue;
      ModelPrimitive q;
      q.material = p.material;
      q.node = p.node;
      q.mesh.name = p.mesh.name;
      std::map<int, int> local;
      for (int j : sets[c]) {
        local[j] = static_cast<int>(q.joint_map.size());
        q.joint_map.push_back(j);
      }
      std::map<std::uint32_t, std::uint32_t> vmap;
      for (std::uint32_t v : chunks[c]) {
        auto it = vmap.find(v);
        if (it == vmap.end()) {
          const std::uint32_t nv = static_cast<std::uint32_t>(q.mesh.vertices.size());
          q.mesh.vertices.push_back(p.mesh.vertices[v]);
          SkinVertex s = p.mesh.skin[v];
          for (int k = 0; k < 4; ++k) {
            auto l = local.find(static_cast<int>(s.joints[k]));
            if (l != local.end()) {
              s.joints[k] = static_cast<float>(l->second);
            } else {
              s.joints[k] = 0.0f;
              s.weights[k] = 0.0f;
            }
          }
          q.mesh.skin.push_back(s);
          it = vmap.emplace(v, nv).first;
        }
        q.mesh.indices.push_back(it->second);
      }
      out.push_back(std::move(q));
    }
  }
  m->primitives = std::move(out);
}

void hitbox_matrices(const Model& m, const std::vector<Mat4>* palette, std::vector<Mat4>* out) {
  out->resize(m.hitboxes.size());
  for (std::size_t i = 0; i < m.hitboxes.size(); ++i) {
    const ModelHitbox& h = m.hitboxes[i];
    const bool posed = palette && h.bone >= 0 && static_cast<std::size_t>(h.bone) < palette->size() &&
                       static_cast<std::size_t>(h.bone) < m.skeleton.joints.size();
    // Joint (model space) = palette * bind (inverse of the inverse bind).
    (*out)[i] = posed ? (*palette)[static_cast<std::size_t>(h.bone)] *
                            m.skeleton.joints[static_cast<std::size_t>(h.bone)].inverse_bind.inverse()
                      : h.bind;
  }
}

void bake_import_transform(Model* m, float scale, bool z_up) {
  if (scale == 1.0f && !z_up) {
    update_bounds(m);
    return;
  }
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
  for (ModelHitbox& h : m->hitboxes) h.bind = c * h.bind;
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
  if (out->format != "source-mdl") {
    // Collision (UCX_...) and LOD > chosen meshes: not drawn, not bounds.
    split_special_meshes(out, opt.lod, opt.collision);
  }
  if (!opt.collision) out->collision_hulls.clear();
  bake_import_transform(out, opt.unit_scale, z_up);
  limit_skin_joints(out);
  update_bounds(out);
  // Authored clips sit beside the file (hero.gltf -> hero.anim3d.json).
  // They replace a clip of the same name and append the rest, then skin
  // through the same palette as anything the file imported.
  if (opt.animations && out->skinned()) {
    std::string clip_warn;
    if (!merge_clip_sidecar(out, &clip_warn)) {
      if (!err.empty()) err += "; ";
      err += clip_warn.empty() ? "anim3d sidecar failed" : clip_warn;
    } else if (!clip_warn.empty()) {
      if (!err.empty()) err += "; ";
      err += clip_warn;
    }
  }
  if (out->primitives.empty()) {
    if (error) *error = path + ": no meshes";
    return false;
  }
  apply_fitted_cache_file(path, out);
  if (error) *error = err;
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
