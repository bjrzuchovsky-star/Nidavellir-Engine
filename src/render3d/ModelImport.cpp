#include "render3d/HullFit.h"
#include "render3d/ModelImport.h"

#include "render3d/ImportLimits.h"

#include "core/AssetMeta.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
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
  // glTF looks along +Z. Auto turns that onto the engine's -Z forward.
  s.set("forward_axis", Value::string("auto"));
  s.set("material_path", Value::string(""));
  s.set("lod", Value::number(0));
  s.set("generate_collider", Value::boolean(true));
  // Collider a placed model gets (render3d/ColliderFit.h): auto / box /
  // capsule / convex / mesh / none.
  s.set("collider", Value::string("auto"));
  s.set("hull_vertices", Value::number(64));
  s.set("decompose_max", Value::number(8));
  s.set("import_animations", Value::boolean(true));
  s.set("max_texture", Value::number(kDefaultMaxTextureEdge));
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
  const std::string fw = lower(s.has("forward_axis") ? s.str("forward_axis") : d.str("forward_axis"));
  if (fw == "+x" || fw == "x" || fw == "posx") o.forward = ForwardAxis::PosX;
  else if (fw == "-x" || fw == "negx") o.forward = ForwardAxis::NegX;
  else if (fw == "+y" || fw == "y" || fw == "posy") o.forward = ForwardAxis::PosY;
  else if (fw == "-y" || fw == "negy") o.forward = ForwardAxis::NegY;
  else if (fw == "+z" || fw == "z" || fw == "posz") o.forward = ForwardAxis::PosZ;
  else if (fw == "-z" || fw == "negz" || fw == "none") o.forward = ForwardAxis::NegZ;
  else o.forward = ForwardAxis::Auto;
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
  o.max_texture = static_cast<int>(num("max_texture"));
  if (o.max_texture < 0) o.max_texture = 0;
  if (o.max_texture > kHardMaxTextureEdge) o.max_texture = kHardMaxTextureEdge;
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

namespace {

struct MonitorSlot {
  ImportMonitor mon{};
  bool on = false;
  int texture_edge = 0;
  std::string notes;
};

MonitorSlot& monitor_slot() {
  thread_local MonitorSlot slot;
  return slot;
}

}  // namespace

void set_import_monitor(const ImportMonitor* monitor) {
  MonitorSlot& s = monitor_slot();
  if (!monitor) {
    s.on = false;
    s.mon = {};
    return;
  }
  s.mon = *monitor;
  s.on = true;
}

bool import_was_cancelled() {
  const MonitorSlot& s = monitor_slot();
  return s.on && s.mon.cancelled && s.mon.cancelled(s.mon.user);
}

void import_report(int percent, const char* phase) {
  const MonitorSlot& s = monitor_slot();
  if (s.on && s.mon.progress) s.mon.progress(s.mon.user, percent, phase ? phase : "");
}

int active_import_texture_edge() { return monitor_slot().texture_edge; }

void import_note(const std::string& text) {
  if (text.empty()) return;
  MonitorSlot& s = monitor_slot();
  if (s.notes.find(text) != std::string::npos) return;
  if (!s.notes.empty()) s.notes += " ";
  s.notes += text;
}

std::string import_take_notes() {
  std::string out;
  out.swap(monitor_slot().notes);
  return out;
}

namespace {

struct TextureEdgeScope {
  int prev;
  explicit TextureEdgeScope(int edge) : prev(monitor_slot().texture_edge) {
    monitor_slot().texture_edge = edge;
  }
  ~TextureEdgeScope() { monitor_slot().texture_edge = prev; }
};

}  // namespace

