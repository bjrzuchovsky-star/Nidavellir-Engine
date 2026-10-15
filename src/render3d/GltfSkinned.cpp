// Skinned glTF writing (one skin + its clips, everything embedded) and the
// two generated CC0 assets that use it: the "Drifter" rider and a VR hand.

#include "render3d/Gltf.h"
#include "render3d/Json.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;
using json::Value;

namespace {

std::string b64(const std::string& in) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8) |
                       static_cast<unsigned char>(in[i + 2]);
    out += k[(v >> 18) & 63];
    out += k[(v >> 12) & 63];
    out += k[(v >> 6) & 63];
    out += k[v & 63];
  }
  if (i < in.size()) {
    unsigned v = static_cast<unsigned char>(in[i]) << 16;
    if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
    out += k[(v >> 18) & 63];
    out += k[(v >> 12) & 63];
    out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

struct Writer {
  std::string bin;
  Value accessors = Value::array(), views = Value::array();
  void pad() {
    while (bin.size() % 4) bin.push_back('\0');
  }
  double view(std::size_t off, std::size_t len, int target) {
    Value v = Value::object();
    v.set("buffer", Value::number(0));
    v.set("byteOffset", Value::number(static_cast<double>(off)));
    v.set("byteLength", Value::number(static_cast<double>(len)));
    if (target) v.set("target", Value::number(target));
    views.push(std::move(v));
    return static_cast<double>(views.size() - 1);
  }
  // Floats (n components per element).
  double floats(const float* data, std::size_t count, int comps, const char* type, int target,
                bool minmax) {
    pad();
    const std::size_t off = bin.size();
    bin.append(reinterpret_cast<const char*>(data), count * static_cast<std::size_t>(comps) * 4);
    Value a = Value::object();
    a.set("bufferView", Value::number(view(off, count * static_cast<std::size_t>(comps) * 4, target)));
    a.set("componentType", Value::number(5126));
    a.set("count", Value::number(static_cast<double>(count)));
    a.set("type", Value::string(type));
    if (minmax && count > 0) {
      std::vector<float> mn(static_cast<std::size_t>(comps), 1e30f), mx(static_cast<std::size_t>(comps), -1e30f);
      for (std::size_t i = 0; i < count; ++i) {
        for (int c = 0; c < comps; ++c) {
          const float f = data[i * static_cast<std::size_t>(comps) + static_cast<std::size_t>(c)];
          mn[static_cast<std::size_t>(c)] = std::min(mn[static_cast<std::size_t>(c)], f);
          mx[static_cast<std::size_t>(c)] = std::max(mx[static_cast<std::size_t>(c)], f);
        }
      }
      a.set("min", json::floats(mn.data(), mn.size()));
      a.set("max", json::floats(mx.data(), mx.size()));
    }
    accessors.push(std::move(a));
    return static_cast<double>(accessors.size() - 1);
  }
  double u16x4(const std::vector<std::uint16_t>& data) {
    pad();
    const std::size_t off = bin.size();
    bin.append(reinterpret_cast<const char*>(data.data()), data.size() * 2);
    Value a = Value::object();
    a.set("bufferView", Value::number(view(off, data.size() * 2, 34962)));
    a.set("componentType", Value::number(5123));
    a.set("count", Value::number(static_cast<double>(data.size() / 4)));
    a.set("type", Value::string("VEC4"));
    accessors.push(std::move(a));
    return static_cast<double>(accessors.size() - 1);
  }
  double u32(const std::vector<std::uint32_t>& data) {
    pad();
    const std::size_t off = bin.size();
    bin.append(reinterpret_cast<const char*>(data.data()), data.size() * 4);
    Value a = Value::object();
    a.set("bufferView", Value::number(view(off, data.size() * 4, 34963)));
    a.set("componentType", Value::number(5125));
    a.set("count", Value::number(static_cast<double>(data.size())));
    a.set("type", Value::string("SCALAR"));
    accessors.push(std::move(a));
    return static_cast<double>(accessors.size() - 1);
  }
};

Value material_json(const MaterialDesc& md) {
  Value mat = Value::object();
  mat.set("name", Value::string(md.name.empty() ? "part" : md.name));
  Value pbr = Value::object();
  pbr.set("baseColorFactor", json::floats(md.base_color, 4));
  pbr.set("metallicFactor", Value::number(md.metallic));
  pbr.set("roughnessFactor", Value::number(md.roughness));
  mat.set("pbrMetallicRoughness", std::move(pbr));
  if (md.double_sided) mat.set("doubleSided", Value::boolean(true));
  return mat;
}

}  // namespace

