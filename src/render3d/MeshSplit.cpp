#include "render3d/MeshSplit.h"

#include "render3d/ImportLimits.h"
#include "render3d/ModelImport.h"
#include "render3d/Skeleton.h"
#include "scene/JsonTree.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace ts {
namespace tombstone {
namespace render3d {
namespace {

namespace fs = std::filesystem;

std::string leaf_name(const std::string& node, const std::string& mesh) {
  std::string s = node.empty() ? mesh : node;
  const auto slash = s.find_last_of('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  return s;
}

std::string sanitise(std::string s) {
  if (s.empty()) s = "Part";
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '_' || c == '-')) c = '_';
  }
  while (!s.empty() && s.front() == '_') s.erase(s.begin());
  if (s.empty()) s = "Part";
  if (s.size() > 48) s.resize(48);
  return s;
}

std::string group_key(const ModelPrimitive& p) {
  if (!p.node.empty()) return p.node;
  if (!p.mesh.name.empty()) return p.mesh.name;
  return {};
}

struct Group {
  std::string key;
  std::string name;
  std::vector<int> prims;
};

std::vector<Group> groups_of(const Model& model) {
  std::vector<Group> groups;
  for (int i = 0; i < static_cast<int>(model.primitives.size()); ++i) {
    const ModelPrimitive& p = model.primitives[i];
    if (!p.mesh.valid()) continue;
    const std::string key = group_key(p);
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.key == key; });
    if (it == groups.end()) {
      Group g;
      g.key = key;
      g.name = sanitise(leaf_name(p.node, p.mesh.name.empty() ? "Part" : p.mesh.name));
      g.prims.push_back(i);
      groups.push_back(std::move(g));
    } else {
      it->prims.push_back(i);
    }
  }
  std::unordered_map<std::string, int> used;
  for (Group& g : groups) {
    const int n = ++used[g.name];
    if (n > 1) g.name += "_" + std::to_string(n);
  }
  return groups;
}

void bake_prim(const Model& model, const ModelPrimitive& p, MeshData* out) {
  *out = p.mesh;
  out->skin.clear();
  if (!p.mesh.skinned() || model.skeleton.empty()) return;
  std::vector<Mat4> full, pal;
  skin_palette(model.skeleton, rest_pose(model.skeleton), &full);
  primitive_palette(p, full, &pal);
  for (std::size_t v = 0; v < out->vertices.size() && v < p.mesh.skin.size(); ++v) {
    const SkinVertex& s = p.mesh.skin[v];
    out->vertices[v].position = skin_point(pal, s.joints, s.weights, p.mesh.vertices[v].position);
    int best = 0;
    float bw = -1.0f;
    for (int k = 0; k < 4; ++k) {
      if (s.weights[k] > bw) {
        bw = s.weights[k];
        best = k;
      }
    }
    const int j = static_cast<int>(s.joints[best]);
    if (j >= 0 && static_cast<std::size_t>(j) < pal.size()) {
      out->vertices[v].normal = normalize(pal[j].transform_dir(p.mesh.vertices[v].normal));
    }
  }
}

Aabb mesh_bounds(const MeshData& m) {
  Aabb b;
  bool any = false;
  for (std::uint32_t i : m.indices) {
    if (i >= m.vertices.size()) continue;
    const Vec3& p = m.vertices[i].position;
    if (!any) {
      b.min = b.max = p;
      any = true;
    }
    b.min = vmin(b.min, p);
    b.max = vmax(b.max, p);
  }
  return any ? b : Aabb{};
}

void shift(MeshData* m, const Vec3& by) {
  for (Vertex3D& v : m->vertices) v.position += by;
}

