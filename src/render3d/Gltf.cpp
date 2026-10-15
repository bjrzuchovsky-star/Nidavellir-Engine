#include "render3d/Gltf.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "render3d/Json.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;

int Model::find_clip(const std::string& name) const {
  for (std::size_t i = 0; i < clips.size(); ++i) {
    if (clips[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

std::size_t Model::triangles() const {
  std::size_t n = 0;
  for (const ModelPrimitive& p : primitives) n += p.mesh.indices.size() / 3;
  return n;
}

bool load_image_memory(const unsigned char* data, std::size_t size, Image* out,
                       std::string* error) {
  int w = 0, h = 0, comp = 0;
  unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
  if (!px) {
    if (error) *error = std::string("image decode failed: ") + stbi_failure_reason();
    return false;
  }
  out->width = w;
  out->height = h;
  out->rgba.assign(px, px + static_cast<std::size_t>(w) * h * 4);
  stbi_image_free(px);
  return true;
}

bool load_image_file(const std::string& path, Image* out, std::string* error) {
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "Could not open " + path;
    return false;
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    if (error) *error = "Empty image " + path;
    return false;
  }
  std::string why;
  if (!load_image_memory(bytes.data(), bytes.size(), out, &why)) {
    if (error) *error = path + ": " + why;
    return false;
  }
  return true;
}

namespace {

const char* result_text(cgltf_result r) {
  switch (r) {
    case cgltf_result_success: return "ok";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
    default: return "error";
  }
}

class ImageCache {
 public:
  ImageCache(const cgltf_data* data, fs::path base) : data_(data), base_(std::move(base)) {}

  std::shared_ptr<const Image> get(const cgltf_texture* tex, std::string* warn) {
    if (!tex || !tex->image) return nullptr;
    const cgltf_image* img = tex->image;
    auto it = cache_.find(img);
    if (it != cache_.end()) return it->second;
    auto out = std::make_shared<Image>();
    std::string why;
    bool ok = false;
    if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
      const auto* bytes = static_cast<const unsigned char*>(img->buffer_view->buffer->data) +
                          img->buffer_view->offset;
      ok = load_image_memory(bytes, img->buffer_view->size, out.get(), &why);
    } else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0) {
      const char* comma = std::strchr(img->uri, ',');
      if (comma && std::strstr(img->uri, ";base64,")) {
        const std::size_t len = std::strlen(comma + 1);
        void* decoded = nullptr;
        cgltf_options opt{};
        const cgltf_size size = len / 4 * 3 - (len >= 1 && comma[len] == '=' ? 1 : 0) -
                                (len >= 2 && comma[len - 1] == '=' ? 1 : 0);
        if (cgltf_load_buffer_base64(&opt, size, comma + 1, &decoded) == cgltf_result_success) {
          ok = load_image_memory(static_cast<unsigned char*>(decoded), size, out.get(), &why);
          std::free(decoded);
        } else {
          why = "bad base64 image";
        }
      } else {
        why = "unsupported data URI";
      }
    } else if (img->uri) {
      std::string uri = img->uri;
      cgltf_decode_uri(uri.data());
      uri.resize(std::strlen(uri.c_str()));
      ok = load_image_file((base_ / fs::path(uri)).string(), out.get(), &why);
    } else {
      why = "image has no data";
    }
    if (!ok) {
      if (warn && warn->empty()) *warn = why;
      out.reset();
    } else {
      ++decoded_;
    }
    cache_[img] = out;
    (void)data_;
    return out;
  }
  std::size_t decoded() const { return decoded_; }

 private:
  const cgltf_data* data_;
  fs::path base_;
  std::map<const cgltf_image*, std::shared_ptr<const Image>> cache_;
  std::size_t decoded_ = 0;
};

void decompose(const Mat4& m, Vec3* t, Quat* r, Vec3* s) {
  *t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
  Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)}, c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)},
      c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
  *s = {length(c0), length(c1), length(c2)};
  if (s->x > 0) c0 = c0 / s->x;
  if (s->y > 0) c1 = c1 / s->y;
  if (s->z > 0) c2 = c2 / s->z;
  const float tr = c0.x + c1.y + c2.z;
  Quat q;
  if (tr > 0.0f) {
    const float k = std::sqrt(tr + 1.0f) * 2.0f;
    q = {(c1.z - c2.y) / k, (c2.x - c0.z) / k, (c0.y - c1.x) / k, 0.25f * k};
  } else if (c0.x > c1.y && c0.x > c2.z) {
    const float k = std::sqrt(1.0f + c0.x - c1.y - c2.z) * 2.0f;
    q = {0.25f * k, (c1.x + c0.y) / k, (c2.x + c0.z) / k, (c1.z - c2.y) / k};
  } else if (c1.y > c2.z) {
    const float k = std::sqrt(1.0f + c1.y - c0.x - c2.z) * 2.0f;
    q = {(c1.x + c0.y) / k, 0.25f * k, (c2.y + c1.z) / k, (c2.x - c0.z) / k};
  } else {
    const float k = std::sqrt(1.0f + c2.z - c0.x - c1.y) * 2.0f;
    q = {(c2.x + c0.z) / k, (c2.y + c1.z) / k, 0.25f * k, (c0.y - c1.x) / k};
  }
  *r = normalize(q);
}