bool write_skinned_gltf(const std::string& path, const std::vector<GltfPart>& parts,
                        const Skeleton& skel, const std::vector<Clip>& clips, std::string* error) {
  if (skel.joints.empty() || skel.joints.size() > static_cast<std::size_t>(kMaxBones)) {
    if (error) *error = "write_skinned_gltf: 1.." + std::to_string(kMaxBones) + " joints needed";
    return false;
  }
  for (std::size_t i = 0; i < skel.joints.size(); ++i) {
    if (skel.joints[i].parent >= static_cast<int>(i)) {
      if (error) *error = "write_skinned_gltf: parents must come before children";
      return false;
    }
  }
  Writer w;
  Value nodes = Value::array(), meshes = Value::array(), materials = Value::array(),
        scene_nodes = Value::array();
  // Joint nodes 0..J-1.
  const std::size_t nj = skel.joints.size();
  for (std::size_t i = 0; i < nj; ++i) {
    const Joint& j = skel.joints[i];
    Value n = Value::object();
    n.set("name", Value::string(j.name));
    const float t[3] = {j.t.x, j.t.y, j.t.z}, r[4] = {j.r.x, j.r.y, j.r.z, j.r.w},
                s[3] = {j.s.x, j.s.y, j.s.z};
    n.set("translation", json::floats(t, 3));
    if (!(j.r == Quat{})) n.set("rotation", json::floats(r, 4));
    if (!(j.s == Vec3{1, 1, 1})) n.set("scale", json::floats(s, 3));
    Value kids = Value::array();
    for (std::size_t c = 0; c < nj; ++c) {
      if (skel.joints[c].parent == static_cast<int>(i)) kids.push(Value::number(static_cast<double>(c)));
    }
    if (kids.size() > 0) n.set("children", std::move(kids));
    nodes.push(std::move(n));
    if (j.parent < 0) scene_nodes.push(Value::number(static_cast<double>(i)));
  }
  // Inverse binds from the rest pose.
  std::vector<Mat4> global;
  joint_matrices(skel, rest_pose(skel), &global);
  std::vector<float> ibm;
  for (const Mat4& g : global) {
    const Mat4 inv = g.inverse();
    ibm.insert(ibm.end(), inv.m, inv.m + 16);
  }
  const double ibm_acc = w.floats(ibm.data(), nj, 16, "MAT4", 0, false);
  for (std::size_t pi = 0; pi < parts.size(); ++pi) {
    const MeshData& m = parts[pi].mesh;
    if (!m.valid() || !m.skinned()) {
      if (error) *error = "write_skinned_gltf: part " + m.name + " is not a valid skinned mesh";
      return false;
    }
    const std::size_t n = m.vertices.size();
    std::vector<float> pos, nrm, uv, wts;
    std::vector<std::uint16_t> jts;
    for (std::size_t v = 0; v < n; ++v) {
      const Vertex3D& vx = m.vertices[v];
      pos.insert(pos.end(), {vx.position.x, vx.position.y, vx.position.z});
      nrm.insert(nrm.end(), {vx.normal.x, vx.normal.y, vx.normal.z});
      uv.insert(uv.end(), {vx.u, vx.v});
      for (int k = 0; k < 4; ++k) {
        jts.push_back(static_cast<std::uint16_t>(std::clamp(static_cast<int>(m.skin[v].joints[k]), 0,
                                                            static_cast<int>(nj) - 1)));
        wts.push_back(m.skin[v].weights[k]);
      }
    }
    Value attrs = Value::object();
    attrs.set("POSITION", Value::number(w.floats(pos.data(), n, 3, "VEC3", 34962, true)));
    attrs.set("NORMAL", Value::number(w.floats(nrm.data(), n, 3, "VEC3", 34962, false)));
    attrs.set("TEXCOORD_0", Value::number(w.floats(uv.data(), n, 2, "VEC2", 34962, false)));
    attrs.set("JOINTS_0", Value::number(w.u16x4(jts)));
    attrs.set("WEIGHTS_0", Value::number(w.floats(wts.data(), n, 4, "VEC4", 34962, false)));
    Value prim = Value::object();
    prim.set("attributes", std::move(attrs));
    prim.set("indices", Value::number(w.u32(m.indices)));
    prim.set("material", Value::number(static_cast<double>(pi)));
    Value prims = Value::array();
    prims.push(std::move(prim));
    Value mesh = Value::object();
    mesh.set("name", Value::string(m.name.empty() ? "part" : m.name));
    mesh.set("primitives", std::move(prims));
    meshes.push(std::move(mesh));
    materials.push(material_json(parts[pi].material));
    Value node = Value::object();
    node.set("name", Value::string(m.name.empty() ? "part" : m.name));
    node.set("mesh", Value::number(static_cast<double>(pi)));
    node.set("skin", Value::number(0));
    scene_nodes.push(Value::number(static_cast<double>(nodes.size())));
    nodes.push(std::move(node));
  }
  Value skin = Value::object();
  skin.set("name", Value::string("skin"));
  skin.set("inverseBindMatrices", Value::number(ibm_acc));
  Value joints = Value::array();
  for (std::size_t i = 0; i < nj; ++i) joints.push(Value::number(static_cast<double>(i)));
  skin.set("joints", std::move(joints));
  Value skins = Value::array();
  skins.push(std::move(skin));

  Value anims = Value::array();
  for (const Clip& clip : clips) {
    Value samplers = Value::array(), channels = Value::array();
    for (const Channel& ch : clip.channels) {
      if (ch.joint < 0 || static_cast<std::size_t>(ch.joint) >= nj || ch.times.empty()) continue;
      const int width = ch.width();
      const std::size_t per = ch.interp == Interp::CubicSpline ? 3 : 1;
      if (ch.values.size() != ch.times.size() * per * static_cast<std::size_t>(width)) {
        if (error) *error = "write_skinned_gltf: clip " + clip.name + " has a short channel";
        return false;
      }
      Value smp = Value::object();
      smp.set("input", Value::number(w.floats(ch.times.data(), ch.times.size(), 1, "SCALAR", 0, true)));
      smp.set("output", Value::number(w.floats(ch.values.data(), ch.times.size() * per, width,
                                               width == 4 ? "VEC4" : "VEC3", 0, false)));
      smp.set("interpolation", Value::string(ch.interp == Interp::Step ? "STEP"
                                             : ch.interp == Interp::CubicSpline ? "CUBICSPLINE"
                                                                                : "LINEAR"));
      Value target = Value::object();
      target.set("node", Value::number(ch.joint));
      target.set("path", Value::string(ch.path == ChannelPath::Rotation ? "rotation"
                                       : ch.path == ChannelPath::Scale ? "scale"
                                                                        : "translation"));
      Value c = Value::object();
      c.set("sampler", Value::number(static_cast<double>(samplers.size())));
      c.set("target", std::move(target));
      samplers.push(std::move(smp));
      channels.push(std::move(c));
    }
    Value a = Value::object();
    a.set("name", Value::string(clip.name));
    a.set("samplers", std::move(samplers));
    a.set("channels", std::move(channels));
    anims.push(std::move(a));
  }

  Value root = Value::object();
  Value asset = Value::object();
  asset.set("version", Value::string("2.0"));
  asset.set("generator", Value::string("TombStone Engine"));
  asset.set("copyright", Value::string("CC0 1.0 (public domain), generated by TombStone Engine"));
  root.set("asset", std::move(asset));
  root.set("scene", Value::number(0));
  Value scene = Value::object();
  scene.set("nodes", std::move(scene_nodes));
  Value scenes = Value::array();
  scenes.push(std::move(scene));
  root.set("scenes", std::move(scenes));
  root.set("nodes", std::move(nodes));
  root.set("meshes", std::move(meshes));
  root.set("materials", std::move(materials));
  root.set("skins", std::move(skins));
  if (anims.size() > 0) root.set("animations", std::move(anims));
  root.set("accessors", std::move(w.accessors));
  root.set("bufferViews", std::move(w.views));
  w.pad();
  Value buffer = Value::object();
  buffer.set("byteLength", Value::number(static_cast<double>(w.bin.size())));
  buffer.set("uri", Value::string("data:application/octet-stream;base64," + b64(w.bin)));
  Value buffers = Value::array();
  buffers.push(std::move(buffer));
  root.set("buffers", std::move(buffers));

  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "Could not write " + path;
    return false;
  }
  out << json::write(root);
  return static_cast<bool>(out);
}