// PNG, RGBA8, stored (uncompressed) deflate. Small enough that a part's
// albedo does not need a compressor.
std::uint32_t crc32(const unsigned char* data, std::size_t n) {
  std::uint32_t c = 0xffffffffu;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= data[i];
    for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return c ^ 0xffffffffu;
}
std::uint32_t adler32(const unsigned char* data, std::size_t n) {
  std::uint32_t a = 1, b = 0;
  for (std::size_t i = 0; i < n; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}
void be32(std::string* o, std::uint32_t v) {
  o->push_back(static_cast<char>((v >> 24) & 255));
  o->push_back(static_cast<char>((v >> 16) & 255));
  o->push_back(static_cast<char>((v >> 8) & 255));
  o->push_back(static_cast<char>(v & 255));
}
void chunk(std::string* o, const char* type, const std::string& data) {
  be32(o, static_cast<std::uint32_t>(data.size()));
  const std::size_t at = o->size();
  o->append(type, 4);
  o->append(data);
  const auto* bytes = reinterpret_cast<const unsigned char*>(o->data() + at);
  be32(o, crc32(bytes, 4 + data.size()));
}
bool write_png(const std::string& path, const Image& img) {
  if (!img.valid()) return false;
  const int w = img.width, h = img.height;
  std::string raw;
  raw.reserve(static_cast<std::size_t>(h) * (1 + w * 4));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.append(reinterpret_cast<const char*>(img.rgba.data() + static_cast<std::size_t>(y) * w * 4),
               static_cast<std::size_t>(w) * 4);
  }
  std::string zlib;
  zlib.push_back(0x78);
  zlib.push_back(0x01);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(raw.data());
  std::size_t left = raw.size();
  std::size_t off = 0;
  while (left > 0 || off == 0) {
    const std::size_t n = std::min<std::size_t>(left, 65535);
    const bool last = n == left;
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(static_cast<char>(n & 255));
    zlib.push_back(static_cast<char>((n >> 8) & 255));
    const std::uint16_t nlen = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(n));
    zlib.push_back(static_cast<char>(nlen & 255));
    zlib.push_back(static_cast<char>((nlen >> 8) & 255));
    if (n) zlib.append(reinterpret_cast<const char*>(src + off), n);
    off += n;
    left -= n;
    if (last) break;
  }
  const std::uint32_t ad = adler32(src, raw.size());
  be32(&zlib, ad);
  std::string ihdr(13, '\0');
  ihdr[0] = static_cast<char>((w >> 24) & 255);
  ihdr[1] = static_cast<char>((w >> 16) & 255);
  ihdr[2] = static_cast<char>((w >> 8) & 255);
  ihdr[3] = static_cast<char>(w & 255);
  ihdr[4] = static_cast<char>((h >> 24) & 255);
  ihdr[5] = static_cast<char>((h >> 16) & 255);
  ihdr[6] = static_cast<char>((h >> 8) & 255);
  ihdr[7] = static_cast<char>(h & 255);
  ihdr[8] = 8;
  ihdr[9] = 6;  // RGBA
  std::string png;
  png.append("\x89PNG\r\n\x1a\n", 8);
  chunk(&png, "IHDR", ihdr);
  chunk(&png, "IDAT", zlib);
  chunk(&png, "IEND", {});
  std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(png.data(), static_cast<std::streamsize>(png.size()));
  return static_cast<bool>(out);
}

std::string base64(const std::string& in) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
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

void append(std::string& buf, const void* p, std::size_t n) { buf.append(static_cast<const char*>(p), n); }
void pad4(std::string& buf) {
  while (buf.size() % 4) buf.push_back('\0');
}

struct TexSlot {
  const Image* image = nullptr;
  std::string uri;
};

// Keep every `step`th triangle and the vertices it uses. A high-poly part
// otherwise becomes a base64 blob big enough to exhaust memory while writing.
void simplify_part(MeshData* m, std::size_t max_tris) {
  if (!m) return;
  const std::size_t tris = m->indices.size() / 3;
  if (tris <= max_tris || max_tris == 0) return;
  const std::size_t step = std::max<std::size_t>(1, tris / max_tris);
  std::vector<std::uint32_t> idx;
  idx.reserve(max_tris * 3);
  for (std::size_t t = 0, n = 0; t < tris && n < max_tris; t += step, ++n) {
    idx.push_back(m->indices[t * 3]);
    idx.push_back(m->indices[t * 3 + 1]);
    idx.push_back(m->indices[t * 3 + 2]);
  }
  std::unordered_map<std::uint32_t, std::uint32_t> remap;
  std::vector<Vertex3D> verts;
  std::vector<std::uint32_t> packed;
  verts.reserve(idx.size());
  packed.reserve(idx.size());
  for (std::uint32_t i : idx) {
    if (i >= m->vertices.size()) continue;
    auto it = remap.find(i);
    if (it == remap.end()) {
      const auto nv = static_cast<std::uint32_t>(verts.size());
      remap.emplace(i, nv);
      verts.push_back(m->vertices[i]);
      packed.push_back(nv);
    } else {
      packed.push_back(it->second);
    }
  }
  m->vertices = std::move(verts);
  m->indices = std::move(packed);
  m->skin.clear();
}

