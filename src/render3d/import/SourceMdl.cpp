// Source engine model import (.mdl + .vvd + .vtx + optional .phy), MDL
// versions 44 - 49 (Half-Life 2, Garry's Mod, TF2, Portal...).
//
// Clean-room implementation written from the public format descriptions
// (Valve Developer Community wiki pages for MDL / VVD / VTX / PHY / VTF and
// the structure layouts of the Source SDK's public studio.h / optimize.h,
// used as layout references only). No GPL or third-party code.
//
//   .mdl   header, bones (rest pose + poseToBone), textures + cd paths,
//          skin families, body parts -> models -> meshes, sequences and
//          animations (RLE value tracks, Quaternion48 / 64, Vector48,
//          sections)
//   .vvd   vertices (position, normal, uv, three bone weights) + tangents,
//          with the LOD fixup table
//   .vtx   strip groups per body part / model / LOD / mesh -> triangles
//   .phy   IVP compact surfaces -> one convex hull per compact ledge
//
// Everything is read in Source space (Z up, inches); load_model() bakes
// the unit scale and the Z -> Y up conversion afterwards.
#include "render3d/ModelImport.h"
#include "render3d/import/SourceFormats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;

namespace {

constexpr float kInchesPerMetre = 1.0f / 0.0254f;

struct Bytes {
  std::vector<unsigned char> d;
  bool in(std::int64_t off, std::int64_t len) const {
    return off >= 0 && len >= 0 && off + len <= static_cast<std::int64_t>(d.size());
  }
  std::int32_t i32(std::int64_t o) const {
    if (!in(o, 4)) return 0;
    std::int32_t v;
    std::memcpy(&v, &d[o], 4);
    return v;
  }
  std::uint32_t u32(std::int64_t o) const { return static_cast<std::uint32_t>(i32(o)); }
  std::int16_t i16(std::int64_t o) const {
    if (!in(o, 2)) return 0;
    std::int16_t v;
    std::memcpy(&v, &d[o], 2);
    return v;
  }
  std::uint16_t u16(std::int64_t o) const { return static_cast<std::uint16_t>(i16(o)); }
  std::uint8_t u8(std::int64_t o) const { return in(o, 1) ? d[o] : 0; }
  float f32(std::int64_t o) const {
    if (!in(o, 4)) return 0.0f;
    float v;
    std::memcpy(&v, &d[o], 4);
    return std::isfinite(v) ? v : 0.0f;
  }
  Vec3 vec3(std::int64_t o) const { return {f32(o), f32(o + 4), f32(o + 8)}; }
  std::string str(std::int64_t o, std::size_t max = 260) const {
    std::string s;
    while (in(o, 1) && d[o] && s.size() < max) s += static_cast<char>(d[o++]);
    return s;
  }
};

bool read_file(const fs::path& p, Bytes* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out->d.empty();
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string clean(std::string s) {
  std::replace(s.begin(), s.end(), '\\', '/');
  while (!s.empty() && s.front() == '/') s.erase(s.begin());
  return s;
}

// Exact path, else a case-insensitive walk (Source content is mixed case
// and case-insensitive on Windows).
bool find_ci(const fs::path& root, const std::string& rel, fs::path* out) {
  std::error_code ec;
  const fs::path direct = root / fs::u8path(rel);
  if (fs::is_regular_file(direct, ec)) {
    *out = direct;
    return true;
  }
  fs::path cur = root;
  std::stringstream ss(rel);
  std::string part;
  std::vector<std::string> parts;
  while (std::getline(ss, part, '/')) {
    if (!part.empty()) parts.push_back(part);
  }
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const fs::path next = cur / fs::u8path(parts[i]);
    if (fs::exists(next, ec)) {
      cur = next;
      continue;
    }
    if (!fs::is_directory(cur, ec)) return false;
    bool found = false;
    const std::string want = lower(parts[i]);
    for (const auto& it : fs::directory_iterator(cur, ec)) {
      if (lower(it.path().filename().string()) == want) {
        cur = it.path();
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  if (!fs::is_regular_file(cur, ec)) return false;
  *out = cur;
  return true;
}

// ---- half floats, compressed quaternions ---------------------------------

float half(std::uint16_t h) {
  const int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31) v = 0.0f;  // inf / nan: treat as zero
  else v = std::ldexp(static_cast<float>(m + 1024), e - 25);
  return s ? -v : v;
}

Quat quat48(const Bytes& b, std::int64_t o) {
  const std::uint16_t a = b.u16(o), c = b.u16(o + 2), e = b.u16(o + 4);
  Quat q;
  q.x = (static_cast<int>(a) - 32768) * (1.0f / 32768.0f);
  q.y = (static_cast<int>(c) - 32768) * (1.0f / 32768.0f);
  q.z = (static_cast<int>(e & 0x7FFF) - 16384) * (1.0f / 16384.0f);
  q.w = std::sqrt(std::max(0.0f, 1.0f - q.x * q.x - q.y * q.y - q.z * q.z));
  if (e & 0x8000) q.w = -q.w;
  return q;
}

Quat quat64(const Bytes& b, std::int64_t o) {
  std::uint64_t v = 0;
  for (int k = 0; k < 8; ++k) v |= std::uint64_t(b.u8(o + k)) << (8 * k);
  Quat q;
  q.x = (static_cast<int>(v & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
  q.y = (static_cast<int>((v >> 21) & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
  q.z = (static_cast<int>((v >> 42) & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
  q.w = std::sqrt(std::max(0.0f, 1.0f - q.x * q.x - q.y * q.y - q.z * q.z));
  if (v >> 63) q.w = -q.w;
  return q;
}

// Source RadianEuler (x roll, y pitch, z yaw): q = qz * qy * qx.
Quat euler_quat(const Vec3& e) {
  return normalize(Quat::axis_angle({0, 0, 1}, e.z) * Quat::axis_angle({0, 1, 0}, e.y) *
                   Quat::axis_angle({1, 0, 0}, e.x));
}

// One frame of an RLE animation value track (mstudioanimvalue_t runs:
// {valid, total} header then `valid` shorts; the last one repeats).
float anim_value(const Bytes& b, std::int64_t p, int frame) {
  int k = frame;
  for (int guard = 0; guard < 4096; ++guard) {
    if (!b.in(p, 2)) return 0.0f;
    const int valid = b.u8(p), total = b.u8(p + 1);
    if (total == 0) return 0.0f;
    if (total > k) {
      const int idx = valid > k ? k + 1 : valid;
      return static_cast<float>(b.i16(p + 2 * idx));
    }
    k -= total;
    p += 2 * (valid + 1);
  }
  return 0.0f;
}

// ---- mdl structures (offsets in studiohdr_t etc.) ------------------------

struct SrcBone {
  std::string name;
  int parent = -1;
  Vec3 pos;
  Quat quat;
  Vec3 rot;  // RadianEuler
  Vec3 posscale, rotscale;
  Mat4 pose_to_bone;
};

struct SrcVert {
  Vec3 pos, normal;
  float u = 0, v = 0;
  float w[3] = {1, 0, 0};
  int bone[3] = {0, 0, 0};
  int nb = 1;
  Vec4 tangent{1, 0, 0, 1};
};

struct VtxLayout {
  int strip_group = 25;  // StripGroupHeader_t bytes (33 with topology)
  int strip = 27;        // StripHeader_t bytes (35 with topology)
};

// ---- materials -------------------------------------------------------------

struct MaterialFinder {
  std::vector<fs::path> roots;  // content roots (holding materials/) or materials dirs
  fs::path model_dir;
  std::map<std::string, std::shared_ptr<const Image>> cache;

  bool find(const std::string& rel_in, const char* ext, fs::path* out) const {
    const std::string rel = clean(rel_in) + ext;
    for (const fs::path& r : roots) {
      if (find_ci(r / "materials", rel, out) || find_ci(r, rel, out)) return true;
    }
    return false;
  }
  bool find_flat(const std::string& name, const char* ext, fs::path* out) const {
    const std::string base = fs::u8path(clean(name)).filename().string() + ext;
    return find_ci(model_dir, base, out) || find_ci(model_dir / "materials", base, out);
  }
  std::shared_ptr<const Image> vtf(const fs::path& p, std::string* warn) {
    const std::string key = p.string();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Bytes b;
    std::shared_ptr<const Image> img;
    if (read_file(p, &b)) {
      auto im = std::make_shared<Image>();
      std::string err;
      if (source::decode_vtf(b.d.data(), b.d.size(), im.get(), &err)) {
        img = std::move(im);
      } else if (warn) {
        *warn += (warn->empty() ? "" : "; ") + p.filename().string() + ": " + err;
      }
    }
    cache[key] = img;
    return img;
  }
  bool read_vmt(const fs::path& p, source::Vmt* out) const {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!source::parse_vmt(ss.str(), out)) return false;
    if (!out->include.empty()) {  // patch material: merge its include
      std::string inc = clean(out->include);
      if (lower(inc).rfind("materials/", 0) == 0) inc = inc.substr(10);
      if (inc.size() > 4 && lower(inc.substr(inc.size() - 4)) == ".vmt") inc.resize(inc.size() - 4);
      fs::path ip;
      source::Vmt base;
      if (find(inc, ".vmt", &ip) && read_vmt_plain(ip, &base)) {
        for (const auto& [k, v] : base.params) out->params.emplace(k, v);
        if (out->shader == "patch") out->shader = base.shader;
      }
    }
    return true;
  }
  static bool read_vmt_plain(const fs::path& p, source::Vmt* out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return source::parse_vmt(ss.str(), out);
  }
  std::shared_ptr<const Image> texture(const std::string& name, const fs::path& near_dir,
                                       std::string* warn) {
    fs::path p;
    if (find(name, ".vtf", &p) || find_ci(near_dir, fs::u8path(clean(name)).filename().string() + ".vtf", &p) ||
        find_flat(name, ".vtf", &p)) {
      return vtf(p, warn);
    }
    return nullptr;
  }
};

void parse_color(const std::string& s, float* rgb) {
  std::string t;
  for (char c : s) t += (c == '[' || c == ']' || c == '{' || c == '}') ? ' ' : c;
  std::istringstream in(t);
  float v[3];
  if (in >> v[0] >> v[1] >> v[2]) {
    const bool bytes = s.find('{') != std::string::npos || v[0] > 1.0f || v[1] > 1.0f || v[2] > 1.0f;
    for (int k = 0; k < 3; ++k) rgb[k] = bytes ? v[k] / 255.0f : v[k];
  }
}

MaterialDesc source_material(MaterialFinder& mf, const std::string& tex,
                             const std::vector<std::string>& cd_paths, std::string* missing,
                             std::string* warn) {
  MaterialDesc m;
  m.name = tex;
  m.base_color[0] = m.base_color[1] = m.base_color[2] = m.base_color[3] = 1.0f;
  m.roughness = 0.75f;
  std::vector<std::string> names;
  for (const std::string& cd : cd_paths) {
    std::string c = clean(cd);
    if (!c.empty() && c.back() != '/') c += '/';
    names.push_back(c + clean(tex));
  }
  names.push_back(clean(tex));
  fs::path vmt_path;
  bool have_vmt = false;
  for (const std::string& n : names) {
    if (mf.find(n, ".vmt", &vmt_path)) {
      have_vmt = true;
      break;
    }
  }
  if (!have_vmt) have_vmt = mf.find_flat(tex, ".vmt", &vmt_path);
  std::shared_ptr<const Image> base;
  if (have_vmt) {
    source::Vmt vmt;
    if (mf.read_vmt(vmt_path, &vmt)) {
      const std::string bt = vmt.param("$basetexture");
      if (!bt.empty()) base = mf.texture(bt, vmt_path.parent_path(), warn);
      const std::string bump = vmt.param("$bumpmap").empty() ? vmt.param("$normalmap") : vmt.param("$bumpmap");
      if (!bump.empty()) {
        if (auto n = mf.texture(bump, vmt_path.parent_path(), warn)) {
          // Source normal maps are Y-down (DirectX); the renderer wants Y-up.
          auto flipped = std::make_shared<Image>(*n);
          for (std::size_t i = 1; i < flipped->rgba.size(); i += 4) flipped->rgba[i] = static_cast<unsigned char>(255 - flipped->rgba[i]);
          m.normal_image = std::move(flipped);
        }
      }
      if (vmt.param("$nocull") == "1") m.double_sided = true;
      if (!vmt.param("$color2").empty()) parse_color(vmt.param("$color2"), m.base_color);
      else if (!vmt.param("$color").empty()) parse_color(vmt.param("$color"), m.base_color);
      if (vmt.shader == "unlitgeneric") m.unlit = true;
      if (!vmt.param("$alpha").empty()) m.base_color[3] = std::clamp(std::strtof(vmt.param("$alpha").c_str(), nullptr), 0.0f, 1.0f);
    }
  } else {
    for (const std::string& n : names) {
      if ((base = mf.texture(n, mf.model_dir, warn))) break;
    }
  }
  if (base) {
    m.base_color_image = std::move(base);
  } else {
    m.base_color_image = missing_texture_image();
    if (missing) *missing += (missing->empty() ? "" : ", ") + tex;
  }
  return m;
}

// ---- phy -------------------------------------------------------------------

void phy_ledge_points(const Bytes& b, std::int64_t ledge, std::vector<Vec3>* out) {
  const std::int32_t point_off = b.i32(ledge);
  const int ntri = b.i16(ledge + 12);
  if (ntri <= 0 || ntri > 65535 || !b.in(ledge + 16, std::int64_t(ntri) * 16)) return;
  std::set<int> idx;
  for (int t = 0; t < ntri; ++t) {
    for (int e = 0; e < 3; ++e) idx.insert(static_cast<int>(b.u32(ledge + 16 + t * 16 + 4 + e * 4) & 0xFFFF));
  }
  for (int i : idx) {
    const std::int64_t p = ledge + point_off + std::int64_t(i) * 16;
    if (!b.in(p, 12)) continue;
    const Vec3 ivp = b.vec3(p);
    // IVP space (metres, Y down / Z forward) -> Source (inches, Z up).
    out->push_back(Vec3{ivp.x, ivp.z, -ivp.y} * kInchesPerMetre);
  }
}

struct PhySolid {
  std::vector<std::vector<Vec3>> hulls;  // bone space
  std::string bone;
};

bool parse_phy(const Bytes& b, std::vector<PhySolid>* out, std::string* warn) {
  const std::int32_t hsize = b.i32(0), count = b.i32(8);
  if (hsize < 16 || count <= 0 || count > 1024) return false;
  std::int64_t pos = hsize;
  for (int s = 0; s < count; ++s) {
    const std::int32_t size = b.i32(pos);
    const std::int64_t body = pos + 4, end = body + size;
    if (size <= 0 || !b.in(body, size)) break;
    PhySolid solid;
    std::int64_t surf = body;
    if (b.u32(body) == 0x59485056u) {  // "VPHY"
      const int model_type = b.i16(body + 6);
      if (model_type != 0) {
        if (warn) *warn += (warn->empty() ? "" : "; ") + std::string("phy solid uses an unsupported collide type");
        pos = end;
        out->push_back(std::move(solid));
        continue;
      }
      surf = body + 28;
    }
    const std::int32_t tree = b.i32(surf + 32);
    std::vector<std::int64_t> ledges;
    if (tree > 48 && surf + tree < end) {
      std::vector<std::int64_t> stack = {surf + tree};
      while (!stack.empty() && ledges.size() < 4096) {
        const std::int64_t node = stack.back();
        stack.pop_back();
        if (!b.in(node, 28)) continue;
        const std::int32_t right = b.i32(node), led = b.i32(node + 4);
        if (right == 0) {
          if (led != 0 && b.in(node + led, 16)) ledges.push_back(node + led);
        } else {
          stack.push_back(node + right);
          stack.push_back(node + 28);
        }
      }
    }
    if (ledges.empty()) {
      // No tree: ledges follow the surface header back to back.
      std::int64_t l = surf + 48;
      while (b.in(l, 16) && ledges.size() < 4096) {
        const std::uint32_t size16 = b.u32(l + 8) >> 8;
        if (size16 == 0) break;
        ledges.push_back(l);
        l += std::int64_t(size16) * 16;
        if (tree > 0 && l >= surf + tree) break;
      }
    }
    for (std::int64_t l : ledges) {
      std::vector<Vec3> pts;
      phy_ledge_points(b, l, &pts);
      if (pts.size() >= 4) solid.hulls.push_back(std::move(pts));
    }
    out->push_back(std::move(solid));
    pos = end;
  }
  // Text section: solid { "index" "0" "name" "bone" ... }.
  if (b.in(pos, 1)) {
    std::string text(reinterpret_cast<const char*>(&b.d[pos]), b.d.size() - pos);
    std::size_t at = 0;
    while ((at = text.find("solid", at)) != std::string::npos) {
      const std::size_t open = text.find('{', at), close = text.find('}', at);
      if (open == std::string::npos || close == std::string::npos || close < open) break;
      const std::string blk = text.substr(open, close - open);
      auto value = [&](const std::string& key) {
        const std::size_t k = blk.find("\"" + key + "\"");
        if (k == std::string::npos) return std::string();
        const std::size_t q1 = blk.find('"', k + key.size() + 2);
        const std::size_t q2 = q1 == std::string::npos ? q1 : blk.find('"', q1 + 1);
        return q2 == std::string::npos ? std::string() : blk.substr(q1 + 1, q2 - q1 - 1);
      };
      const std::string idx = value("index");
      const int i = idx.empty() ? -1 : std::atoi(idx.c_str());
      if (i >= 0 && i < static_cast<int>(out->size())) (*out)[i].bone = value("name");
      at = close;
    }
  }
  return true;
}

Mat4 mat3x4(const Bytes& b, std::int64_t o) {
  Mat4 m;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) m.at(r, c) = b.f32(o + (r * 4 + c) * 4);
  }
  return m;
}

bool is_zero(const Mat4& m) {
  for (int k = 0; k < 12; ++k) {
    if (m.m[k] != 0.0f) return false;
  }
  return true;
}

void append(std::string* s, const std::string& w) {
  if (!w.empty()) *s += (s->empty() ? "" : "; ") + w;
}

}  // namespace

bool load_source_mdl(const std::string& path, const ModelImportOptions& opt, Model* out,
                     std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  const fs::path mdl_path = fs::u8path(path);
  Bytes mdl;
  if (!read_file(mdl_path, &mdl)) return fail("Could not open " + path);
  if (mdl.d.size() < 408 || std::memcmp(mdl.d.data(), "IDST", 4) != 0) {
    return fail("not a Source engine model (no IDST header)");
  }
  const int version = mdl.i32(4);
  if (version < 44 || version > 49) {
    return fail("Source MDL version " + std::to_string(version) + " is not supported (44 - 49)");
  }
  const std::int32_t checksum = mdl.i32(8);
  fs::path base = mdl_path;
  base.replace_extension();
  const std::string b8 = base.string();
  Bytes vvd, vtx, phy;
  if (!read_file(fs::u8path(b8 + ".vvd"), &vvd)) return fail(mdl_path.filename().string() + ": missing " + base.filename().string() + ".vvd");
  std::string vtx_name;
  for (const char* x : {".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx"}) {
    if (read_file(fs::u8path(b8 + x), &vtx)) {
      vtx_name = base.filename().string() + x;
      break;
    }
  }
  if (vtx.d.empty()) return fail(mdl_path.filename().string() + ": missing " + base.filename().string() + ".dx90.vtx (or .dx80 / .sw)");
  if (vvd.d.size() < 64 || std::memcmp(vvd.d.data(), "IDSV", 4) != 0) return fail(".vvd is not a Source vertex file");
  if (vtx.d.size() < 36 || vtx.i32(0) != 7) return fail(vtx_name + ": unsupported VTX version " + std::to_string(vtx.i32(0)));
  std::string warn;
  if (vvd.i32(8) != checksum) append(&warn, ".vvd checksum does not match the .mdl");
  if (vtx.i32(16) != checksum) append(&warn, vtx_name + " checksum does not match the .mdl");

  // -- bones ----------------------------------------------------------------
  const int nbones = mdl.i32(156);
  const std::int64_t bone_at = mdl.i32(160);
  if (nbones <= 0 || nbones > 4096 || !mdl.in(bone_at, std::int64_t(nbones) * 216)) return fail("bad bone table");
  std::vector<SrcBone> bones(nbones);
  for (int i = 0; i < nbones; ++i) {
    const std::int64_t o = bone_at + std::int64_t(i) * 216;
    SrcBone& bn = bones[i];
    bn.name = mdl.str(o + mdl.i32(o));
    bn.parent = mdl.i32(o + 4);
    if (bn.parent >= i) bn.parent = -1;  // parents come first
    bn.pos = mdl.vec3(o + 32);
    bn.quat = normalize(Quat{mdl.f32(o + 44), mdl.f32(o + 48), mdl.f32(o + 52), mdl.f32(o + 56)});
    bn.rot = mdl.vec3(o + 60);
    bn.posscale = mdl.vec3(o + 72);
    bn.rotscale = mdl.vec3(o + 84);
    bn.pose_to_bone = mat3x4(mdl, o + 96);
  }
  // Model-space rest matrices (inverse binds when poseToBone is empty).
  std::vector<Mat4> rest(nbones);
  for (int i = 0; i < nbones; ++i) {
    const Mat4 local = Mat4::trs(bones[i].pos, bones[i].quat, {1, 1, 1});
    rest[i] = bones[i].parent >= 0 ? rest[bones[i].parent] * local : local;
  }
  if (nbones > kMaxBones) append(&warn, std::to_string(nbones) + " bones (the renderer skins " + std::to_string(kMaxBones) + ")");

  // -- textures / skins --------------------------------------------------------
  const int ntex = mdl.i32(204);
  const std::int64_t tex_at = mdl.i32(208);
  std::vector<std::string> textures;
  for (int i = 0; i < ntex && i < 1024; ++i) {
    const std::int64_t o = tex_at + std::int64_t(i) * 64;
    if (!mdl.in(o, 64)) break;
    textures.push_back(mdl.str(o + mdl.i32(o)));
  }
  std::vector<std::string> cd;
  const int ncd = mdl.i32(212);
  for (int i = 0; i < ncd && i < 64; ++i) cd.push_back(mdl.str(mdl.i32(mdl.i32(216) + i * 4)));
  const int nskinref = mdl.i32(220), nfam = std::max(1, mdl.i32(224));
  const std::int64_t skin_at = mdl.i32(228);
  const int skin = std::clamp(opt.skin, 0, nfam - 1);
  auto skin_tex = [&](int ref) {
    if (ref < 0 || ref >= nskinref) return ref;
    const std::int64_t o = skin_at + (std::int64_t(skin) * nskinref + ref) * 2;
    return mdl.in(o, 2) ? static_cast<int>(mdl.i16(o)) : ref;
  };

  MaterialFinder mf;
  mf.model_dir = mdl_path.parent_path();
  for (const std::string& r : opt.material_paths) mf.roots.push_back(fs::u8path(r));
  {
    // Content roots above the model: <root>/models/... -> <root>/materials.
    std::error_code ec;
    fs::path p = mf.model_dir;
    for (int up = 0; up < 10 && !p.empty(); ++up) {
      if (fs::is_directory(p / "materials", ec)) mf.roots.push_back(p);
      if (lower(p.filename().string()) == "models") mf.roots.push_back(p.parent_path());
      const fs::path parent = p.parent_path();
      if (parent == p) break;
      p = parent;
    }
  }
  std::string missing;
  for (const std::string& t : textures) out->materials.push_back(source_material(mf, t, cd, &missing, &warn));
  if (!missing.empty()) append(&warn, "missing textures (magenta): " + missing);

  // -- vertices (vvd, LOD fixups) ----------------------------------------------
  const int vtx_lods = std::max(1, vtx.i32(20));
  const int vvd_lods = std::max(1, vvd.i32(12));
  const int lod = std::clamp(opt.lod, 0, std::min(vtx_lods, vvd_lods) - 1);
  out->lods = std::min(vtx_lods, vvd_lods);
  const int nfix = vvd.i32(48);
  const std::int64_t fix_at = vvd.i32(52), vdata = vvd.i32(56), tdata = vvd.i32(60);
  std::vector<std::pair<int, int>> ranges;  // (first source vertex, count)
  if (nfix > 0) {
    for (int i = 0; i < nfix; ++i) {
      const std::int64_t f = fix_at + std::int64_t(i) * 12;
      if (vvd.i32(f) >= lod) ranges.emplace_back(vvd.i32(f + 4), vvd.i32(f + 8));
    }
  } else {
    ranges.emplace_back(0, vvd.i32(16 + lod * 4));
  }
  std::vector<SrcVert> verts;
  for (const auto& [first, count] : ranges) {
    if (first < 0 || count < 0 || (count > 0 && !vvd.in(vdata + std::int64_t(first) * 48, std::int64_t(count) * 48))) {
      return fail(".vvd vertex range outside the file");
    }
    for (int k = 0; k < count; ++k) {
      const std::int64_t o = vdata + std::int64_t(first + k) * 48;
      SrcVert v;
      v.nb = std::clamp<int>(vvd.u8(o + 15), 1, 3);
      for (int j = 0; j < 3; ++j) {
        v.w[j] = j < v.nb ? vvd.f32(o + j * 4) : 0.0f;
        v.bone[j] = std::clamp<int>(vvd.u8(o + 12 + j), 0, nbones - 1);
      }
      v.pos = vvd.vec3(o + 16);
      v.normal = vvd.vec3(o + 28);
      v.u = vvd.f32(o + 40);
      v.v = vvd.f32(o + 44);
      const std::int64_t t = tdata + std::int64_t(first + k) * 16;
      if (tdata > 0 && vvd.in(t, 16)) v.tangent = {vvd.f32(t), vvd.f32(t + 4), vvd.f32(t + 8), vvd.f32(t + 12) < 0 ? -1.0f : 1.0f};
      verts.push_back(v);
    }
  }
  if (verts.empty()) return fail(".vvd has no vertices");

  // -- vtx layout probe (25-byte strip groups, or 33 with topology data) ----
  auto vtx_walk = [&](const VtxLayout& L, auto&& on_group) -> bool {
    const int nbp = vtx.i32(28);
    const std::int64_t bp_at = vtx.i32(32);
    for (int bp = 0; bp < nbp; ++bp) {
      const std::int64_t bpo = bp_at + std::int64_t(bp) * 8;
      if (!vtx.in(bpo, 8)) return false;
      const int nm = vtx.i32(bpo);
      for (int m = 0; m < nm; ++m) {
        const std::int64_t mo = bpo + vtx.i32(bpo + 4) + std::int64_t(m) * 8;
        if (!vtx.in(mo, 8)) return false;
        const int nl = vtx.i32(mo);
        if (nl <= 0) continue;
        const std::int64_t lo = mo + vtx.i32(mo + 4) + std::int64_t(std::min(lod, nl - 1)) * 12;
        if (!vtx.in(lo, 12)) return false;
        const int nmesh = vtx.i32(lo);
        for (int k = 0; k < nmesh; ++k) {
          const std::int64_t meo = lo + vtx.i32(lo + 4) + std::int64_t(k) * 9;
          if (!vtx.in(meo, 9)) return false;
          const int ng = vtx.i32(meo);
          for (int g = 0; g < ng; ++g) {
            const std::int64_t go = meo + vtx.i32(meo + 4) + std::int64_t(g) * L.strip_group;
            if (!vtx.in(go, 25)) return false;
            const int nv = vtx.i32(go), ni = vtx.i32(go + 8), ns = vtx.i32(go + 16);
            const std::int64_t vo = go + vtx.i32(go + 4), io = go + vtx.i32(go + 12);
            if (nv < 0 || ni < 0 || ns < 0 || !vtx.in(vo, std::int64_t(nv) * 9) || !vtx.in(io, std::int64_t(ni) * 2)) return false;
            for (int i = 0; i < ni; ++i) {
              if (vtx.u16(io + i * 2) >= nv) return false;
            }
            if (!on_group(bp, m, k, go, nv, vo, ni, io, ns)) return false;
          }
        }
      }
    }
    return true;
  };
  VtxLayout layout;
  auto nop = [](int, int, int, std::int64_t, int, std::int64_t, int, std::int64_t, int) { return true; };
  if (!vtx_walk(layout, nop)) {
    layout = VtxLayout{33, 35};
    if (!vtx_walk(layout, nop)) return fail(vtx_name + ": strip data does not match the model");
  }

  // -- body parts -> models -> meshes ----------------------------------------
  const int nbody = mdl.i32(232);
  const std::int64_t body_at = mdl.i32(236);
  struct MeshKey {
    int bp, m, k;
    bool operator<(const MeshKey& o) const { return std::tie(bp, m, k) < std::tie(o.bp, o.m, o.k); }
  };
  struct MeshInfo {
    int material = 0;
    int first_vertex = 0;  // into verts
    int nverts = 0;
    std::string name;
  };
  std::map<MeshKey, MeshInfo> meshes;
  std::vector<int> chosen(std::max(0, nbody), 0);
  for (int bp = 0; bp < nbody && bp < 256; ++bp) {
    const std::int64_t bo = body_at + std::int64_t(bp) * 16;
    if (!mdl.in(bo, 16)) break;
    ModelBodyGroup g;
    g.name = mdl.str(bo + mdl.i32(bo));
    const int nm = mdl.i32(bo + 4);
    const std::int64_t models_at = bo + mdl.i32(bo + 12);
    for (int m = 0; m < nm && m < 256; ++m) {
      const std::int64_t mo = models_at + std::int64_t(m) * 148;
      if (!mdl.in(mo, 148)) break;
      std::string mname = mdl.str(mo, 64);
      const int nmesh = mdl.i32(mo + 72);
      if (mname.empty() || nmesh == 0) mname = nmesh == 0 ? "(blank)" : "option " + std::to_string(m);
      g.options.push_back(mname);
      const int model_first = mdl.i32(mo + 84) / 48;
      for (int k = 0; k < nmesh && k < 1024; ++k) {
        const std::int64_t meo = mo + mdl.i32(mo + 76) + std::int64_t(k) * 116;
        if (!mdl.in(meo, 116)) break;
        MeshInfo mi;
        mi.material = mdl.i32(meo);
        mi.first_vertex = model_first + mdl.i32(meo + 12);
        mi.nverts = mdl.i32(meo + 8);
        mi.name = mname + (nmesh > 1 ? "_" + std::to_string(k) : "");
        meshes[{bp, m, k}] = mi;
      }
    }
    auto it = opt.body_groups.find(g.name);
    g.selected = std::clamp(it == opt.body_groups.end() ? 0 : it->second, 0,
                            std::max(0, static_cast<int>(g.options.size()) - 1));
    chosen[bp] = g.selected;
    out->body_groups.push_back(std::move(g));
  }
  out->skin_families = nfam;
  out->skin = skin;

  const bool skinned = nbones > 1;
  std::map<MeshKey, ModelPrimitive> prims;
  std::map<MeshKey, std::map<int, std::uint32_t>> remap;
  bool bad_index = false;
  vtx_walk(layout, [&](int bp, int m, int k, std::int64_t go, int nv, std::int64_t vo, int ni,
                       std::int64_t io, int ns) {
    if (bp >= static_cast<int>(chosen.size()) || chosen[bp] != m) return true;
    auto mit = meshes.find({bp, m, k});
    if (mit == meshes.end()) return true;
    const MeshInfo& mi = mit->second;
    ModelPrimitive& prim = prims[{bp, m, k}];
    auto& map = remap[{bp, m, k}];
    prim.mesh.name = mi.name;
    prim.material = skin_tex(mi.material);
    if (prim.material < 0 || prim.material >= static_cast<int>(out->materials.size())) prim.material = -1;
    auto vert = [&](int sg_index) -> std::uint32_t {
      const int orig = vtx.u16(vo + std::int64_t(sg_index) * 9 + 4);
      const int gi = mi.first_vertex + orig;
      auto f = map.find(gi);
      if (f != map.end()) return f->second;
      const std::uint32_t li = static_cast<std::uint32_t>(prim.mesh.vertices.size());
      SrcVert sv;
      if (gi >= 0 && gi < static_cast<int>(verts.size())) sv = verts[gi];
      else bad_index = true;
      Vertex3D v;
      v.position = sv.pos;
      v.normal = sv.normal;
      v.u = sv.u;
      v.v = sv.v;
      v.tangent = sv.tangent;
      prim.mesh.vertices.push_back(v);
      if (skinned) {
        SkinVertex s;
        float sum = 0.0f;
        for (int j = 0; j < 3; ++j) {
          s.joints[j] = static_cast<float>(sv.bone[j]);
          s.weights[j] = sv.w[j];
          sum += sv.w[j];
        }
        s.joints[3] = 0;
        s.weights[3] = 0;
        if (sum > 0.0f) {
          for (int j = 0; j < 3; ++j) s.weights[j] /= sum;
        } else {
          s.weights[0] = 1.0f;
        }
        prim.mesh.skin.push_back(s);
      }
      map[gi] = li;
      return li;
    };
    auto tri = [&](int a, int b, int c) {
      if (a == b || b == c || a == c) return;
      prim.mesh.indices.push_back(vert(a));
      prim.mesh.indices.push_back(vert(b));
      prim.mesh.indices.push_back(vert(c));
    };
    auto list = [&](std::int64_t first, int count) {
      for (int i = 0; i + 2 < count; i += 3) {
        tri(vtx.u16(io + (first + i) * 2), vtx.u16(io + (first + i + 1) * 2), vtx.u16(io + (first + i + 2) * 2));
      }
    };
    bool used_strips = false;
    const std::int64_t so = go + vtx.i32(go + 20);
    if (ns > 0 && vtx.in(so, std::int64_t(ns) * layout.strip)) {
      used_strips = true;
      for (int s = 0; s < ns; ++s) {
        const std::int64_t st = so + std::int64_t(s) * layout.strip;
        const int sn = vtx.i32(st), sfirst = vtx.i32(st + 4);
        const int flags = vtx.u8(st + 18);
        if (sn < 0 || sfirst < 0 || sfirst + sn > ni) {
          used_strips = false;
          break;
        }
        if (flags & 2) {  // triangle strip
          for (int i = 0; i + 2 < sn; ++i) {
            const int a = vtx.u16(io + (sfirst + i) * 2), b = vtx.u16(io + (sfirst + i + 1) * 2),
                      c = vtx.u16(io + (sfirst + i + 2) * 2);
            if (i & 1) tri(b, a, c);
            else tri(a, b, c);
          }
        } else {
          list(sfirst, sn);
        }
      }
    }
    if (!used_strips) list(0, ni);
    (void)nv;
    return true;
  });
  if (bad_index) append(&warn, "some VTX vertices point outside the .vvd");

  for (auto& [key, prim] : prims) {
    if (prim.mesh.indices.empty()) continue;
    // Source winds front faces clockwise; check against the normals and
    // flip to counter-clockwise when most triangles disagree.
    int agree = 0, disagree = 0;
    auto& idx = prim.mesh.indices;
    const auto& vs = prim.mesh.vertices;
    for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
      const Vec3 a = vs[idx[i]].position, b = vs[idx[i + 1]].position, c = vs[idx[i + 2]].position;
      const Vec3 n = cross(b - a, c - a);
      const Vec3 vn = vs[idx[i]].normal + vs[idx[i + 1]].normal + vs[idx[i + 2]].normal;
      const float d = dot(n, vn);
      if (d > 0) ++agree;
      else if (d < 0) ++disagree;
    }
    if (disagree > agree) {
      for (std::size_t i = 0; i + 2 < idx.size(); i += 3) std::swap(idx[i + 1], idx[i + 2]);
    }
    bool zero_tangents = true;
    for (const Vertex3D& v : vs) {
      if (v.tangent.x != 0.0f || v.tangent.y != 0.0f || v.tangent.z != 0.0f) {
        zero_tangents = false;
        break;
      }
    }
    if (zero_tangents || tdata <= 0) compute_tangents(prim.mesh);
    out->primitives.push_back(std::move(prim));
  }
  if (out->primitives.empty()) return fail(mdl_path.filename().string() + ": no triangles for the chosen body groups / LOD");

  // -- skeleton ---------------------------------------------------------------
  if (skinned) {
    for (int i = 0; i < nbones; ++i) {
      Joint j;
      j.name = bones[i].name;
      j.parent = bones[i].parent;
      j.t = bones[i].pos;
      j.r = bones[i].quat;
      j.inverse_bind = is_zero(bones[i].pose_to_bone) ? rest[i].inverse() : bones[i].pose_to_bone;
      out->skeleton.joints.push_back(std::move(j));
    }
  }  // (single-bone props stay rigid: VVD vertices are in model space)

  // -- animations -------------------------------------------------------------
  if (opt.animations && skinned) {
    const int nanim = mdl.i32(180);
    const std::int64_t anim_at = mdl.i32(184);
    std::vector<Clip> by_anim(std::max(0, std::min(nanim, 4096)));
    std::vector<bool> anim_ok(by_anim.size(), false);
    int skipped_delta = 0, skipped_block = 0;
    for (int a = 0; a < static_cast<int>(by_anim.size()); ++a) {
      const std::int64_t ad = anim_at + std::int64_t(a) * 100;
      if (!mdl.in(ad, 100)) break;
      Clip clip;
      clip.name = mdl.str(ad + mdl.i32(ad + 4));
      if (!clip.name.empty() && clip.name[0] == '@') clip.name.erase(0, 1);
      const float fps = mdl.f32(ad + 8) > 0 ? mdl.f32(ad + 8) : 30.0f;
      const int flags = mdl.i32(ad + 12);
      const int frames = mdl.i32(ad + 16);
      const int block = mdl.i32(ad + 52);
      const std::int32_t index = mdl.i32(ad + 56);
      const std::int32_t sec_index = mdl.i32(ad + 80), sec_frames = mdl.i32(ad + 84);
      if (flags & 0x4) { ++skipped_delta; continue; }
      if (block != 0) { ++skipped_block; continue; }
      if (frames <= 0 || frames > 100000) continue;
      // Per frame: find the anim chain and the frame inside it.
      std::vector<std::vector<Vec3>> tpos(nbones);
      std::vector<std::vector<Quat>> trot(nbones);
      std::vector<bool> touched(nbones, false);
      bool ok = true;
      for (int f = 0; f < frames && ok; ++f) {
        std::int64_t chain = ad + index;
        int local = f;
        if (sec_frames > 0) {
          int section;
          if (frames > sec_frames && f == frames - 1) {
            section = frames / sec_frames + 1;
            local = 0;
          } else {
            section = f / sec_frames;
            local = f - section * sec_frames;
          }
          const std::int64_t so = ad + sec_index + std::int64_t(section) * 8;
          if (!mdl.in(so, 8) || mdl.i32(so) != 0) { ok = false; break; }
          chain = ad + mdl.i32(so + 4);
        } else if (index == 0) {
          ok = false;
          break;
        }
        std::vector<Vec3> fp(nbones);
        std::vector<Quat> fr(nbones);
        for (int b = 0; b < nbones; ++b) {
          fp[b] = bones[b].pos;
          fr[b] = bones[b].quat;
        }
        for (int guard = 0; guard < nbones + 1 && mdl.in(chain, 4); ++guard) {
          const int bone = mdl.u8(chain), fl = mdl.u8(chain + 1);
          const int next = mdl.i16(chain + 2);
          if (bone < nbones) {
            const SrcBone& bn = bones[bone];
            const bool delta = (fl & 0x10) != 0;
            std::int64_t p = chain + 4;
            if (fl & 0x02) {
              fr[bone] = quat48(mdl, p);
              p += 6;
            } else if (fl & 0x20) {
              fr[bone] = quat64(mdl, p);
              p += 8;
            } else if (fl & 0x08) {
              Vec3 e;
              float* ev[3] = {&e.x, &e.y, &e.z};
              const float rs[3] = {bn.rotscale.x, bn.rotscale.y, bn.rotscale.z};
              const float r0[3] = {bn.rot.x, bn.rot.y, bn.rot.z};
              for (int c = 0; c < 3; ++c) {
                const int off = mdl.i16(p + c * 2);
                *ev[c] = (off ? anim_value(mdl, p + off, local) * rs[c] : 0.0f) + (delta ? 0.0f : r0[c]);
              }
              fr[bone] = euler_quat(e);
              p += 6;
            } else if (delta) {
              fr[bone] = Quat{};
            }
            if (fl & 0x01) {
              fp[bone] = {half(mdl.u16(p)), half(mdl.u16(p + 2)), half(mdl.u16(p + 4))};
            } else if (fl & 0x04) {
              Vec3 v;
              float* vv[3] = {&v.x, &v.y, &v.z};
              const float ps[3] = {bn.posscale.x, bn.posscale.y, bn.posscale.z};
              const float p0[3] = {bn.pos.x, bn.pos.y, bn.pos.z};
              for (int c = 0; c < 3; ++c) {
                const int off = mdl.i16(p + c * 2);
                *vv[c] = (off ? anim_value(mdl, p + off, local) * ps[c] : 0.0f) + (delta ? 0.0f : p0[c]);
              }
              fp[bone] = v;
            } else if (delta) {
              fp[bone] = Vec3{};
            }
            touched[bone] = true;
          }
          if (next == 0) break;
          chain += next;
        }
        for (int b = 0; b < nbones; ++b) {
          tpos[b].push_back(fp[b]);
          trot[b].push_back(fr[b]);
        }
      }
      if (!ok) continue;
      clip.duration = frames > 1 ? (frames - 1) / fps : 0.0f;
      for (int b = 0; b < nbones; ++b) {
        if (!touched[b]) continue;
        Channel t, r;
        t.joint = r.joint = b;
        t.path = ChannelPath::Translation;
        r.path = ChannelPath::Rotation;
        Quat prev;
        for (int f = 0; f < frames; ++f) {
          const float time = f / fps;
          t.times.push_back(time);
          r.times.push_back(time);
          t.values.insert(t.values.end(), {tpos[b][f].x, tpos[b][f].y, tpos[b][f].z});
          Quat q = normalize(trot[b][f]);
          if (f > 0 && q.x * prev.x + q.y * prev.y + q.z * prev.z + q.w * prev.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
          prev = q;
          r.values.insert(r.values.end(), {q.x, q.y, q.z, q.w});
        }
        clip.channels.push_back(std::move(t));
        clip.channels.push_back(std::move(r));
      }
      by_anim[a] = std::move(clip);
      anim_ok[a] = true;
    }
    // Sequences name the clips (first blend of each).
    const int nseq = mdl.i32(188);
    const std::int64_t seq_at = mdl.i32(192);
    std::vector<bool> used(by_anim.size(), false);
    std::set<std::string> names;
    for (int s = 0; s < nseq && s < 4096; ++s) {
      const std::int64_t sd = seq_at + std::int64_t(s) * 212;
      if (!mdl.in(sd, 212)) break;
      const std::string label = mdl.str(sd + mdl.i32(sd + 4));
      const int a = mdl.i16(sd + mdl.i32(sd + 60));
      if (label.empty() || a < 0 || a >= static_cast<int>(by_anim.size()) || !anim_ok[a]) continue;
      if (!names.insert(label).second) continue;
      Clip c = by_anim[a];
      c.name = label;
      out->clips.push_back(std::move(c));
      used[a] = true;
    }
    for (std::size_t a = 0; a < by_anim.size(); ++a) {
      if (anim_ok[a] && !used[a] && names.insert(by_anim[a].name).second) out->clips.push_back(by_anim[a]);
    }
    if (skipped_delta) append(&warn, std::to_string(skipped_delta) + " delta (additive) animations skipped");
    if (skipped_block) append(&warn, std::to_string(skipped_block) + " animations in external .ani blocks skipped");
    if (mdl.i32(336) > 0) append(&warn, std::to_string(mdl.i32(336)) + " $includemodel animation files not loaded");
  }

  // -- collision (.phy) -------------------------------------------------------
  if (opt.collision && read_file(fs::u8path(b8 + ".phy"), &phy)) {
    std::vector<PhySolid> solids;
    if (!parse_phy(phy, &solids, &warn)) {
      append(&warn, ".phy could not be read");
    } else {
      for (std::size_t s = 0; s < solids.size(); ++s) {
        int bone = 0;
        for (int i = 0; i < nbones; ++i) {
          if (!solids[s].bone.empty() && lower(bones[i].name) == lower(solids[s].bone)) bone = i;
        }
        // Hull points are in their bone's space; bring them to model space.
        const Mat4 to_model = is_zero(bones[bone].pose_to_bone) ? rest[bone] : bones[bone].pose_to_bone.inverse();
        for (auto& hull : solids[s].hulls) {
          for (Vec3& p : hull) p = to_model.transform_point(p);
          out->collision_hulls.push_back(std::move(hull));
        }
      }
    }
  }

  update_bounds(out);
  if (error) *error = warn;
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