// ---- Generated assets ---------------------------------------------------------

namespace {

// A box column along +Y through rings at the given heights: each ring has
// a (joint a, joint b, weight of b) skin, so limbs bend smoothly at joints.
struct Ring {
  float y;
  int a, b;
  float wb;
};

void add_column(MeshData* m, float cx, float cz, float hw, float hd, const std::vector<Ring>& rings,
                bool caps) {
  auto skin_of = [](const Ring& r) {
    SkinVertex s;
    s.joints[0] = static_cast<float>(r.a);
    s.joints[1] = static_cast<float>(r.b < 0 ? r.a : r.b);
    s.weights[0] = r.b < 0 ? 1.0f : 1.0f - r.wb;
    s.weights[1] = r.b < 0 ? 0.0f : r.wb;
    s.weights[2] = s.weights[3] = 0.0f;
    return s;
  };
  // Four sides, each with its own normal.
  const Vec3 nrm[4] = {{0, 0, 1}, {1, 0, 0}, {0, 0, -1}, {-1, 0, 0}};
  const float cx4[4][2][2] = {{{-1, 1}, {1, 1}}, {{1, 1}, {1, -1}}, {{1, -1}, {-1, -1}}, {{-1, -1}, {-1, 1}}};
  for (int side = 0; side < 4; ++side) {
    const std::uint32_t base = static_cast<std::uint32_t>(m->vertices.size());
    for (std::size_t r = 0; r < rings.size(); ++r) {
      for (int k = 0; k < 2; ++k) {
        Vertex3D v;
        v.position = {cx + cx4[side][k][0] * hw, rings[r].y, cz + cx4[side][k][1] * hd};
        v.normal = nrm[side];
        v.u = static_cast<float>(k);
        v.v = static_cast<float>(r) / static_cast<float>(rings.size() - 1);
        m->vertices.push_back(v);
        m->skin.push_back(skin_of(rings[r]));
      }
    }
    for (std::uint32_t r = 0; r + 1 < rings.size(); ++r) {
      const std::uint32_t a = base + r * 2, b = a + 1, c = a + 2, d = a + 3;
      // Counter-clockwise from outside: rings go up (+Y).
      m->indices.insert(m->indices.end(), {a, b, d, a, d, c});
    }
  }
  if (!caps) return;
  for (int end = 0; end < 2; ++end) {
    const Ring& r = end == 0 ? rings.front() : rings.back();
    const std::uint32_t base = static_cast<std::uint32_t>(m->vertices.size());
    const float sx[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; ++k) {
      Vertex3D v;
      v.position = {cx + sx[k] * hw, r.y, cz + sz[k] * hd};
      v.normal = {0, end == 0 ? -1.0f : 1.0f, 0};
      m->vertices.push_back(v);
      m->skin.push_back(skin_of(r));
    }
    if (end == 1) m->indices.insert(m->indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    else m->indices.insert(m->indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
}

void add_box(MeshData* m, Vec3 c, Vec3 half, int joint) {
  add_column(m, c.x, c.z, half.x, half.z, {{c.y - half.y, joint, -1, 0}, {c.y + half.y, joint, -1, 0}}, true);
}

MaterialDesc mat(const char* name, float r, float g, float b, float rough, bool two_sided = false) {
  MaterialDesc d;
  d.name = name;
  d.base_color[0] = r;
  d.base_color[1] = g;
  d.base_color[2] = b;
  d.base_color[3] = 1.0f;
  d.roughness = rough;
  d.double_sided = two_sided;
  return d;
}

Quat rx(float deg) { return Quat::axis_angle({1, 0, 0}, radians(deg)); }
Quat ry(float deg) { return Quat::axis_angle({0, 1, 0}, radians(deg)); }
Quat rz(float deg) { return Quat::axis_angle({0, 0, 1}, radians(deg)); }

Channel rot_channel(int joint, const std::vector<float>& times, const std::vector<Quat>& keys,
                    Interp interp = Interp::Linear) {
  Channel c;
  c.joint = joint;
  c.path = ChannelPath::Rotation;
  c.interp = interp;
  c.times = times;
  for (const Quat& q : keys) c.values.insert(c.values.end(), {q.x, q.y, q.z, q.w});
  return c;
}

Channel move_channel(int joint, const std::vector<float>& times, const std::vector<Vec3>& keys,
                     Interp interp = Interp::Linear) {
  Channel c;
  c.joint = joint;
  c.path = ChannelPath::Translation;
  c.interp = interp;
  c.times = times;
  for (const Vec3& v : keys) {
    if (interp == Interp::CubicSpline) c.values.insert(c.values.end(), {0, 0, 0});  // in-tangent
    c.values.insert(c.values.end(), {v.x, v.y, v.z});
    if (interp == Interp::CubicSpline) c.values.insert(c.values.end(), {0, 0, 0});  // out-tangent
  }
  return c;
}

// One cycle of a swinging joint: sin-shaped keys at 5 points.
Channel swing(int joint, float period, float amp, float phase, char axis, float bias = 0.0f) {
  std::vector<float> t;
  std::vector<Quat> q;
  for (int k = 0; k <= 8; ++k) {
    const float s = static_cast<float>(k) / 8.0f;
    t.push_back(s * period);
    const float a = bias + amp * std::sin((s + phase) * 2.0f * kPi);
    q.push_back(axis == 'x' ? rx(a) : axis == 'y' ? ry(a) : rz(a));
  }
  return rot_channel(joint, t, q);
}

// Knee / elbow: bends one way only, twice the swing frequency feel.
Channel bend(int joint, float period, float amp, float phase, char axis) {
  std::vector<float> t;
  std::vector<Quat> q;
  for (int k = 0; k <= 8; ++k) {
    const float s = static_cast<float>(k) / 8.0f;
    t.push_back(s * period);
    const float a = amp * std::max(0.0f, std::sin((s + phase) * 2.0f * kPi));
    q.push_back(axis == 'x' ? rx(a) : rz(a));
  }
  return rot_channel(joint, t, q);
}

}  // namespace

bool write_sample_character_gltf(const std::string& path, std::string* error) {
  enum J { Hips, Spine, Chest, Neck, Head, UaL, LaL, HandL, UaR, LaR, HandR, UlL, LlL, UlR, LlR };
  Skeleton sk;
  auto joint = [&](const char* name, int parent, Vec3 t) {
    Joint j;
    j.name = name;
    j.parent = parent;
    j.t = t;
    sk.joints.push_back(j);
  };
  // Model faces -Z (the engine's forward). Left = -X.
  joint("hips", -1, {0, 0.95f, 0});
  joint("spine", Hips, {0, 0.15f, 0});
  joint("chest", Spine, {0, 0.25f, 0});
  joint("neck", Chest, {0, 0.25f, 0});
  joint("head", Neck, {0, 0.10f, 0});
  joint("upper_arm.L", Chest, {-0.24f, 0.18f, 0});
  joint("lower_arm.L", UaL, {0, -0.28f, 0});
  joint("hand.L", LaL, {0, -0.26f, 0});
  joint("upper_arm.R", Chest, {0.24f, 0.18f, 0});
  joint("lower_arm.R", UaR, {0, -0.28f, 0});
  joint("hand.R", LaR, {0, -0.26f, 0});
  joint("upper_leg.L", Hips, {-0.1f, -0.05f, 0});
  joint("lower_leg.L", UlL, {0, -0.42f, 0});
  joint("upper_leg.R", Hips, {0.1f, -0.05f, 0});
  joint("lower_leg.R", UlR, {0, -0.42f, 0});

  std::vector<GltfPart> parts(5);
  parts[0].material = mat("shirt", 0.62f, 0.27f, 0.18f, 0.8f);
  parts[1].material = mat("denim", 0.20f, 0.28f, 0.45f, 0.85f);
  parts[2].material = mat("leather", 0.30f, 0.19f, 0.11f, 0.6f);
  parts[3].material = mat("skin", 0.86f, 0.64f, 0.48f, 0.7f);
  parts[4].material = mat("hat", 0.42f, 0.30f, 0.18f, 0.9f);
  MeshData& shirt = parts[0].mesh;
  MeshData& denim = parts[1].mesh;
  MeshData& leather = parts[2].mesh;
  MeshData& skinm = parts[3].mesh;
  MeshData& hat = parts[4].mesh;
  shirt.name = "shirt";
  denim.name = "pants";
  leather.name = "boots";
  skinm.name = "skin";
  hat.name = "hat";
  // Torso: hips -> spine -> chest.
  add_column(&shirt, 0, 0, 0.18f, 0.11f,
             {{0.90f, Hips, -1, 0}, {1.05f, Hips, Spine, 0.5f}, {1.22f, Spine, Chest, 0.5f},
              {1.40f, Chest, -1, 0}, {1.60f, Chest, -1, 0}},
             true);
  // Arms (shirt sleeves) and hands.
  for (int side = 0; side < 2; ++side) {
    const float x = side == 0 ? -0.24f : 0.24f;
    const int ua = side == 0 ? UaL : UaR, la = side == 0 ? LaL : LaR, hd = side == 0 ? HandL : HandR;
    add_column(&shirt, x, 0, 0.065f, 0.065f,
               {{1.02f, la, -1, 0}, {1.22f, la, -1, 0}, {1.28f, ua, la, 0.5f}, {1.34f, ua, -1, 0},
                {1.60f, ua, -1, 0}},
               true);
    add_box(&skinm, {x, 0.95f, 0}, {0.05f, 0.07f, 0.06f}, hd);
  }
  // Legs, boots.
  for (int side = 0; side < 2; ++side) {
    const float x = side == 0 ? -0.1f : 0.1f;
    const int ul = side == 0 ? UlL : UlR, ll = side == 0 ? LlL : LlR;
    add_column(&denim, x, 0, 0.08f, 0.09f,
               {{0.12f, ll, -1, 0}, {0.42f, ll, -1, 0}, {0.48f, ul, ll, 0.5f}, {0.55f, ul, -1, 0},
                {0.92f, ul, Hips, 0.3f}},
               true);
    add_box(&leather, {x, 0.06f, -0.04f}, {0.085f, 0.06f, 0.14f}, ll);
  }
  add_box(&leather, {0, 0.93f, 0}, {0.19f, 0.03f, 0.12f}, Hips);  // belt
  // Neck, head, hat.
  add_box(&skinm, {0, 1.64f, 0}, {0.05f, 0.04f, 0.05f}, Neck);
  add_box(&skinm, {0, 1.79f, 0}, {0.11f, 0.12f, 0.11f}, Head);
  add_box(&hat, {0, 1.92f, 0}, {0.22f, 0.012f, 0.22f}, Head);
  add_box(&hat, {0, 1.98f, 0}, {0.12f, 0.06f, 0.12f}, Head);
  for (GltfPart& p : parts) {
    p.material.metallic = 0.0f;
    compute_tangents(p.mesh);
  }

  std::vector<Clip> clips;
  {  // idle: breathe, look around.
    Clip c;
    c.name = "idle";
    c.duration = 2.0f;
    c.channels.push_back(swing(Chest, 2.0f, 2.0f, 0.0f, 'x'));
    c.channels.push_back(swing(Head, 2.0f, 8.0f, 0.25f, 'y'));
    c.channels.push_back(swing(UaL, 2.0f, 3.0f, 0.0f, 'z', -4.0f));
    c.channels.push_back(swing(UaR, 2.0f, 3.0f, 0.5f, 'z', 4.0f));
    clips.push_back(c);
  }
  auto gait = [&](const char* name, float period, float leg, float knee, float arm, float elbow,
                  float lean, float bob, Interp hip_interp) {
    Clip c;
    c.name = name;
    c.duration = period;
    c.channels.push_back(swing(UlL, period, leg, 0.0f, 'x'));
    c.channels.push_back(swing(UlR, period, leg, 0.5f, 'x'));
    c.channels.push_back(bend(LlL, period, -knee, 0.5f, 'x'));
    c.channels.push_back(bend(LlR, period, -knee, 0.0f, 'x'));
    c.channels.push_back(swing(UaL, period, arm, 0.5f, 'x'));
    c.channels.push_back(swing(UaR, period, arm, 0.0f, 'x'));
    c.channels.push_back(bend(LaL, period, elbow, 0.5f, 'x'));
    c.channels.push_back(bend(LaR, period, elbow, 0.0f, 'x'));
    c.channels.push_back(rot_channel(Spine, {0, period}, {rx(-lean), rx(-lean)}));
    std::vector<float> t;
    std::vector<Vec3> v;
    for (int k = 0; k <= 4; ++k) {
      t.push_back(period * static_cast<float>(k) / 4.0f);
      v.push_back({0, 0.95f + ((k % 2) ? -bob : bob * 0.3f), 0});
    }
    c.channels.push_back(move_channel(Hips, t, v, hip_interp));
    clips.push_back(c);
  };
  gait("walk", 1.0f, 25.0f, 35.0f, 20.0f, 15.0f, 3.0f, 0.02f, Interp::Linear);
  gait("run", 0.6f, 45.0f, 75.0f, 40.0f, 70.0f, 12.0f, 0.05f, Interp::CubicSpline);
  {  // jump: crouch, tuck, land (arms snap up: step keys).
    Clip c;
    c.name = "jump";
    c.duration = 0.8f;
    c.channels.push_back(rot_channel(UlL, {0, 0.2f, 0.5f, 0.8f}, {rx(0), rx(40), rx(55), rx(0)}));
    c.channels.push_back(rot_channel(UlR, {0, 0.2f, 0.5f, 0.8f}, {rx(0), rx(35), rx(50), rx(0)}));
    c.channels.push_back(rot_channel(LlL, {0, 0.2f, 0.5f, 0.8f}, {rx(0), rx(-60), rx(-80), rx(0)}));
    c.channels.push_back(rot_channel(LlR, {0, 0.2f, 0.5f, 0.8f}, {rx(0), rx(-55), rx(-75), rx(0)}));
    c.channels.push_back(rot_channel(UaL, {0, 0.15f, 0.6f}, {rz(0), rz(-120), rz(-20)}, Interp::Step));
    c.channels.push_back(rot_channel(UaR, {0, 0.15f, 0.6f}, {rz(0), rz(120), rz(20)}, Interp::Step));
    clips.push_back(c);
  }
  {  // wave: right arm only (an upper-body layer clip).
    Clip c;
    c.name = "wave";
    c.duration = 1.2f;
    c.channels.push_back(rot_channel(UaR, {0, 0.25f, 0.95f, 1.2f}, {rz(10), rz(150), rz(150), rz(10)}));
    std::vector<float> t{0, 0.25f, 0.45f, 0.65f, 0.85f, 1.2f};
    c.channels.push_back(rot_channel(LaR, t, {rz(0), rz(25), rz(-25), rz(25), rz(-25), rz(0)}));
    clips.push_back(c);
  }
  return write_skinned_gltf(path, parts, sk, clips, error);
}

bool write_sample_hand_gltf(const std::string& path, std::string* error) {
  // Right hand, palm down, fingers along -Z from the wrist (origin = grip).
  Skeleton sk;
  auto joint = [&](const std::string& name, int parent, Vec3 t) {
    Joint j;
    j.name = name;
    j.parent = parent;
    j.t = t;
    sk.joints.push_back(j);
    return static_cast<int>(sk.joints.size() - 1);
  };
  const int wrist = joint("wrist", -1, {0, 0, 0});
  std::vector<GltfPart> parts(1);
  parts[0].material = mat("glove", 0.45f, 0.31f, 0.2f, 0.75f, true);
  MeshData& m = parts[0].mesh;
  m.name = "hand";
  {
    // Palm as a box along -Z: build directly (columns run along Y).
    MeshData palm;
    add_box(&palm, {0, 0, 0}, {0.045f, 0.05f, 0.015f}, wrist);
    // Rotate the box so its long axis (Y) runs along -Z.
    const Quat q = rx(-90);
    for (Vertex3D& v : palm.vertices) {
      v.position = q.rotate(v.position) + Vec3{0, 0, 0.0f};
      v.normal = q.rotate(v.normal);
    }
    m = palm;
    m.name = "hand";
  }
  const float xs[4] = {-0.033f, -0.011f, 0.011f, 0.033f};
  const char* names[4] = {"index", "middle", "ring", "pinky"};
  const float lens[4] = {0.035f, 0.04f, 0.037f, 0.03f};
  std::vector<int> curl_joints;
  auto finger = [&](const std::string& name, Vec3 at, float len, Quat base_rot) {
    const int a = joint(name + "1", wrist, at);
    sk.joints[static_cast<std::size_t>(a)].r = base_rot;
    const int b = joint(name + "2", a, {0, 0, -len});
    curl_joints.push_back(a);
    curl_joints.push_back(b);
    // Two segments along -Z in the joints' frames, built in model space at
    // rest (straight), so build each as a box rotated to lie along -Z.
    std::vector<Mat4> g;
    joint_matrices(sk, rest_pose(sk), &g);
    for (int seg = 0; seg < 2; ++seg) {
      MeshData box;
      add_box(&box, {0, 0, 0}, {0.009f, len * 0.5f, 0.009f}, seg == 0 ? a : b);
      const Quat q = rx(-90);
      const Mat4 at_joint = g[static_cast<std::size_t>(seg == 0 ? a : b)];
      for (std::size_t v = 0; v < box.vertices.size(); ++v) {
        Vertex3D vx = box.vertices[v];
        const Vec3 local = q.rotate(vx.position) + Vec3{0, 0, -len * 0.5f};
        vx.position = at_joint.transform_point(local);
        vx.normal = normalize(at_joint.transform_dir(q.rotate(vx.normal)));
        m.vertices.push_back(vx);
        m.skin.push_back(box.skin[v]);
      }
      const std::uint32_t off = static_cast<std::uint32_t>(m.vertices.size() - box.vertices.size());
      for (std::uint32_t i : box.indices) m.indices.push_back(i + off);
    }
  };
  for (int f = 0; f < 4; ++f) finger(names[f], {xs[f], 0, -0.05f}, lens[f], Quat{});
  finger("thumb", {-0.045f, 0, -0.01f}, 0.03f, ry(35));
  compute_tangents(m);

  Clip grip;
  grip.name = "grip";
  grip.duration = 1.0f;
  for (std::size_t i = 0; i < curl_joints.size(); ++i) {
    const int j = curl_joints[i];
    const Quat rest = sk.joints[static_cast<std::size_t>(j)].r;
    const bool thumb = sk.joints[static_cast<std::size_t>(j)].name.rfind("thumb", 0) == 0;
    const float amount = thumb ? -45.0f : (i % 2 == 0 ? -75.0f : -85.0f);
    grip.channels.push_back(rot_channel(j, {0.0f, 1.0f}, {rest, rest * rx(amount)}));
  }
  return write_skinned_gltf(path, parts, sk, {grip}, error);
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