MaterialDesc convert_material(const cgltf_material& m, ImageCache& images,
                              std::string* warn) {
  MaterialDesc d;
  d.name = m.name ? m.name : "material";
  if (m.has_pbr_metallic_roughness) {
    const cgltf_pbr_metallic_roughness& p = m.pbr_metallic_roughness;
    for (int i = 0; i < 4; ++i) d.base_color[i] = p.base_color_factor[i];
    d.metallic = p.metallic_factor;
    d.roughness = p.roughness_factor;
    d.base_color_image = images.get(p.base_color_texture.texture, warn);
    d.metal_rough_image = images.get(p.metallic_roughness_texture.texture, warn);
  }
  d.normal_image = images.get(m.normal_texture.texture, warn);
  for (int i = 0; i < 3; ++i) d.emissive[i] = m.emissive_factor[i];
  d.double_sided = m.double_sided != 0;
  return d;
}

}  // namespace

bool load_gltf(const std::string& path, Model* out, std::string* error) {
  cgltf_options options{};
  cgltf_data* data = nullptr;
  const std::string p8 = path;
  cgltf_result r = cgltf_parse_file(&options, p8.c_str(), &data);
  if (r != cgltf_result_success) {
    if (error) *error = path + ": " + result_text(r);
    return false;
  }
  std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> guard(data, cgltf_free);
  r = cgltf_load_buffers(&options, data, p8.c_str());
  if (r != cgltf_result_success) {
    if (error) *error = path + ": buffers: " + result_text(r);
    return false;
  }
  r = cgltf_validate(data);
  if (r != cgltf_result_success) {
    if (error) *error = path + ": " + result_text(r);
    return false;
  }

  Model model;
  model.source = path;
  ImageCache images(data, fs::path(path).parent_path());
  std::string warn;
  for (cgltf_size i = 0; i < data->materials_count; ++i) {
    model.materials.push_back(convert_material(data->materials[i], images, &warn));
  }

  // ---- Skin (the first one) -------------------------------------------------
  const cgltf_skin* skin = data->skins_count > 0 ? &data->skins[0] : nullptr;
  std::map<const cgltf_node*, int> joint_of;  // node -> skeleton index
  std::vector<int> remap;                     // skin joint index -> skeleton index
  if (skin && skin->joints_count > static_cast<cgltf_size>(kMaxBones)) {
    if (warn.empty()) {
      warn = "skin has " + std::to_string(skin->joints_count) + " joints (max " +
             std::to_string(kMaxBones) + "); drawn unskinned";
    }
    skin = nullptr;
  }
  if (skin && skin->joints_count > 0) {
    std::map<const cgltf_node*, cgltf_size> in_skin;
    for (cgltf_size j = 0; j < skin->joints_count; ++j) in_skin[skin->joints[j]] = j;
    auto joint_parent = [&](const cgltf_node* n) -> const cgltf_node* {
      for (const cgltf_node* p = n->parent; p; p = p->parent) {
        if (in_skin.count(p)) return p;
      }
      return nullptr;
    };
    auto depth = [&](const cgltf_node* n) {
      int d = 0;
      for (const cgltf_node* p = joint_parent(n); p; p = joint_parent(p)) ++d;
      return d;
    };
    std::vector<cgltf_size> order(skin->joints_count);
    for (cgltf_size j = 0; j < order.size(); ++j) order[j] = j;
    std::stable_sort(order.begin(), order.end(), [&](cgltf_size a, cgltf_size b) {
      return depth(skin->joints[a]) < depth(skin->joints[b]);
    });
    remap.assign(skin->joints_count, 0);
    for (cgltf_size k = 0; k < order.size(); ++k) {
      remap[order[k]] = static_cast<int>(k);
      joint_of[skin->joints[order[k]]] = static_cast<int>(k);
    }
    model.skeleton.joints.resize(order.size());
    for (cgltf_size k = 0; k < order.size(); ++k) {
      const cgltf_node* n = skin->joints[order[k]];
      Joint& j = model.skeleton.joints[k];
      j.name = n->name ? n->name : ("joint " + std::to_string(k));
      const cgltf_node* jp = joint_parent(n);
      j.parent = jp ? joint_of[jp] : -1;
      if (!jp && n->parent) cgltf_node_transform_world(n->parent, j.base.m);
      if (n->has_matrix) {
        Mat4 m;
        std::memcpy(m.m, n->matrix, sizeof(m.m));
        decompose(m, &j.t, &j.r, &j.s);
      } else {
        if (n->has_translation) j.t = {n->translation[0], n->translation[1], n->translation[2]};
        if (n->has_rotation) j.r = normalize(Quat{n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3]});
        if (n->has_scale) j.s = {n->scale[0], n->scale[1], n->scale[2]};
      }
      if (skin->inverse_bind_matrices) {
        cgltf_accessor_read_float(skin->inverse_bind_matrices, order[k], j.inverse_bind.m, 16);
      }
    }
    // Clips: channels that drive the skin's joints (T / R / S).
    for (cgltf_size a = 0; a < data->animations_count; ++a) {
      const cgltf_animation& an = data->animations[a];
      Clip clip;
      clip.name = an.name && *an.name ? an.name : ("clip " + std::to_string(a + 1));
      for (cgltf_size c = 0; c < an.channels_count; ++c) {
        const cgltf_animation_channel& ac = an.channels[c];
        auto jt = joint_of.find(ac.target_node);
        if (jt == joint_of.end() || !ac.sampler || !ac.sampler->input || !ac.sampler->output) continue;
        Channel ch;
        ch.joint = jt->second;
        if (ac.target_path == cgltf_animation_path_type_translation) ch.path = ChannelPath::Translation;
        else if (ac.target_path == cgltf_animation_path_type_rotation) ch.path = ChannelPath::Rotation;
        else if (ac.target_path == cgltf_animation_path_type_scale) ch.path = ChannelPath::Scale;
        else continue;
        ch.interp = ac.sampler->interpolation == cgltf_interpolation_type_step ? Interp::Step
                    : ac.sampler->interpolation == cgltf_interpolation_type_cubic_spline ? Interp::CubicSpline
                                                                                         : Interp::Linear;
        const cgltf_accessor* in = ac.sampler->input;
        const cgltf_accessor* outp = ac.sampler->output;
        ch.times.resize(in->count);
        for (cgltf_size k = 0; k < in->count; ++k) cgltf_accessor_read_float(in, k, &ch.times[k], 1);
        const int w = ch.width();
        ch.values.resize(outp->count * static_cast<cgltf_size>(w));
        for (cgltf_size k = 0; k < outp->count; ++k) {
          cgltf_accessor_read_float(outp, k, &ch.values[k * static_cast<cgltf_size>(w)], static_cast<cgltf_size>(w));
        }
        const std::size_t need = ch.times.size() * static_cast<std::size_t>(w) *
                                 (ch.interp == Interp::CubicSpline ? 3 : 1);
        if (ch.times.empty() || ch.values.size() < need) continue;
        clip.duration = std::max(clip.duration, ch.times.back());
        clip.channels.push_back(std::move(ch));
      }
      if (!clip.channels.empty()) model.clips.push_back(std::move(clip));
    }
  }

  auto add_mesh = [&](const cgltf_mesh& mesh, const Mat4& world, bool skinned) -> bool {
    Mat4 normal_m = world.inverse();
    // transpose of the inverse (upper 3x3) for normals
    Mat4 nt;
    for (int r0 = 0; r0 < 3; ++r0)
      for (int c0 = 0; c0 < 3; ++c0) nt.at(r0, c0) = normal_m.at(c0, r0);
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
      const cgltf_primitive& prim = mesh.primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles) continue;
      const cgltf_accessor* pos = nullptr;
      const cgltf_accessor* nrm = nullptr;
      const cgltf_accessor* uv = nullptr;
      const cgltf_accessor* tan = nullptr;
      const cgltf_accessor* joints = nullptr;
      const cgltf_accessor* weights = nullptr;
      for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const cgltf_attribute& at = prim.attributes[a];
        if (at.type == cgltf_attribute_type_position) pos = at.data;
        else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
        else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
        else if (at.type == cgltf_attribute_type_tangent) tan = at.data;
        else if (at.type == cgltf_attribute_type_joints && at.index == 0) joints = at.data;
        else if (at.type == cgltf_attribute_type_weights && at.index == 0) weights = at.data;
      }
      if (!pos) continue;
      const bool skin_this = skinned && joints && weights;
      ModelPrimitive mp;
      mp.mesh.name = mesh.name ? mesh.name : "mesh";
      const cgltf_size n = pos->count;
      mp.mesh.vertices.resize(n);
      if (skin_this) mp.mesh.skin.resize(n);
      for (cgltf_size v = 0; v < n; ++v) {
        Vertex3D& vx = mp.mesh.vertices[v];
        float f[4] = {0, 0, 0, 1};
        cgltf_accessor_read_float(pos, v, f, 3);
        vx.position = world.transform_point({f[0], f[1], f[2]});
        if (nrm && cgltf_accessor_read_float(nrm, v, f, 3)) {
          vx.normal = normalize(nt.transform_dir({f[0], f[1], f[2]}));
        }
        if (uv && cgltf_accessor_read_float(uv, v, f, 2)) {
          vx.u = f[0];
          vx.v = f[1];
        }
        if (tan && cgltf_accessor_read_float(tan, v, f, 4)) {
          const Vec3 t = normalize(world.transform_dir({f[0], f[1], f[2]}));
          vx.tangent = {t.x, t.y, t.z, f[3] < 0 ? -1.0f : 1.0f};
        }
        if (skin_this) {
          SkinVertex& sv = mp.mesh.skin[v];
          float jw[4] = {0, 0, 0, 0}, ww[4] = {0, 0, 0, 0};
          cgltf_accessor_read_float(joints, v, jw, 4);
          cgltf_accessor_read_float(weights, v, ww, 4);
          float total = 0.0f;
          for (int k = 0; k < 4; ++k) {
            const int j = static_cast<int>(jw[k] + 0.5f);
            sv.joints[k] = (j >= 0 && static_cast<std::size_t>(j) < remap.size())
                               ? static_cast<float>(remap[static_cast<std::size_t>(j)])
                               : 0.0f;
            sv.weights[k] = std::max(ww[k], 0.0f);
            total += sv.weights[k];
          }
          if (total > 0.0f) {
            for (float& wk : sv.weights) wk /= total;
          } else {
            sv.weights[0] = 1.0f;
          }
        }
      }
      if (prim.indices) {
        mp.mesh.indices.resize(prim.indices->count);
        for (cgltf_size k = 0; k < prim.indices->count; ++k) {
          mp.mesh.indices[k] = static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, k));
        }
      } else {
        mp.mesh.indices.resize(n);
        for (cgltf_size k = 0; k < n; ++k) mp.mesh.indices[k] = static_cast<std::uint32_t>(k);
      }
      // Flat normals when the file has none.
      if (!nrm) {
        for (std::size_t k = 0; k + 2 < mp.mesh.indices.size(); k += 3) {
          Vertex3D& a = mp.mesh.vertices[mp.mesh.indices[k]];
          Vertex3D& b = mp.mesh.vertices[mp.mesh.indices[k + 1]];
          Vertex3D& c = mp.mesh.vertices[mp.mesh.indices[k + 2]];
          const Vec3 fn = normalize(cross(b.position - a.position, c.position - a.position));
          a.normal += fn;
          b.normal += fn;
          c.normal += fn;
        }
        for (Vertex3D& v : mp.mesh.vertices) v.normal = normalize(v.normal);
      }
      // A mirrored node flips the winding.
      const float det = world.at(0, 0) * (world.at(1, 1) * world.at(2, 2) - world.at(1, 2) * world.at(2, 1)) -
                        world.at(0, 1) * (world.at(1, 0) * world.at(2, 2) - world.at(1, 2) * world.at(2, 0)) +
                        world.at(0, 2) * (world.at(1, 0) * world.at(2, 1) - world.at(1, 1) * world.at(2, 0));
      if (det < 0.0f) {
        for (std::size_t k = 0; k + 2 < mp.mesh.indices.size(); k += 3) {
          std::swap(mp.mesh.indices[k + 1], mp.mesh.indices[k + 2]);
        }
      }
      if (!tan) compute_tangents(mp.mesh);
      if (!mp.mesh.valid()) {
        if (error) *error = path + ": primitive with bad indices in mesh " + mp.mesh.name;
        return false;
      }
      mp.material = prim.material ? static_cast<int>(prim.material - data->materials) : -1;
      model.primitives.push_back(std::move(mp));
    }
    return true;
  };

  bool any_node = false;
  for (cgltf_size i = 0; i < data->nodes_count; ++i) {
    const cgltf_node& node = data->nodes[i];
    if (!node.mesh) continue;
    any_node = true;
    // Skinned meshes stay in bind space: the joints place them (glTF
    // ignores the skinned node's own transform).
    const bool skinned = skin && node.skin == skin && !model.skeleton.empty();
    Mat4 world;
    if (!skinned) cgltf_node_transform_world(&node, world.m);
    if (!add_mesh(*node.mesh, world, skinned)) return false;
  }
  if (!any_node) {
    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
      if (!add_mesh(data->meshes[i], Mat4{}, false)) return false;
    }
  }
  bool any_skinned = false;
  for (const ModelPrimitive& p : model.primitives) any_skinned = any_skinned || p.mesh.skinned();
  if (!any_skinned) {
    model.skeleton = {};
    model.clips.clear();
  }
  if (model.primitives.empty()) {
    if (error) *error = path + ": no triangle meshes";
    return false;
  }
  model.textures = images.decoded();
  Aabb b{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
  std::vector<Mat4> rest;
  if (model.skinned()) skin_palette(model.skeleton, rest_pose(model.skeleton), &rest);
  for (const ModelPrimitive& p : model.primitives) {
    if (p.mesh.skinned()) {
      // Bounds of the rest pose.
      for (std::size_t v = 0; v < p.mesh.vertices.size(); ++v) {
        const Vec3 q = skin_point(rest, p.mesh.skin[v].joints, p.mesh.skin[v].weights,
                                  p.mesh.vertices[v].position);
        b.min = vmin(b.min, q);
        b.max = vmax(b.max, q);
      }
      continue;
    }
    const Aabb pb = p.mesh.bounds();
    b.min = vmin(b.min, pb.min);
    b.max = vmax(b.max, pb.max);
  }
  model.bounds = b;
  *out = std::move(model);
  if (error && !warn.empty()) *error = "warning: " + warn;
  return true;
}