void limit_skin_joints(Model* m) {
  if (!m->skinned()) return;
  const std::size_t limit = static_cast<std::size_t>(kMaxBones);
  if (m->skeleton.joints.size() <= limit) return;
  const int nj = static_cast<int>(m->skeleton.joints.size());
  std::vector<ModelPrimitive> out;
  out.reserve(m->primitives.size());
  for (ModelPrimitive& p : m->primitives) {
    if (!p.mesh.skinned() || !p.joint_map.empty()) {
      out.push_back(std::move(p));
      continue;
    }
    // The old path copied a std::set per triangle. On a high-poly skin that
    // is millions of allocations and the thing that froze a rig import.
    // A byte per joint says whether the current chunk already has it.
    std::vector<char> in_chunk(static_cast<std::size_t>(std::max(nj, 0)), 0);
    struct Chunk {
      std::vector<std::uint32_t> idx;
      std::vector<int> joints;
    };
    std::vector<Chunk> chunks(1);
    auto joints_of = [&](std::uint32_t v, int* js, int* njs) {
      if (v >= p.mesh.skin.size()) return;
      const SkinVertex& s = p.mesh.skin[v];
      for (int k = 0; k < 4; ++k) {
        if (s.weights[k] <= 0.0f && k != 0) continue;
        const int j = static_cast<int>(s.joints[k] + 0.5f);
        if (j < 0 || j >= nj) continue;
        bool seen = false;
        for (int i = 0; i < *njs; ++i) seen = seen || js[i] == j;
        if (!seen && *njs < 12) js[(*njs)++] = j;
      }
    };
    auto grow = [&](const int* js, int njs) {
      int add = 0;
      for (int i = 0; i < njs; ++i)
        if (!in_chunk[static_cast<std::size_t>(js[i])]) ++add;
      if (!chunks.back().idx.empty() && chunks.back().joints.size() + static_cast<std::size_t>(add) > limit) {
        for (int j : chunks.back().joints) in_chunk[static_cast<std::size_t>(j)] = 0;
        chunks.emplace_back();
      }
      for (int i = 0; i < njs; ++i) {
        const int j = js[i];
        if (in_chunk[static_cast<std::size_t>(j)]) continue;
        in_chunk[static_cast<std::size_t>(j)] = 1;
        chunks.back().joints.push_back(j);
      }
    };
    for (std::size_t t = 0; t + 2 < p.mesh.indices.size(); t += 3) {
      if ((t & 0xffffu) == 0 && import_was_cancelled()) return;
      int js[12];
      int njs = 0;
      for (int c = 0; c < 3; ++c) joints_of(p.mesh.indices[t + static_cast<std::size_t>(c)], js, &njs);
      grow(js, njs);
      for (int c = 0; c < 3; ++c) chunks.back().idx.push_back(p.mesh.indices[t + static_cast<std::size_t>(c)]);
    }
    for (Chunk& ch : chunks) {
      if (ch.idx.empty()) continue;
      ModelPrimitive q;
      q.material = p.material;
      q.node = p.node;
      q.mesh.name = p.mesh.name;
      q.joint_map.reserve(ch.joints.size());
      std::vector<int> local(static_cast<std::size_t>(std::max(nj, 1)), -1);
      for (int j : ch.joints) {
        local[static_cast<std::size_t>(j)] = static_cast<int>(q.joint_map.size());
        q.joint_map.push_back(j);
      }
      std::vector<std::uint32_t> vmap(p.mesh.vertices.size(), 0xffffffffu);
      q.mesh.vertices.reserve(std::min(p.mesh.vertices.size(), ch.idx.size()));
      q.mesh.skin.reserve(q.mesh.vertices.capacity());
      q.mesh.indices.reserve(ch.idx.size());
      for (std::uint32_t v : ch.idx) {
        if (v >= p.mesh.vertices.size()) continue;
        std::uint32_t& slot = vmap[v];
        if (slot == 0xffffffffu) {
          slot = static_cast<std::uint32_t>(q.mesh.vertices.size());
          q.mesh.vertices.push_back(p.mesh.vertices[v]);
          SkinVertex s = v < p.mesh.skin.size() ? p.mesh.skin[v] : SkinVertex{};
          for (int k = 0; k < 4; ++k) {
            const int j = static_cast<int>(s.joints[k] + 0.5f);
            if (j >= 0 && j < nj && local[static_cast<std::size_t>(j)] >= 0) {
              s.joints[k] = static_cast<float>(local[static_cast<std::size_t>(j)]);
            } else {
              s.joints[k] = 0.0f;
              s.weights[k] = 0.0f;
            }
          }
          q.mesh.skin.push_back(s);
        }
        q.mesh.indices.push_back(slot);
      }
      if (q.mesh.valid()) out.push_back(std::move(q));
    }
  }
  if (!out.empty()) m->primitives = std::move(out);
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

namespace {

ForwardAxis resolved_forward(const Model& m, ForwardAxis axis) {
  if (axis != ForwardAxis::Auto) return axis;
  return m.format == "gltf" ? ForwardAxis::PosZ : ForwardAxis::NegZ;
}

Quat forward_quat(ForwardAxis axis) {
  switch (axis) {
    case ForwardAxis::PosZ: return Quat::axis_angle({0, 1, 0}, kPi);
    case ForwardAxis::PosX: return Quat::axis_angle({0, 1, 0}, kPi * 0.5f);
    case ForwardAxis::NegX: return Quat::axis_angle({0, 1, 0}, -kPi * 0.5f);
    case ForwardAxis::PosY: return Quat::axis_angle({1, 0, 0}, -kPi * 0.5f);
    case ForwardAxis::NegY: return Quat::axis_angle({1, 0, 0}, kPi * 0.5f);
    case ForwardAxis::NegZ:
    case ForwardAxis::Auto: return {};
  }
  return {};
}

}  // namespace

void bake_forward_axis(Model* m, ForwardAxis axis) {
  if (!m) return;
  const Quat q = forward_quat(resolved_forward(*m, axis));
  if (std::fabs(q.x) + std::fabs(q.y) + std::fabs(q.z) < 1.0e-8f) return;
  const Mat4 c = Mat4::rotate(q);
  const Mat4 c_inv = c.inverse();
  for (ModelPrimitive& p : m->primitives) {
    for (Vertex3D& v : p.mesh.vertices) {
      v.position = q.rotate(v.position);
      v.normal = normalize(q.rotate(v.normal));
      const Vec3 t = q.rotate(Vec3{v.tangent.x, v.tangent.y, v.tangent.z});
      v.tangent = {t.x, t.y, t.z, v.tangent.w};
    }
  }
  for (Joint& j : m->skeleton.joints) {
    if (j.parent < 0) j.base = c * j.base;
    j.inverse_bind = j.inverse_bind * c_inv;
  }
  for (auto& hull : m->collision_hulls) {
    for (Vec3& p : hull) p = q.rotate(p);
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
  if (!out) return false;
  (void)import_take_notes();
  TextureEdgeScope edge(opt.max_texture);
  auto fail = [&](const std::string& why) {
    *out = Model{};
    if (error) *error = why;
    return false;
  };
  if (import_was_cancelled()) return fail(path + ": Import cancelled.");
  try {
    const std::string e = ext_of(path);
    *out = Model{};
    std::string err;
    bool ok = false;
    bool z_up = opt.up == UpAxis::Z;
    import_report(8, "Opening file");
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
    if (import_was_cancelled()) return fail(path + ": Import cancelled.");
    if (!ok) return fail(err.empty() ? path + ": import failed" : err);
    out->source = path;
    if (!opt.animations) out->clips.clear();
    import_report(72, "Preparing mesh");
    if (out->format != "source-mdl") {
      // Collision (UCX_...) and LOD > chosen meshes: not drawn, not bounds.
      split_special_meshes(out, opt.lod, opt.collision);
    }
    if (!opt.collision) out->collision_hulls.clear();
    bake_import_transform(out, opt.unit_scale, z_up);
    bake_forward_axis(out, opt.forward);
    limit_skin_joints(out);
    if (import_was_cancelled()) return fail(path + ": Import cancelled.");
    update_bounds(out);
    if (out->primitives.empty()) return fail(path + ": no meshes");
    apply_fitted_cache_file(path, out);
    const std::string notes = import_take_notes();
    if (!notes.empty()) {
      if (!err.empty() && err.rfind("warning:", 0) != 0) err = "warning: " + err;
      if (err.empty()) err = "warning: " + notes;
      else err += " " + notes;
    }
    import_report(88, "Model ready");
    if (error) *error = err;
    return true;
  } catch (const std::bad_alloc&) {
    return fail(path +
                ": out of memory while importing. The file is larger than this machine can hold. "
                "Lower Texture size in the asset settings, or decimate the mesh in Blender and export glTF.");
  } catch (const std::exception& ex) {
    return fail(path + ": import failed (" + ex.what() + ")");
  }
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