bool write_part_gltf(const std::string& path, const std::string& node_name, const std::vector<MeshData>& meshes,
                     const std::vector<MaterialDesc>& materials, const std::string& tex_dir,
                     std::vector<std::string>* written, std::unordered_map<const Image*, std::string>* shared,
                     const Skeleton* skel, std::string* error) {
  using json::Value;
  std::error_code ec;
  fs::create_directories(fs::u8path(path).parent_path(), ec);
  std::unordered_map<const Image*, int> image_of;
  std::vector<TexSlot> images;
  auto image_index = [&](const std::shared_ptr<const Image>& img, const std::string& fallback_path,
                         const char* role) -> int {
    if (img && img->valid()) {
      if (shared) {
        auto sit = shared->find(img.get());
        if (sit != shared->end()) {
          image_of[img.get()] = static_cast<int>(images.size());
          images.push_back({img.get(), sit->second});
          return static_cast<int>(images.size()) - 1;
        }
      }
      auto it = image_of.find(img.get());
      if (it != image_of.end()) return it->second;
      const std::string file = node_name + "_" + role + (images.empty() ? "" : std::to_string(images.size())) + ".png";
      const std::string abs = (fs::u8path(tex_dir) / file).string();
      if (!write_png(abs, *img)) {
        if (error) *error = "Could not write " + abs;
        return -2;
      }
      if (written) written->push_back(abs);
      if (shared) (*shared)[img.get()] = file;
      image_of[img.get()] = static_cast<int>(images.size());
      images.push_back({img.get(), file});
      return static_cast<int>(images.size()) - 1;
    }
    if (!fallback_path.empty()) {
      images.push_back({nullptr, fallback_path});
      return static_cast<int>(images.size()) - 1;
    }
    return -1;
  };

  std::string bin;
  Value accessors = Value::array(), views = Value::array(), mats = Value::array();
  Value prims = Value::array();
  auto add_view = [&](std::size_t offset, std::size_t len, int target) {
    Value v = Value::object();
    v.set("buffer", Value::number(0));
    v.set("byteOffset", Value::number(static_cast<double>(offset)));
    v.set("byteLength", Value::number(static_cast<double>(len)));
    v.set("target", Value::number(target));
    views.push(std::move(v));
    return static_cast<double>(views.size() - 1);
  };
  auto add_accessor = [&](double view, int comp, std::size_t count, const char* type, const float* mn,
                          const float* mx) {
    Value a = Value::object();
    a.set("bufferView", Value::number(view));
    a.set("componentType", Value::number(comp));
    a.set("count", Value::number(static_cast<double>(count)));
    a.set("type", Value::string(type));
    if (mn) a.set("min", json::floats(mn, 3));
    if (mx) a.set("max", json::floats(mx, 3));
    accessors.push(std::move(a));
    return static_cast<double>(accessors.size() - 1);
  };

  if (meshes.size() != materials.size()) {
    if (error) *error = "split: mesh and material counts differ";
    return false;
  }
  for (std::size_t pi = 0; pi < meshes.size(); ++pi) {
    const MeshData& m = meshes[pi];
    double joints_acc = -1;
    double weights_acc = -1;
    if (!m.valid()) {
      if (error) *error = "split: a part has an invalid mesh";
      return false;
    }
    const Aabb b = mesh_bounds(m);
    const float mn[3] = {b.min.x, b.min.y, b.min.z}, mx[3] = {b.max.x, b.max.y, b.max.z};
    const std::size_t n = m.vertices.size();
    std::size_t off = bin.size();
    for (const Vertex3D& v : m.vertices) append(bin, &v.position, 12);
    const double pos = add_accessor(add_view(off, n * 12, 34962), 5126, n, "VEC3", mn, mx);
    off = bin.size();
    for (const Vertex3D& v : m.vertices) append(bin, &v.normal, 12);
    const double nrm = add_accessor(add_view(off, n * 12, 34962), 5126, n, "VEC3", nullptr, nullptr);
    off = bin.size();
    for (const Vertex3D& v : m.vertices) {
      const float uv[2] = {v.u, v.v};
      append(bin, uv, 8);
    }
    const double tex = add_accessor(add_view(off, n * 8, 34962), 5126, n, "VEC2", nullptr, nullptr);
    off = bin.size();
    append(bin, m.indices.data(), m.indices.size() * 4);
    const double idx = add_accessor(add_view(off, m.indices.size() * 4, 34963), 5125, m.indices.size(), "SCALAR",
                                    nullptr, nullptr);
    if (skel && !skel->joints.empty() && m.skinned()) {
      std::vector<std::uint16_t> joints(n * 4, 0);
      std::vector<float> weights(n * 4, 0.0f);
      const int nj = static_cast<int>(skel->joints.size());
      for (std::size_t v = 0; v < n && v < m.skin.size(); ++v) {
        for (int k = 0; k < 4; ++k) {
          int j = static_cast<int>(m.skin[v].joints[k]);
          if (j < 0 || j >= nj) j = 0;
          joints[v * 4 + static_cast<std::size_t>(k)] = static_cast<std::uint16_t>(j);
          weights[v * 4 + static_cast<std::size_t>(k)] = m.skin[v].weights[k];
        }
      }
      pad4(bin);
      off = bin.size();
      append(bin, joints.data(), joints.size() * sizeof(std::uint16_t));
      joints_acc = add_accessor(add_view(off, joints.size() * sizeof(std::uint16_t), 34962), 5123, n, "VEC4",
                                nullptr, nullptr);
      pad4(bin);
      off = bin.size();
      append(bin, weights.data(), weights.size() * sizeof(float));
      weights_acc = add_accessor(add_view(off, weights.size() * sizeof(float), 34962), 5126, n, "VEC4", nullptr,
                                 nullptr);
    }
    pad4(bin);

    const MaterialDesc& md = materials[pi];
    const int albedo = image_index(md.base_color_image, md.base_color_texture, "albedo");
    if (albedo == -2) return false;
    const int normal = image_index(md.normal_image, md.normal_texture, "normal");
    if (normal == -2) return false;
    const int mr = image_index(md.metal_rough_image, md.metal_rough_texture, "metalrough");
    if (mr == -2) return false;
    Value mat = Value::object();
    mat.set("name", Value::string(md.name.empty() ? node_name : md.name));
    Value pbr = Value::object();
    pbr.set("baseColorFactor", json::floats(md.base_color, 4));
    pbr.set("metallicFactor", Value::number(md.metallic));
    pbr.set("roughnessFactor", Value::number(md.roughness));
    if (albedo >= 0) {
      Value texv = Value::object();
      texv.set("index", Value::number(albedo));
      pbr.set("baseColorTexture", std::move(texv));
    }
    if (mr >= 0) {
      Value texv = Value::object();
      texv.set("index", Value::number(mr));
      pbr.set("metallicRoughnessTexture", std::move(texv));
    }
    mat.set("pbrMetallicRoughness", std::move(pbr));
    if (md.emissive[0] != 0.0f || md.emissive[1] != 0.0f || md.emissive[2] != 0.0f) {
      mat.set("emissiveFactor", json::floats(md.emissive, 3));
    }
    if (normal >= 0) {
      Value texv = Value::object();
      texv.set("index", Value::number(normal));
      mat.set("normalTexture", std::move(texv));
    }
    if (md.double_sided) mat.set("doubleSided", Value::boolean(true));
    if (md.alpha_mode == MaterialDesc::kAlphaMask) {
      mat.set("alphaMode", Value::string("MASK"));
      mat.set("alphaCutoff", Value::number(md.alpha_cutoff));
    } else if (md.alpha_mode == MaterialDesc::kAlphaBlend) {
      mat.set("alphaMode", Value::string("BLEND"));
    }
    const int mat_index = static_cast<int>(mats.size());
    mats.push(std::move(mat));

    Value attrs = Value::object();
    attrs.set("POSITION", Value::number(pos));
    attrs.set("NORMAL", Value::number(nrm));
    attrs.set("TEXCOORD_0", Value::number(tex));
    if (joints_acc >= 0) {
      attrs.set("JOINTS_0", Value::number(joints_acc));
      attrs.set("WEIGHTS_0", Value::number(weights_acc));
    }
    Value prim = Value::object();
    prim.set("attributes", std::move(attrs));
    prim.set("indices", Value::number(idx));
    prim.set("material", Value::number(mat_index));
    prims.push(std::move(prim));
  }

  Value root = Value::object();
  Value asset = Value::object();
  asset.set("version", Value::string("2.0"));
  asset.set("generator", Value::string("Nidavellir Engine"));
  root.set("asset", std::move(asset));
  root.set("scene", Value::number(0));
  Value nodes = Value::array();
  Value sn = Value::array();
  const bool with_skin = skel && !skel->joints.empty();
  if (with_skin) {
    const std::size_t nj = skel->joints.size();
    std::vector<Mat4> global;
    joint_matrices(*skel, rest_pose(*skel), &global);
    std::vector<float> ibm;
    ibm.reserve(nj * 16);
    for (const Mat4& g : global) {
      const Mat4 inv = g.inverse();
      ibm.insert(ibm.end(), inv.m, inv.m + 16);
    }
    pad4(bin);
    const std::size_t ibm_off = bin.size();
    append(bin, ibm.data(), ibm.size() * sizeof(float));
    const double ibm_acc = add_accessor(add_view(ibm_off, ibm.size() * sizeof(float), 34962), 5126, nj, "MAT4",
                                        nullptr, nullptr);
    for (std::size_t i = 0; i < nj; ++i) {
      const Joint& j = skel->joints[i];
      Value n = Value::object();
      n.set("name", Value::string(j.name));
      const float t[3] = {j.t.x, j.t.y, j.t.z};
      const float r[4] = {j.r.x, j.r.y, j.r.z, j.r.w};
      const float s[3] = {j.s.x, j.s.y, j.s.z};
      n.set("translation", json::floats(t, 3));
      n.set("rotation", json::floats(r, 4));
      n.set("scale", json::floats(s, 3));
      Value kids = Value::array();
      for (std::size_t c = 0; c < nj; ++c) {
        if (skel->joints[c].parent == static_cast<int>(i)) kids.push(Value::number(static_cast<double>(c)));
      }
      if (kids.size() > 0) n.set("children", std::move(kids));
      nodes.push(std::move(n));
      if (j.parent < 0) sn.push(Value::number(static_cast<double>(i)));
    }
    Value node = Value::object();
    node.set("name", Value::string(node_name));
    node.set("mesh", Value::number(0));
    node.set("skin", Value::number(0));
    sn.push(Value::number(static_cast<double>(nodes.size())));
    nodes.push(std::move(node));
    Value skin = Value::object();
    skin.set("inverseBindMatrices", Value::number(ibm_acc));
    Value joint_ids = Value::array();
    for (std::size_t i = 0; i < nj; ++i) joint_ids.push(Value::number(static_cast<double>(i)));
    skin.set("joints", std::move(joint_ids));
    Value skins = Value::array();
    skins.push(std::move(skin));
    root.set("skins", std::move(skins));
  } else {
    Value node = Value::object();
    node.set("name", Value::string(node_name));
    node.set("mesh", Value::number(0));
    nodes.push(std::move(node));
    sn.push(Value::number(0));
  }
  root.set("nodes", std::move(nodes));
  Value scene = Value::object();
  scene.set("nodes", std::move(sn));
  Value scenes = Value::array();
  scenes.push(std::move(scene));
  root.set("scenes", std::move(scenes));
  Value mesh = Value::object();
  mesh.set("name", Value::string(node_name));
  mesh.set("primitives", std::move(prims));
  Value mesh_list = Value::array();
  mesh_list.push(std::move(mesh));
  root.set("meshes", std::move(mesh_list));
  root.set("materials", std::move(mats));
  if (!images.empty()) {
    Value imgs = Value::array(), texs = Value::array();
    for (const TexSlot& s : images) {
      Value im = Value::object();
      im.set("uri", Value::string(s.uri));
      imgs.push(std::move(im));
      Value tx = Value::object();
      tx.set("source", Value::number(static_cast<double>(texs.size())));
      texs.push(std::move(tx));
    }
    root.set("images", std::move(imgs));
    root.set("textures", std::move(texs));
  }
  root.set("accessors", std::move(accessors));
  root.set("bufferViews", std::move(views));
  Value buffer = Value::object();
  buffer.set("byteLength", Value::number(static_cast<double>(bin.size())));
  buffer.set("uri", Value::string("data:application/octet-stream;base64," + base64(bin)));
  Value buffers = Value::array();
  buffers.push(std::move(buffer));
  root.set("buffers", std::move(buffers));

  std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "Could not write " + path;
    return false;
  }
  out << json::write(root);
  if (!out) {
    if (error) *error = "Could not write " + path;
    return false;
  }
  if (written) written->push_back(path);
  return true;
}