namespace {

std::string base64(const std::string& in) {
  // Alphabet built at run time: A-Z a-z 0-9 + /.
  static const std::string k = [] {
    std::string s;
    for (char c = 'A'; c <= 'Z'; ++c) s += c;
    for (char c = 'a'; c <= 'z'; ++c) s += c;
    for (char c = '0'; c <= '9'; ++c) s += c;
    return s + "+/";
  }();
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

void append(std::string& buf, const void* p, std::size_t n) {
  buf.append(static_cast<const char*>(p), n);
}

void pad4(std::string& buf) {
  while (buf.size() % 4) buf.push_back('\0');
}

}  // namespace

bool write_gltf_embedded(const std::string& path,
                         const std::vector<GltfPart>& parts,
                         std::string* error) {
  using json::Value;
  std::string bin;
  Value accessors = Value::array(), views = Value::array(), meshes = Value::array(),
        nodes = Value::array(), materials = Value::array(), scene_nodes = Value::array();
  auto add_view = [&](std::size_t offset, std::size_t len, int target) {
    Value v = Value::object();
    v.set("buffer", Value::number(0));
    v.set("byteOffset", Value::number(static_cast<double>(offset)));
    v.set("byteLength", Value::number(static_cast<double>(len)));
    v.set("target", Value::number(target));
    views.push(std::move(v));
    return static_cast<double>(views.size() - 1);
  };
  auto add_accessor = [&](double view, int comp, std::size_t count, const char* type,
                          const float* mn, const float* mx) {
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
  for (std::size_t pi = 0; pi < parts.size(); ++pi) {
    const MeshData& m = parts[pi].mesh;
    if (!m.valid()) {
      if (error) *error = "write_gltf_embedded: invalid mesh " + m.name;
      return false;
    }
    const Aabb b = m.bounds();
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
    const double idx = add_accessor(add_view(off, m.indices.size() * 4, 34963), 5125,
                                    m.indices.size(), "SCALAR", nullptr, nullptr);
    pad4(bin);

    const MaterialDesc& md = parts[pi].material;
    Value mat = Value::object();
    mat.set("name", Value::string(md.name.empty() ? "part" : md.name));
    Value pbr = Value::object();
    pbr.set("baseColorFactor", json::floats(md.base_color, 4));
    pbr.set("metallicFactor", Value::number(md.metallic));
    pbr.set("roughnessFactor", Value::number(md.roughness));
    mat.set("pbrMetallicRoughness", std::move(pbr));
    if (md.emissive[0] > 0 || md.emissive[1] > 0 || md.emissive[2] > 0) {
      mat.set("emissiveFactor", json::floats(md.emissive, 3));
    }
    materials.push(std::move(mat));

    Value attrs = Value::object();
    attrs.set("POSITION", Value::number(pos));
    attrs.set("NORMAL", Value::number(nrm));
    attrs.set("TEXCOORD_0", Value::number(tex));
    Value prim = Value::object();
    prim.set("attributes", std::move(attrs));
    prim.set("indices", Value::number(idx));
    prim.set("material", Value::number(static_cast<double>(pi)));
    Value prims = Value::array();
    prims.push(std::move(prim));
    Value mesh = Value::object();
    mesh.set("name", Value::string(m.name.empty() ? "part" : m.name));
    mesh.set("primitives", std::move(prims));
    meshes.push(std::move(mesh));
    Value node = Value::object();
    node.set("name", Value::string(m.name.empty() ? "part" : m.name));
    node.set("mesh", Value::number(static_cast<double>(pi)));
    nodes.push(std::move(node));
    scene_nodes.push(Value::number(static_cast<double>(pi)));
  }
  Value root = Value::object();
  Value asset = Value::object();
  asset.set("version", Value::string("2.0"));
  asset.set("generator", Value::string("TombStone Engine"));
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
  root.set("accessors", std::move(accessors));
  root.set("bufferViews", std::move(views));
  Value buffer = Value::object();
  buffer.set("byteLength", Value::number(static_cast<double>(bin.size())));
  buffer.set("uri", Value::string("data:application/octet-stream;base64," + base64(bin)));
  Value buffers = Value::array();
  buffers.push(std::move(buffer));
  root.set("buffers", std::move(buffers));

  std::error_code ec;
  const fs::path p = fs::path(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "Could not write " + path;
    return false;
  }
  out << json::write(root);
  return static_cast<bool>(out);
}

bool write_sample_lantern_gltf(const std::string& path, std::string* error) {
  auto box = [](const char* name, Vec3 center, Vec3 size) {
    MeshData m = make_cube(1.0f);
    m.name = name;
    for (Vertex3D& v : m.vertices) v.position = mul(v.position, size) + center;
    return m;
  };
  std::vector<GltfPart> parts(3);
  parts[0].mesh = box("post", {0.0f, 1.2f, 0.0f}, {0.14f, 2.4f, 0.14f});
  parts[0].material.name = "iron";
  const float iron[4] = {0.18f, 0.17f, 0.16f, 1.0f};
  std::memcpy(parts[0].material.base_color, iron, sizeof(iron));
  parts[0].material.metallic = 0.85f;
  parts[0].material.roughness = 0.45f;
  parts[1].mesh = box("arm", {-0.25f, 2.35f, 0.0f}, {0.5f, 0.08f, 0.08f});
  parts[1].material = parts[0].material;
  parts[2].mesh = box("glass", {-0.45f, 2.05f, 0.0f}, {0.28f, 0.4f, 0.28f});
  parts[2].material.name = "lamp glass";
  const float glass[4] = {1.0f, 0.78f, 0.38f, 1.0f};
  std::memcpy(parts[2].material.base_color, glass, sizeof(glass));
  parts[2].material.roughness = 0.2f;
  parts[2].material.emissive[0] = 1.0f;
  parts[2].material.emissive[1] = 0.62f;
  parts[2].material.emissive[2] = 0.22f;
  return write_gltf_embedded(path, parts, error);
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