std::string unique_file(const fs::path& dir, const std::string& stem) {
  fs::path p = dir / (stem + ".gltf");
  if (!fs::exists(p)) return p.string();
  for (int n = 2; n < 1000; ++n) {
    p = dir / (stem + "_" + std::to_string(n) + ".gltf");
    if (!fs::exists(p)) return p.string();
  }
  return (dir / (stem + "_x.gltf")).string();
}

}  // namespace

int model_part_count(const Model& model) { return static_cast<int>(groups_of(model).size()); }

bool write_model_parts(const Model& model, const std::string& dest_dir, const SplitWrite& opt,
                       std::vector<SplitPart>* parts, std::vector<std::string>* written_abs,
                       std::string* error) {
  if (!parts) return false;
  parts->clear();
  const std::vector<Group> groups = groups_of(model);
  if (groups.size() < 2) {
    if (error) *error = "This model is one piece already.";
    return false;
  }
  std::error_code ec;
  fs::create_directories(fs::u8path(dest_dir), ec);
  std::unordered_map<const Image*, std::string> shared_tex;
  bool simplified = false;
  for (const Group& g : groups) {
    std::vector<MeshData> meshes;
    std::vector<MaterialDesc> mats;
    Vec3 acc;
    int n = 0;
    bool part_skinned = false;
    for (int pi : g.prims) {
      if (model.skinned() && model.primitives[pi].mesh.skinned()) part_skinned = true;
    }
    for (int pi : g.prims) {
      const ModelPrimitive& prim = model.primitives[pi];
      MeshData mesh;
      if (part_skinned && prim.mesh.skinned()) {
        mesh = prim.mesh;
      } else {
        bake_prim(model, prim, &mesh);
        const std::size_t before = mesh.indices.size() / 3;
        simplify_part(&mesh, kSplitPartTriangleLimit);
        if (mesh.indices.size() / 3 < before) simplified = true;
      }
      const Aabb b = mesh_bounds(mesh);
      if (b.valid()) {
        acc += b.center();
        ++n;
      }
      meshes.push_back(std::move(mesh));
      MaterialDesc md = (prim.material >= 0 && static_cast<std::size_t>(prim.material) < model.materials.size())
                            ? model.materials[prim.material]
                            : MaterialDesc{};
      if (md.name.empty()) md.name = g.name;
      mats.push_back(std::move(md));
    }
    const Vec3 origin = n ? acc * (1.0f / static_cast<float>(n)) : Vec3{};
    // A skinned part stays in bind space with the shared skeleton. Shifting
    // it to its own pivot would pull it off the bones.
    if (opt.recentre && !part_skinned) {
      for (MeshData& m : meshes) shift(&m, origin * -1.0f);
    }
    const std::string file = unique_file(dest_dir, g.name);
    std::vector<std::string> wrote;
    const Skeleton* skel = part_skinned ? &model.skeleton : nullptr;
    if (!write_part_gltf(file, g.name, meshes, mats, dest_dir, &wrote, &shared_tex, skel, error)) return false;
    if (written_abs) written_abs->insert(written_abs->end(), wrote.begin(), wrote.end());
    SplitPart part;
    part.name = g.name;
    part.node = g.key.empty() ? g.name : g.key;
    part.origin = part_skinned ? Vec3{} : origin;
    part.rel_gltf = fs::u8path(file).filename().string();
    parts->push_back(std::move(part));
  }
  (void)simplified;
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
