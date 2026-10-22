// Model import smokes. All test assets are generated below (our own tiny
// box / hat / bar / quad models and textures) in a temp folder.
#include "SmokeModels.h"
#include "SmokeBounds.h"

#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "render3d/ModelImport.h"
#include "render3d/import/SourceFormats.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace r3 = ts::tombstone::render3d;
namespace adb = ts::tombstone::editor::assetdb;
namespace json = ts::tombstone::json;
using r3::Vec3;

namespace {

int fail(const char* which, const std::string& what) {
  std::cerr << "[smoke] " << which << " FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

void spit(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

void spit(const fs::path& p, const std::vector<unsigned char>& d) {
  spit(p, std::string(d.begin(), d.end()));
}

// Little-endian byte builder.
struct Buf {
  std::vector<unsigned char> d;
  std::size_t size() const { return d.size(); }
  std::size_t alloc(std::size_t n) {
    const std::size_t o = d.size();
    d.resize(o + n, 0);
    return o;
  }
  void i32(std::size_t o, std::int32_t v) { std::memcpy(&d[o], &v, 4); }
  void i16(std::size_t o, std::int16_t v) { std::memcpy(&d[o], &v, 2); }
  void u16(std::size_t o, std::uint16_t v) { std::memcpy(&d[o], &v, 2); }
  void u8(std::size_t o, std::uint8_t v) { d[o] = v; }
  void f32(std::size_t o, float v) { std::memcpy(&d[o], &v, 4); }
  void vec3(std::size_t o, float x, float y, float z) {
    f32(o, x);
    f32(o + 4, y);
    f32(o + 8, z);
  }
  void bytes(std::size_t o, const void* p, std::size_t n) { std::memcpy(&d[o], p, n); }
  std::size_t str(const std::string& s) {
    const std::size_t o = alloc(s.size() + 1);
    std::memcpy(&d[o], s.data(), s.size());
    return o;
  }
  // Field at `field` = offset of a new string, relative to `base`.
  void str_rel(std::size_t base, std::size_t field, const std::string& s) {
    i32(field, static_cast<std::int32_t>(str(s)) - static_cast<std::int32_t>(base));
  }
};

// ---- synthetic Source model ------------------------------------------------

struct TVert {
  float p[3], n[3], uv[2];
  int bone[3] = {0, 0, 0};
  float w[3] = {1, 0, 0};
  int nb = 1;
};
struct TMesh {
  int material = 0;
  std::vector<TVert> v;
  std::vector<int> tris;       // counter-clockwise seen from outside
  std::vector<int> lod1_tris;  // LOD 1 (subset)
};
struct TModel {
  std::string name;
  std::vector<TMesh> meshes;
};
struct TBodyPart {
  std::string name;
  std::vector<TModel> models;
};

// Box 16 x 32 x 48 inches (Source X / Y / Z), standing on the origin. The
// top face is weighted to bone 1 ("arm").
TMesh make_box(float scale) {
  TMesh m;
  const float hx = 8 * scale, hy = 16 * scale, hz = 48 * scale;
  struct Face { float n[3]; float c[4][3]; };
  const Face faces[6] = {
      {{1, 0, 0}, {{hx, -hy, 0}, {hx, hy, 0}, {hx, hy, hz}, {hx, -hy, hz}}},
      {{-1, 0, 0}, {{-hx, hy, 0}, {-hx, -hy, 0}, {-hx, -hy, hz}, {-hx, hy, hz}}},
      {{0, 1, 0}, {{hx, hy, 0}, {-hx, hy, 0}, {-hx, hy, hz}, {hx, hy, hz}}},
      {{0, -1, 0}, {{-hx, -hy, 0}, {hx, -hy, 0}, {hx, -hy, hz}, {-hx, -hy, hz}}},
      {{0, 0, 1}, {{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}}},
      {{0, 0, -1}, {{-hx, hy, 0}, {hx, hy, 0}, {hx, -hy, 0}, {-hx, -hy, 0}}},
  };
  const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  for (int f = 0; f < 6; ++f) {
    const int base = static_cast<int>(m.v.size());
    for (int k = 0; k < 4; ++k) {
      TVert v;
      for (int a = 0; a < 3; ++a) {
        v.p[a] = faces[f].c[k][a];
        v.n[a] = faces[f].n[a];
      }
      v.uv[0] = uv[k][0];
      v.uv[1] = uv[k][1];
      if (v.p[2] >= hz - 1e-4f) {
        v.bone[0] = 1;  // top: the arm bone
      }
      m.v.push_back(v);
    }
    for (int t : {0, 1, 2, 0, 2, 3}) m.tris.push_back(base + t);
  }
  m.lod1_tris.assign(m.tris.begin(), m.tris.begin() + 18);  // three faces
  return m;
}

// Little pyramid hat on top of the box (z 48 .. 60), all on bone 1.
TMesh make_hat() {
  TMesh m;
  m.material = 1;
  const float pts[5][3] = {{-4, -4, 48}, {4, -4, 48}, {4, 4, 48}, {-4, 4, 48}, {0, 0, 60}};
  for (const auto& p : pts) {
    TVert v;
    const float c[3] = {0, 0, 52};
    float n[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int a = 0; a < 3; ++a) {
      v.p[a] = p[a];
      v.n[a] = n[a] / l;
    }
    v.uv[0] = 0.5f;
    v.uv[1] = 0.5f;
    v.bone[0] = 1;
    m.v.push_back(v);
  }
  m.tris = {0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4, 0, 2, 1, 0, 3, 2};
  m.lod1_tris = m.tris;
  return m;
}

constexpr std::int32_t kChecksum = 0x5453310A;

struct SourceSet {
  std::vector<unsigned char> mdl, vvd, vtx, phy;
};

SourceSet build_source(int version, bool wide_strips, float box_scale) {
  std::vector<TBodyPart> parts(2);
  parts[0].name = "body";
  parts[0].models.push_back({"box", {make_box(box_scale)}});
  parts[1].name = "hat";
  parts[1].models.push_back({"blank", {}});
  parts[1].models.push_back({"tophat", {make_hat()}});

  // VVD order: every mesh's vertices, body part by body part.
  std::vector<TVert> all;
  std::vector<std::vector<int>> model_first(parts.size());
  for (std::size_t b = 0; b < parts.size(); ++b) {
    for (const TModel& m : parts[b].models) {
      model_first[b].push_back(static_cast<int>(all.size()));
      for (const TMesh& me : m.meshes) all.insert(all.end(), me.v.begin(), me.v.end());
    }
  }
  SourceSet out;

  // .mdl
  Buf h;
  h.alloc(408);
  h.bytes(0, "IDST", 4);
  h.i32(4, version);
  h.i32(8, kChecksum);
  h.bytes(12, "ts_test/box.mdl", 15);
  h.vec3(104, -8, -16, 0);
  h.vec3(116, 8, 16, 60);
  h.vec3(128, -8, -16, 0);
  h.vec3(140, 8, 16, 60);
  struct BoneDef { const char* name; int parent; float pos[3]; };
  const BoneDef bones[2] = {{"root", -1, {0, 0, 0}}, {"arm", 0, {0, 0, 32}}};
  const std::size_t bone_at = h.alloc(2 * 216);
  h.i32(156, 2);
  h.i32(160, static_cast<std::int32_t>(bone_at));
  for (int i = 0; i < 2; ++i) {
    const std::size_t o = bone_at + i * 216;
    h.i32(o + 4, bones[i].parent);
    for (int k = 0; k < 6; ++k) h.i32(o + 8 + k * 4, -1);
    h.vec3(o + 32, bones[i].pos[0], bones[i].pos[1], bones[i].pos[2]);
    h.f32(o + 56, 1.0f);  // quat w
    h.vec3(o + 72, 1, 1, 1);  // posscale
    h.vec3(o + 84, 1, 1, 1);  // rotscale
    // poseToBone (3x4, row major): model -> bone = translate(-pos).
    const float mpos[3] = {0, 0, i == 1 ? 32.0f : 0.0f};
    for (int r = 0; r < 3; ++r) {
      h.f32(o + 96 + (r * 4 + r) * 4, 1.0f);
      h.f32(o + 96 + (r * 4 + 3) * 4, -mpos[r]);
    }
    h.f32(o + 156, 1.0f);  // qAlignment w
  }
  // Hitbox set "default": a body box on the root, a head box (group 1,
  // named) on the arm bone, in bone space.
  {
    const std::size_t hset = h.alloc(12);
    h.i32(172, 1);
    h.i32(176, static_cast<std::int32_t>(hset));
    const std::size_t hbs = h.alloc(2 * 68);
    h.i32(hset + 4, 2);
    h.i32(hset + 8, static_cast<std::int32_t>(hbs - hset));
    h.i32(hbs, 0);
    h.i32(hbs + 4, 3);
    h.vec3(hbs + 8, -8, -16, 0);
    h.vec3(hbs + 20, 8, 16, 32);
    h.i32(hbs + 68, 1);
    h.i32(hbs + 68 + 4, 1);
    h.vec3(hbs + 68 + 8, 4, 4, 8);   // min / max swapped on purpose
    h.vec3(hbs + 68 + 20, -4, -4, 0);
    h.str_rel(hset, hset, "default");
    h.str_rel(hbs + 68, hbs + 68 + 32, "head_box");
  }
  // Animation "idle": 2 frames at 10 fps; bone 0 raw position, bone 1 raw
  // rotation + an RLE position track (z: 0 then 16 on top of 32).
  const std::size_t ad = h.alloc(100);
  h.i32(180, 1);
  h.i32(184, static_cast<std::int32_t>(ad));
  h.i32(ad, -static_cast<std::int32_t>(ad));
  h.f32(ad + 8, 10.0f);
  h.i32(ad + 16, 2);
  const std::size_t chain = h.alloc(10 + 4 + 6 + 6 + 6);
  h.i32(ad + 56, static_cast<std::int32_t>(chain - ad));
  h.u8(chain, 0);
  h.u8(chain + 1, 0x01);  // RAWPOS
  h.i16(chain + 2, 10);
  // Vector48 (0, 0, 0) = zero halves.
  const std::size_t e1 = chain + 10;
  h.u8(e1, 1);
  h.u8(e1 + 1, 0x02 | 0x04);  // RAWROT | ANIMPOS
  h.i16(e1 + 2, 0);
  h.u16(e1 + 4, 32768);  // Quaternion48 identity
  h.u16(e1 + 6, 32768);
  h.u16(e1 + 8, 16384);
  h.i16(e1 + 10, 0);  // valueptr x, y: none
  h.i16(e1 + 12, 0);
  h.i16(e1 + 14, 6);  // z track right after the valueptr
  h.u8(e1 + 16, 2);   // valid
  h.u8(e1 + 17, 2);   // total
  h.i16(e1 + 18, 0);
  h.i16(e1 + 20, 16);
  h.str_rel(ad, ad + 4, "@idle");
  // Sequence "idle" -> anim 0.
  const std::size_t sd = h.alloc(212);
  h.i32(188, 1);
  h.i32(192, static_cast<std::int32_t>(sd));
  h.i32(sd, -static_cast<std::int32_t>(sd));
  h.i32(sd + 12, 1);  // looping
  h.i32(sd + 56, 1);  // numblends
  const std::size_t blend = h.alloc(4);
  h.i32(sd + 60, static_cast<std::int32_t>(blend - sd));
  h.i32(sd + 68, 1);
  h.i32(sd + 72, 1);
  h.str_rel(sd, sd + 4, "idle");
  h.str_rel(sd, sd + 8, "ACT_IDLE");
  // Textures + cd path + skins.
  const char* tex[2] = {"crate", "missing_tex"};
  const std::size_t tex_at = h.alloc(2 * 64);
  h.i32(204, 2);
  h.i32(208, static_cast<std::int32_t>(tex_at));
  const std::size_t cd_at = h.alloc(4);
  h.i32(212, 1);
  h.i32(216, static_cast<std::int32_t>(cd_at));
  const std::size_t skin_at = h.alloc(8);
  h.i32(220, 2);
  h.i32(224, 2);
  h.i32(228, static_cast<std::int32_t>(skin_at));
  h.i16(skin_at, 0);
  h.i16(skin_at + 2, 1);
  h.i16(skin_at + 4, 1);  // skin 1 swaps the two
  h.i16(skin_at + 6, 0);
  // Body parts -> models -> meshes.
  const std::size_t bp_at = h.alloc(parts.size() * 16);
  h.i32(232, static_cast<std::int32_t>(parts.size()));
  h.i32(236, static_cast<std::int32_t>(bp_at));
  h.f32(328, 10.0f);  // mass
  struct Pending { std::size_t base, field; std::string s; };
  std::vector<Pending> strings;
  for (int i = 0; i < 2; ++i) strings.push_back({bone_at + i * 216, bone_at + i * 216, bones[i].name});
  for (int i = 0; i < 2; ++i) strings.push_back({tex_at + i * 64, tex_at + i * 64, tex[i]});
  int base_mul = 1;
  for (std::size_t b = 0; b < parts.size(); ++b) {
    const std::size_t bo = bp_at + b * 16;
    strings.push_back({bo, bo, parts[b].name});
    h.i32(bo + 4, static_cast<std::int32_t>(parts[b].models.size()));
    h.i32(bo + 8, base_mul);
    base_mul *= static_cast<int>(parts[b].models.size());
    const std::size_t models = h.alloc(parts[b].models.size() * 148);
    h.i32(bo + 12, static_cast<std::int32_t>(models - bo));
    for (std::size_t m = 0; m < parts[b].models.size(); ++m) {
      const TModel& tm = parts[b].models[m];
      const std::size_t mo = models + m * 148;
      h.bytes(mo, tm.name.data(), tm.name.size());
      h.f32(mo + 68, 40.0f);
      int nverts = 0;
      for (const TMesh& me : tm.meshes) nverts += static_cast<int>(me.v.size());
      h.i32(mo + 72, static_cast<std::int32_t>(tm.meshes.size()));
      h.i32(mo + 80, nverts);
      h.i32(mo + 84, model_first[b][m] * 48);
      h.i32(mo + 88, model_first[b][m] * 16);
      if (tm.meshes.empty()) continue;
      const std::size_t meshes = h.alloc(tm.meshes.size() * 116);
      h.i32(mo + 76, static_cast<std::int32_t>(meshes - mo));
      int off = 0;
      for (std::size_t k = 0; k < tm.meshes.size(); ++k) {
        const std::size_t me = meshes + k * 116;
        h.i32(me, tm.meshes[k].material);
        h.i32(me + 4, static_cast<std::int32_t>(mo) - static_cast<std::int32_t>(me));
        h.i32(me + 8, static_cast<std::int32_t>(tm.meshes[k].v.size()));
        h.i32(me + 12, off);
        h.i32(me + 32, static_cast<std::int32_t>(k));
        for (int l = 0; l < 8; ++l) h.i32(me + 52 + l * 4, static_cast<std::int32_t>(tm.meshes[k].v.size()));
        off += static_cast<int>(tm.meshes[k].v.size());
      }
    }
  }
  for (const Pending& p : strings) h.str_rel(p.base, p.field, p.s);
  h.i32(cd_at, static_cast<std::int32_t>(h.str("models\\ts_test\\")));
  h.i32(76, static_cast<std::int32_t>(h.size()));
  out.mdl = h.d;

  // .vvd
  Buf v;
  v.alloc(64);
  v.bytes(0, "IDSV", 4);
  v.i32(4, 4);
  v.i32(8, kChecksum);
  v.i32(12, 2);
  for (int l = 0; l < 8; ++l) v.i32(16 + l * 4, l < 2 ? static_cast<std::int32_t>(all.size()) : 0);
  v.i32(48, 0);
  v.i32(52, 64);
  v.i32(56, 64);
  const std::size_t vd = v.alloc(all.size() * 48);
  const std::size_t td = v.alloc(all.size() * 16);  // zero tangents -> recomputed
  v.i32(60, static_cast<std::int32_t>(td));
  for (std::size_t i = 0; i < all.size(); ++i) {
    const TVert& t = all[i];
    const std::size_t o = vd + i * 48;
    for (int k = 0; k < 3; ++k) {
      v.f32(o + k * 4, t.w[k]);
      v.u8(o + 12 + k, static_cast<std::uint8_t>(t.bone[k]));
    }
    v.u8(o + 15, static_cast<std::uint8_t>(t.nb));
    v.vec3(o + 16, t.p[0], t.p[1], t.p[2]);
    v.vec3(o + 28, t.n[0], t.n[1], t.n[2]);
    v.f32(o + 40, t.uv[0]);
    v.f32(o + 44, t.uv[1]);
  }
  out.vvd = v.d;

  // .dx90.vtx (25-byte strip groups / 27-byte strips; 33 / 35 when wide)
  const int SG = wide_strips ? 33 : 25, ST = wide_strips ? 35 : 27;
  Buf x;
  x.alloc(36);
  x.i32(0, 7);
  x.i32(4, 24);
  x.u16(8, 53);
  x.u16(10, 9);
  x.i32(12, 3);
  x.i32(16, kChecksum);
  x.i32(20, 2);
  x.i32(28, static_cast<std::int32_t>(parts.size()));
  const std::size_t xbp = x.alloc(parts.size() * 8);
  x.i32(32, static_cast<std::int32_t>(xbp));
  for (std::size_t b = 0; b < parts.size(); ++b) {
    const std::size_t bo = xbp + b * 8;
    const std::size_t models = x.alloc(parts[b].models.size() * 8);
    x.i32(bo, static_cast<std::int32_t>(parts[b].models.size()));
    x.i32(bo + 4, static_cast<std::int32_t>(models - bo));
    for (std::size_t m = 0; m < parts[b].models.size(); ++m) {
      const TModel& tm = parts[b].models[m];
      const std::size_t mo = models + m * 8;
      const std::size_t lods = x.alloc(2 * 12);
      x.i32(mo, 2);
      x.i32(mo + 4, static_cast<std::int32_t>(lods - mo));
      for (int l = 0; l < 2; ++l) {
        const std::size_t lo = lods + l * 12;
        x.i32(lo, static_cast<std::int32_t>(tm.meshes.size()));
        x.f32(lo + 8, l * 12.0f);
        if (tm.meshes.empty()) continue;
        const std::size_t meshes = x.alloc(tm.meshes.size() * 9);
        x.i32(lo + 4, static_cast<std::int32_t>(meshes - lo));
        for (std::size_t k = 0; k < tm.meshes.size(); ++k) {
          const TMesh& me = tm.meshes[k];
          const std::vector<int>& tris = l == 0 ? me.tris : me.lod1_tris;
          const std::size_t mh = meshes + k * 9;
          const std::size_t sg = x.alloc(SG);
          x.i32(mh, 1);
          x.i32(mh + 4, static_cast<std::int32_t>(sg - mh));
          const std::size_t verts = x.alloc(me.v.size() * 9);
          const std::size_t idx = x.alloc(tris.size() * 2);
          const std::size_t strip = x.alloc(ST);
          x.i32(sg, static_cast<std::int32_t>(me.v.size()));
          x.i32(sg + 4, static_cast<std::int32_t>(verts - sg));
          x.i32(sg + 8, static_cast<std::int32_t>(tris.size()));
          x.i32(sg + 12, static_cast<std::int32_t>(idx - sg));
          x.i32(sg + 16, 1);
          x.i32(sg + 20, static_cast<std::int32_t>(strip - sg));
          x.u8(sg + 24, 0x02);
          for (std::size_t i = 0; i < me.v.size(); ++i) {
            const std::size_t o = verts + i * 9;
            x.u8(o, 0);
            x.u8(o + 1, 1);
            x.u8(o + 2, 2);
            x.u8(o + 3, 1);
            x.u16(o + 4, static_cast<std::uint16_t>(i));
            x.u8(o + 6, static_cast<std::uint8_t>(me.v[i].bone[0]));
          }
          // Clockwise (Direct3D front faces), like studiomdl writes them.
          for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            x.u16(idx + t * 2, static_cast<std::uint16_t>(tris[t]));
            x.u16(idx + t * 2 + 2, static_cast<std::uint16_t>(tris[t + 2]));
            x.u16(idx + t * 2 + 4, static_cast<std::uint16_t>(tris[t + 1]));
          }
          x.i32(strip, static_cast<std::int32_t>(tris.size()));
          x.i32(strip + 8, static_cast<std::int32_t>(me.v.size()));
          x.i16(strip + 16, 2);
          x.u8(strip + 18, 0x01);  // triangle list
        }
      }
    }
  }
  out.vtx = x.d;

  // .phy: one solid (bone "root"), one convex ledge = the box's 8 corners.
  const float hx = 8, hy = 16, hz = 48;
  const float corners[8][3] = {{-hx, -hy, 0}, {hx, -hy, 0}, {hx, hy, 0}, {-hx, hy, 0},
                               {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}};
  const int ctris[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                            {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
  Buf surf;
  surf.alloc(48);
  const std::size_t ledge = surf.alloc(16 + 12 * 16);
  const std::size_t points = surf.alloc(8 * 16);
  const std::size_t node = surf.alloc(28);
  surf.i32(ledge, static_cast<std::int32_t>(points - ledge));
  surf.i32(ledge + 8, (13 << 8) | (1 << 2));
  surf.i16(ledge + 12, 12);
  for (int t = 0; t < 12; ++t) {
    const std::size_t o = ledge + 16 + t * 16;
    surf.i32(o, t);
    for (int e = 0; e < 3; ++e) surf.i32(o + 4 + e * 4, ctris[t][e]);
  }
  for (int i = 0; i < 8; ++i) {
    // Source inches -> IVP metres (x, -z, y).
    surf.vec3(points + i * 16, corners[i][0] * 0.0254f, -corners[i][2] * 0.0254f, corners[i][1] * 0.0254f);
  }
  surf.i32(node, 0);
  surf.i32(node + 4, static_cast<std::int32_t>(ledge) - static_cast<std::int32_t>(node));
  surf.f32(node + 20, 1.0f);
  surf.i32(28, static_cast<std::int32_t>(surf.size()) << 8);
  surf.i32(32, static_cast<std::int32_t>(node));
  surf.bytes(44, "IVPS", 4);
  Buf p;
  p.alloc(16);
  p.i32(0, 16);
  p.i32(8, 1);
  p.i32(12, kChecksum);
  const std::size_t solid = p.alloc(4 + 28);
  p.i32(solid, static_cast<std::int32_t>(28 + surf.size()));
  p.bytes(solid + 4, "VPHY", 4);
  p.u16(solid + 8, 0x100);
  p.u16(solid + 10, 0);
  p.i32(solid + 12, static_cast<std::int32_t>(surf.size()));
  const std::size_t sb = p.alloc(surf.size());
  p.bytes(sb, surf.d.data(), surf.size());
  const std::string text =
      "solid {\n\"index\" \"0\"\n\"name\" \"root\"\n\"mass\" \"10.000000\"\n\"surfaceprop\" \"wood\"\n}\n"
      "editparams {\n\"rootname\" \"\"\n\"totalmass\" \"10.000000\"\n}\n";
  p.str(text);
  out.phy = p.d;
  return out;
}

void write_source(const fs::path& dir, const std::string& base, const SourceSet& s) {
  spit(dir / (base + ".mdl"), s.mdl);
  spit(dir / (base + ".vvd"), s.vvd);
  spit(dir / (base + ".dx90.vtx"), s.vtx);
  spit(dir / (base + ".phy"), s.phy);
}

// VTF 7.2: DXT1 8x8 with 2 mips (mip 1 red, mip 0 green) + a 4x4 thumbnail.
std::vector<unsigned char> vtf_dxt1() {
  Buf b;
  b.alloc(80);
  b.bytes(0, "VTF\0", 4);
  b.i32(4, 7);
  b.i32(8, 2);
  b.i32(12, 80);
  b.u16(16, 8);
  b.u16(18, 8);
  b.u16(24, 1);
  b.f32(48, 1.0f);
  b.i32(52, 13);  // DXT1
  b.u8(56, 2);
  b.i32(57, 13);
  b.u8(61, 4);
  b.u8(62, 4);
  b.u16(63, 1);
  auto block = [&](std::uint16_t c) {
    const std::size_t o = b.alloc(8);
    b.u16(o, c);
    b.u16(o + 2, 0);
  };
  block(0x001F);  // thumbnail: blue
  block(0xF800);  // mip 1: red
  for (int i = 0; i < 4; ++i) block(0x07E0);  // mip 0: green
  return b.d;
}

// VTF 7.4 with a resource table: BGRA8888 2x2.
std::vector<unsigned char> vtf_bgra() {
  Buf b;
  b.alloc(80 + 2 * 8);
  b.bytes(0, "VTF\0", 4);
  b.i32(4, 7);
  b.i32(8, 4);
  b.i32(12, 96);
  b.u16(16, 2);
  b.u16(18, 2);
  b.u16(24, 1);
  b.i32(52, 12);  // BGRA8888
  b.u8(56, 1);
  b.i32(57, -1);
  b.u16(63, 1);
  b.i32(68, 2);
  b.u8(80, 0x01);  // low-res resource (none: points at nothing)
  b.i32(84, 96);
  b.u8(88, 0x30);  // image data
  b.i32(92, 96);
  const std::size_t px = b.alloc(16);
  for (int i = 0; i < 4; ++i) {
    const unsigned char bgra[4] = {255, 128, 0, 255};
    b.bytes(px + i * 4, bgra, 4);
  }
  return b.d;
}

void write_materials(const fs::path& content) {
  const fs::path m = content / "materials" / "models" / "ts_test";
  spit(m / "crate.vmt",
       "\"VertexLitGeneric\"\n{\n\t// generated for the smoke\n\t\"$basetexture\" \"models/ts_test/crate_color\"\n"
       "\t\"$bumpmap\" \"models\\\\ts_test\\\\crate_normal\"\n\t$nocull 1\n}\n");
  spit(m / "crate_color.vtf", vtf_dxt1());
  spit(m / "crate_normal.vtf", vtf_bgra());
}

std::size_t vertex_count(const r3::Model& m) {
  std::size_t n = 0;
  for (const auto& p : m.primitives) n += p.mesh.vertices.size();
  return n;
}

bool windings_agree(const r3::Model& m) {
  for (const auto& p : m.primitives) {
    const auto& v = p.mesh.vertices;
    const auto& i = p.mesh.indices;
    for (std::size_t t = 0; t + 2 < i.size(); t += 3) {
      const Vec3 n = r3::cross(v[i[t + 1]].position - v[i[t]].position, v[i[t + 2]].position - v[i[t]].position);
      if (r3::dot(n, v[i[t]].normal + v[i[t + 1]].normal + v[i[t + 2]].normal) <= 0.0f) return false;
    }
  }
  return true;
}

// Highest vertex after posing `clip` at `time` through the skin palette.
float posed_top(const r3::Model& m, const r3::Clip* clip, float time) {
  r3::Pose pose = r3::rest_pose(m.skeleton);
  if (clip) r3::sample_clip(*clip, time, false, &pose);
  std::vector<r3::Mat4> pal;
  r3::skin_palette(m.skeleton, pose, &pal);
  float top = -1e30f;
  for (const auto& p : m.primitives) {
    for (std::size_t k = 0; k < p.mesh.vertices.size(); ++k) {
      const Vec3& v = p.mesh.vertices[k].position;
      if (p.mesh.skin.empty()) {
        top = std::max(top, v.y);
        continue;
      }
      const r3::SkinVertex& s = p.mesh.skin[k];
      top = std::max(top, r3::skin_point(pal, s.joints, s.weights, v).y);
    }
  }
  return top;
}

int check_source_model(const fs::path& mdl, int version) {
  const char* W = "mdl";
  const std::string tag = "v" + std::to_string(version) + ": ";
  r3::Model m;
  std::string warn;
  if (!r3::load_model(mdl.string(), &m, &warn)) return fail(W, tag + "load: " + warn);
  if (m.format != "source-mdl") return fail(W, tag + "format " + m.format);
  if (m.primitives.size() != 1 || vertex_count(m) != 24 || m.triangles() != 12) {
    return fail(W, tag + "geometry: " + std::to_string(m.primitives.size()) + " prims, " +
                       std::to_string(vertex_count(m)) + " verts, " + std::to_string(m.triangles()) + " tris");
  }
  if (m.skeleton.joints.size() != 2 || m.skeleton.joints[1].name != "arm" || m.skeleton.joints[1].parent != 0) {
    return fail(W, tag + "bones");
  }
  if (m.materials.size() != 2) return fail(W, tag + "materials " + std::to_string(m.materials.size()));
  // 16 x 32 x 48 inches (X, Y, Z up) -> 0.4064 x 1.2192 (Y up) x 0.8128 m.
  const Vec3 ext = m.bounds.extent();
  if (!near(ext.x, 0.4064f) || !near(ext.y, 1.2192f) || !near(ext.z, 0.8128f) || !near(m.bounds.min.y, 0.0f)) {
    return fail(W, tag + "bounds " + std::to_string(ext.x) + " " + std::to_string(ext.y) + " " + std::to_string(ext.z));
  }
  if (!windings_agree(m)) return fail(W, tag + "winding (expected counter-clockwise front faces)");
  const r3::MaterialDesc& crate = m.materials[0];
  if (!crate.base_color_image || crate.base_color_image->width != 8 || crate.base_color_image->rgba[0] != 0 ||
      crate.base_color_image->rgba[1] != 255 || crate.base_color_image->rgba[2] != 0) {
    return fail(W, tag + "crate VTF (DXT1 mip 0 green)");
  }
  if (!crate.normal_image || crate.normal_image->width != 2 || crate.normal_image->rgba[0] != 0 ||
      crate.normal_image->rgba[1] != 127 || crate.normal_image->rgba[2] != 255) {
    return fail(W, tag + "bump VTF (BGRA8888 via 7.4 resources, green flipped)");
  }
  if (!crate.double_sided) return fail(W, tag + "$nocull");
  if (m.materials[1].base_color_image != r3::missing_texture_image() || warn.find("missing_tex") == std::string::npos) {
    return fail(W, tag + "magenta placeholder for missing_tex (warn: " + warn + ")");
  }
  // Skin: the top face rides bone 1.
  const auto& prim = m.primitives[0];
  for (std::size_t k = 0; k < prim.mesh.vertices.size(); ++k) {
    const bool top = prim.mesh.vertices[k].position.y > 1.2f;
    if ((prim.mesh.skin[k].joints[0] == 1.0f) != top || !near(prim.mesh.skin[k].weights[0], 1.0f)) {
      return fail(W, tag + "skin weights");
    }
  }
  const int ci = m.find_clip("idle");
  if (ci < 0 || !near(m.clips[ci].duration, 0.1f)) return fail(W, tag + "sequence 'idle'");
  // Rest top 1.2192 m; frame 1 lifts the arm 16 inches (0.4064 m).
  const float rest_top = posed_top(m, nullptr, 0.0f), lifted = posed_top(m, &m.clips[ci], 0.1f);
  if (!near(rest_top, 1.2192f) || !near(lifted, 1.2192f + 0.4064f)) {
    return fail(W, tag + "animated skinning: rest " + std::to_string(rest_top) + ", lifted " + std::to_string(lifted));
  }
  if (m.collision_hulls.size() != 1 || m.collision_hulls[0].size() != 8) return fail(W, tag + ".phy hull");
  r3::Aabb hb{m.collision_hulls[0][0], m.collision_hulls[0][0]};
  for (const Vec3& q : m.collision_hulls[0]) {
    hb.min = {std::min(hb.min.x, q.x), std::min(hb.min.y, q.y), std::min(hb.min.z, q.z)};
    hb.max = {std::max(hb.max.x, q.x), std::max(hb.max.y, q.y), std::max(hb.max.z, q.z)};
  }
  if (!near(hb.min.y, m.bounds.min.y) || !near(hb.max.y, m.bounds.max.y) || !near(hb.max.z, m.bounds.max.z)) {
    return fail(W, tag + "hull bounds");
  }
  if (m.lods != 2 || m.skin_families != 2 || m.body_groups.size() != 2 || m.body_groups[1].options.size() != 2) {
    return fail(W, tag + "lods / skins / body groups");
  }
  // Hitboxes: per-bone boxes; the head box sits 32..40 inches up (Source
  // Z) = 0.8128..1.016 m (Y up) in the rest pose, x +-4 inches.
  if (m.hitbox_set != "default" || m.hitboxes.size() != 2 || m.hitboxes[1].bone != 1 ||
      m.hitboxes[1].bone_name != "arm" || m.hitboxes[1].group != 1 || m.hitboxes[1].name != "head_box" ||
      m.hitboxes[0].group != 3) {
    return fail(W, tag + "hitbox set (" + std::to_string(m.hitboxes.size()) + " boxes)");
  }
  {
    std::vector<r3::Mat4> hm;
    r3::hitbox_matrices(m, nullptr, &hm);
    const r3::Aabb head = r3::Aabb{m.hitboxes[1].min, m.hitboxes[1].max}.transformed(hm[1]);
    if (!near(head.min.y, 0.8128f) || !near(head.max.y, 1.016f) || !near(head.max.x, 0.1016f) ||
        !near(head.min.x, -0.1016f)) {
      return fail(W, tag + "head hitbox rest placement y " + std::to_string(head.min.y) + ".." +
                         std::to_string(head.max.y));
    }
    // Posed: the idle clip's frame 1 lifts the arm 16 inches; the head box follows.
    r3::Pose pose = r3::rest_pose(m.skeleton);
    r3::sample_clip(m.clips[ci], 0.1f, false, &pose);
    std::vector<r3::Mat4> pal;
    r3::skin_palette(m.skeleton, pose, &pal);
    r3::hitbox_matrices(m, &pal, &hm);
    const r3::Aabb lifted_head = r3::Aabb{m.hitboxes[1].min, m.hitboxes[1].max}.transformed(hm[1]);
    if (!near(lifted_head.min.y, 0.8128f + 0.4064f)) {
      return fail(W, tag + "head hitbox posed y " + std::to_string(lifted_head.min.y));
    }
  }
  return 0;
}

}  // namespace

int run_mdl_smoke() {
  const char* W = "mdl";
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "ts_smoke_models";
  fs::remove_all(root / "mdl", ec);
  const fs::path content = root / "mdl" / "content";
  const fs::path models = content / "models" / "ts_test";
  write_materials(content);
  for (int version : {44, 48, 49}) {
    const std::string base = "box" + std::to_string(version);
    write_source(models, base, build_source(version, version == 49, 1.0f));
    if (int r = check_source_model(models / (base + ".mdl"), version)) return r;
  }
  // Unsupported version and missing siblings fail with a reason.
  {
    SourceSet s = build_source(53, false, 1.0f);
    write_source(models, "box53", s);
    r3::Model m;
    std::string err;
    if (r3::load_model((models / "box53.mdl").string(), &m, &err) || err.find("44 - 49") == std::string::npos) {
      return fail(W, "version 53 should be refused (" + err + ")");
    }
    fs::remove(models / "box48.dx90.vtx", ec);
    spit(models / "box48.dx80.vtx", build_source(48, false, 1.0f).vtx);  // .dx80 fallback
    if (!r3::load_model((models / "box48.mdl").string(), &m, &err)) return fail(W, ".dx80.vtx fallback: " + err);
    fs::remove(models / "box48.dx80.vtx", ec);
    if (r3::load_model((models / "box48.mdl").string(), &m, &err) || err.find("vtx") == std::string::npos) {
      return fail(W, "missing .vtx should be reported (" + err + ")");
    }
  }
  // Settings: body group, skin, LOD, scale, up axis.
  {
    const std::string mdl = (models / "box49.mdl").string();
    r3::ModelImportOptions o;
    r3::read_model_options(mdl, &o);
    if (!near(o.unit_scale, 0.0254f) || o.up != r3::UpAxis::Z) return fail(W, "Source defaults (0.0254, Z up)");
    o.body_groups["hat"] = 1;
    o.skin = 1;
    r3::Model m;
    std::string warn;
    if (!r3::load_model(mdl, o, &m, &warn) || m.primitives.size() != 2 || vertex_count(m) != 29 ||
        m.primitives[0].material != 1 || m.primitives[1].material != 0 || m.body_groups[1].selected != 1) {
      return fail(W, "body group 'hat' = tophat + skin 1");
    }
    if (!near(m.bounds.max.y, 60 * 0.0254f)) return fail(W, "hat height");
    o = r3::ModelImportOptions{};
    o.unit_scale = 0.0254f;
    o.up = r3::UpAxis::Z;
    o.lod = 1;
    if (!r3::load_model(mdl, o, &m, &warn) || m.triangles() != 6) return fail(W, "LOD 1 (6 triangles)");
    o.lod = 0;
    o.unit_scale = 1.0f;
    o.up = r3::UpAxis::Y;  // keep Source's own axes
    if (!r3::load_model(mdl, o, &m, &warn) || !near(m.bounds.extent().z, 48.0f) || !near(m.bounds.extent().y, 32.0f)) {
      return fail(W, "unit scale 1 + no axis change");
    }
    o.animations = false;
    o.collision = false;
    if (!r3::load_model(mdl, o, &m, &warn) || !m.clips.empty() || !m.collision_hulls.empty()) {
      return fail(W, "animations / collision off");
    }
  }
  // Material search path: a loose copy finds nothing until pointed at the
  // content folder (as a user would point at garrysmod/).
  {
    const fs::path loose = root / "mdl" / "loose";
    write_source(loose, "crate", build_source(48, false, 1.0f));
    r3::ModelImportOptions o;
    r3::read_model_options((loose / "crate.mdl").string(), &o);
    r3::Model m;
    std::string warn;
    if (!r3::load_model((loose / "crate.mdl").string(), o, &m, &warn) ||
        m.materials[0].base_color_image != r3::missing_texture_image()) {
      return fail(W, "loose copy should miss its textures");
    }
    for (const fs::path& sp : {content, content / "materials"}) {
      o.material_paths = {(root / "mdl" / "nowhere").string(), sp.string()};
      if (!r3::load_model((loose / "crate.mdl").string(), o, &m, &warn) || !m.materials[0].base_color_image ||
          m.materials[0].base_color_image->width != 8) {
        return fail(W, "material search path " + sp.string());
      }
    }
  }
  // VTF formats decode (one pixel each).
  {
    struct Case { int fmt; std::vector<unsigned char> px; unsigned char want[4]; };
    const Case cases[] = {{0, {10, 20, 30, 40}, {10, 20, 30, 40}},  {12, {30, 20, 10, 40}, {10, 20, 30, 40}},
                          {2, {10, 20, 30}, {10, 20, 30, 255}},     {3, {30, 20, 10}, {10, 20, 30, 255}},
                          {11, {40, 10, 20, 30}, {10, 20, 30, 40}}, {1, {40, 30, 20, 10}, {10, 20, 30, 40}},
                          {16, {30, 20, 10, 0}, {10, 20, 30, 255}}, {5, {99}, {99, 99, 99, 255}},
                          {6, {99, 7}, {99, 99, 99, 7}}};
    for (const Case& c : cases) {
      Buf b;
      b.alloc(80);
      b.bytes(0, "VTF\0", 4);
      b.i32(4, 7);
      b.i32(8, 2);
      b.i32(12, 80);
      b.u16(16, 1);
      b.u16(18, 1);
      b.u16(24, 1);
      b.i32(52, c.fmt);
      b.u8(56, 1);
      b.i32(57, -1);
      b.u16(63, 1);
      const std::size_t o = b.alloc(c.px.size());
      b.bytes(o, c.px.data(), c.px.size());
      r3::Image im;
      std::string err;
      if (!r3::source::decode_vtf(b.d.data(), b.size(), &im, &err) || std::memcmp(im.rgba.data(), c.want, 4) != 0) {
        return fail(W, "VTF format " + std::to_string(c.fmt) + " " + err);
      }
    }
  }
  // Asset database: info, hot reload when only the .vvd changes, moves
  // carrying the siblings.
  {
    const fs::path proj = root / "mdl" / "proj";
    write_source(proj / "assets" / "props", "box", build_source(48, false, 1.0f));
    adb::AssetDb db;
    db.set_importer(adb::import_asset);
    db.open(proj.string());
    db.scan();
    const adb::AssetDb::Entry* e = db.by_path("assets/props/box.mdl");
    if (!e || e->meta.kind != adb::Kind::Model || !e->import_ok) {
      return fail(W, std::string("asset db entry: ") + (e ? e->import_error : "none"));
    }
    if (db.by_path("assets/props/box.vvd") || db.by_path("assets/props/box.phy")) return fail(W, "siblings got .meta files");
    const json::Value& info = e->meta.info;
    if (info.num("vertices", 0) != 24 || info.num("bones", 0) != 2 || info.num("materials", 0) != 2 ||
        info.str("format") != "Source MDL" || info.num("hulls", 0) != 1 || info.num("skins", 0) != 2 ||
        !info["body_groups"].is_array() || info.num("clips", 0) != 1) {
      return fail(W, "asset info: " + json::write(info));
    }
    if (!near(e->meta.settings.flt("unit_scale", 0), 0.0254f) || e->meta.settings.str("up_axis") != "z") {
      return fail(W, "asset default settings");
    }
    json::Value s = e->meta.settings;
    s.set("material_path", json::Value::string(content.string()));
    if (!db.set_settings("assets/props/box.mdl", s) || db.by_path("assets/props/box.mdl")->meta.info.str("warning").find("crate") != std::string::npos) {
      return fail(W, "material_path setting through the asset db");
    }
    const float h0 = db.by_path("assets/props/box.mdl")->meta.info["size"][1].as_float();
    // Hot reload: a bigger box in the .vvd only.
    const fs::path vvd = proj / "assets" / "props" / "box.vvd";
    spit(vvd, build_source(48, false, 2.0f).vvd);
    fs::last_write_time(vvd, fs::last_write_time(vvd, ec) + std::chrono::seconds(3), ec);
    const auto rep = db.scan();
    const float h1 = db.by_path("assets/props/box.mdl")->meta.info["size"][1].as_float();
    if (rep.changed.size() != 1 || rep.changed[0] != "assets/props/box.mdl" || !near(h1, h0 * 2.0f, 0.01f)) {
      return fail(W, "hot reload after the .vvd changed (" + std::to_string(h0) + " -> " + std::to_string(h1) + ")");
    }
    std::string err;
    if (!db.move("assets/props/box.mdl", "assets/moved/crate.mdl", &err) ||
        !fs::exists(proj / "assets/moved/crate.vvd") || !fs::exists(proj / "assets/moved/crate.dx90.vtx") ||
        !fs::exists(proj / "assets/moved/crate.phy") || fs::exists(proj / "assets/props/box.vvd") ||
        !db.reimport("assets/moved/crate.mdl")) {
      return fail(W, "move with siblings: " + err);
    }
  }
  std::cout << "[smoke] mdl OK (Source .mdl v44 / v48 / v49 + .vvd + .dx90 / .dx80 .vtx + .phy: 24 vertices, "
               "2 bones, 2 materials, Z-up inches -> Y-up metres, CW -> CCW winding, skin weights, sequence "
               "'idle' posing the arm through the palette, body groups, skins, LOD 1, VMT -> VTF (DXT1 mips, "
               "BGRA8888 7.4 resources, 9 more formats), magenta placeholder, material search path, 1 convex "
               "hull, asset info + hot reload from the .vvd + moves with siblings)\n";
  return 0;
}

namespace {

// 2x2 uncompressed 32-bit TGA, top-left origin: red, green / blue, white.
std::string tga_2x2() {
  std::string t(18, '\0');
  t[2] = 2;
  t[12] = 2;
  t[14] = 2;
  t[16] = 32;
  t[17] = 0x28;
  const unsigned char px[4][4] = {{0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}, {255, 255, 255, 255}};
  for (const auto& p : px) t.append(reinterpret_cast<const char*>(p), 4);
  return t;
}

const char* kObj =
    "# generated quad\nmtllib quad.mtl\no quad\n"
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
    "usemtl paint\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";

const char* kDae = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="meter" meter="1"/><up_axis>Z_UP</up_axis></asset>
  <library_geometries>
    <geometry id="bar-mesh" name="bar">
      <mesh>
        <source id="bar-pos">
          <float_array id="bar-pos-array" count="24">0 0 0 1 0 0 1 1 0 0 1 0 0 0 2 1 0 2 1 1 2 0 1 2</float_array>
          <technique_common><accessor source="#bar-pos-array" count="8" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common>
        </source>
        <vertices id="bar-verts"><input semantic="POSITION" source="#bar-pos"/></vertices>
        <triangles count="12"><input semantic="VERTEX" source="#bar-verts" offset="0"/>
          <p>0 2 1 0 3 2 4 5 6 4 6 7 0 1 5 0 5 4 1 2 6 1 6 5 2 3 7 2 7 6 3 0 4 3 4 7</p></triangles>
      </mesh>
    </geometry>
  </library_geometries>
  <library_controllers>
    <controller id="bar-skin"><skin source="#bar-mesh">
      <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>
      <source id="bar-joints"><Name_array id="bar-joints-array" count="2">root tip</Name_array>
        <technique_common><accessor source="#bar-joints-array" count="2" stride="1"><param name="JOINT" type="name"/></accessor></technique_common></source>
      <source id="bar-binds"><float_array id="bar-binds-array" count="32">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 1 0 0 0 0 1 0 0 0 0 1 -1 0 0 0 1</float_array>
        <technique_common><accessor source="#bar-binds-array" count="2" stride="16"><param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
      <source id="bar-weights"><float_array id="bar-weights-array" count="1">1</float_array>
        <technique_common><accessor source="#bar-weights-array" count="1" stride="1"><param name="WEIGHT" type="float"/></accessor></technique_common></source>
      <joints><input semantic="JOINT" source="#bar-joints"/><input semantic="INV_BIND_MATRIX" source="#bar-binds"/></joints>
      <vertex_weights count="8"><input semantic="JOINT" source="#bar-joints" offset="0"/><input semantic="WEIGHT" source="#bar-weights" offset="1"/>
        <vcount>1 1 1 1 1 1 1 1</vcount><v>0 0 0 0 0 0 0 0 1 0 1 0 1 0 1 0</v></vertex_weights>
    </skin></controller>
  </library_controllers>
  <library_animations>
    <animation id="tip-anim">
      <source id="tip-time"><float_array id="tip-time-array" count="2">0 1</float_array>
        <technique_common><accessor source="#tip-time-array" count="2" stride="1"><param name="TIME" type="float"/></accessor></technique_common></source>
      <source id="tip-out"><float_array id="tip-out-array" count="32">1 0 0 0 0 1 0 0 0 0 1 1 0 0 0 1 1 0 0 0 0 1 0 0 0 0 1 2 0 0 0 1</float_array>
        <technique_common><accessor source="#tip-out-array" count="2" stride="16"><param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
      <source id="tip-interp"><Name_array id="tip-interp-array" count="2">LINEAR LINEAR</Name_array>
        <technique_common><accessor source="#tip-interp-array" count="2" stride="1"><param name="INTERPOLATION" type="name"/></accessor></technique_common></source>
      <sampler id="tip-sampler"><input semantic="INPUT" source="#tip-time"/><input semantic="OUTPUT" source="#tip-out"/><input semantic="INTERPOLATION" source="#tip-interp"/></sampler>
      <channel source="#tip-sampler" target="tip/transform"/>
    </animation>
  </library_animations>
  <library_visual_scenes>
    <visual_scene id="Scene" name="Scene">
      <node id="root" name="root" sid="root" type="JOINT"><matrix sid="transform">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>
        <node id="tip" name="tip" sid="tip" type="JOINT"><matrix sid="transform">1 0 0 0 0 1 0 0 0 0 1 1 0 0 0 1</matrix></node>
      </node>
      <node id="bar" name="bar" type="NODE"><instance_controller url="#bar-skin"><skeleton>#root</skeleton></instance_controller></node>
    </visual_scene>
  </library_visual_scenes>
  <scene><instance_visual_scene url="#Scene"/></scene>
</COLLADA>
)";

const char* kFbx = R"(; FBX 7.3.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7300
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",100
	}
}
Definitions:  {
	Version: 100
	Count: 3
	ObjectType: "Model" {
		Count: 1
	}
	ObjectType: "Geometry" {
		Count: 1
	}
	ObjectType: "Material" {
		Count: 1
	}
}
Objects:  {
	Geometry: 1000, "Geometry::tri", "Mesh" {
		Vertices: *12 {
			a: 0,0,0,1,0,0,1,1,0,0,1,0
		}
		PolygonVertexIndex: *4 {
			a: 0,1,2,-4
		}
		GeometryVersion: 124
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "AllSame"
			ReferenceInformationType: "IndexToDirect"
			Materials: *1 {
				a: 0
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}
		}
	}
	Model: 2000, "Model::tri", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,0,0
		}
		Shading: T
		Culling: "CullingOff"
	}
	Material: 3000, "Material::red", "" {
		Version: 102
		ShadingModel: "phong"
		MultiLayer: 0
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",1,0,0
		}
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",1000,2000
	C: "OO",3000,2000
}
)";

const char* kPly =
    "ply\nformat ascii 1.0\ncomment generated\nelement vertex 4\nproperty float x\nproperty float y\n"
    "property float z\nelement face 2\nproperty list uchar int vertex_indices\nend_header\n"
    "0 0 0\n1 0 0\n1 1 0\n0 1 0\n3 0 1 2\n3 0 2 3\n";

const char* kStl =
    "solid quad\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n   vertex 1 1 0\n"
    "  endloop\n endfacet\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 1 0\n"
    "   vertex 0 1 0\n  endloop\n endfacet\nendsolid quad\n";

const char* kX =
    "xof 0303txt 0032\nMesh tri {\n 3;\n 0.0;0.0;0.0;,\n 1.0;0.0;0.0;,\n 0.0;1.0;0.0;;\n 1;\n 3;0,2,1;;\n}\n";

}  // namespace

int run_models_smoke() {
  if (int r = run_mdl_smoke()) return r;
  if (int r = run_bounds_smoke()) return r;
  const char* W = "models";
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / "ts_smoke_models" / "formats";
  fs::remove_all(dir, ec);
  for (const char* e : {"a.gltf", "a.glb", "a.MDL", "a.obj", "a.fbx", "a.dae", "a.3ds", "a.ply", "a.stl", "a.x", "a.blend", "a.smd"}) {
    if (!r3::is_model_file(e)) return fail(W, std::string("is_model_file ") + e);
  }
  if (r3::is_model_file("a.vvd") || r3::is_model_file("a.vtf") || r3::is_model_file("a.mtl")) return fail(W, "siblings are not models");
  std::string formats = "glTF (cgltf), Source MDL";
  if (!r3::assimp_available()) {
    std::cout << "[smoke] models: built without Assimp, skipping OBJ / FBX / DAE / PLY / STL / X\n";
  } else {
    auto load = [&](const fs::path& p, r3::Model* m, std::string* warn) {
      r3::ModelImportOptions o;
      r3::read_model_options(p.string(), &o);
      return r3::load_model(p.string(), o, m, warn);
    };
    // OBJ + MTL + TGA.
    spit(dir / "quad.obj", kObj);
    spit(dir / "quad.mtl", "newmtl paint\nKd 1 1 1\nmap_Kd tex/paint.tga\n");
    spit(dir / "tex" / "paint.tga", tga_2x2());
    r3::Model m;
    std::string warn;
    if (!load(dir / "quad.obj", &m, &warn)) return fail(W, "obj: " + warn);
    if (m.primitives.size() != 1 || vertex_count(m) != 4 || m.triangles() != 2) return fail(W, "obj geometry");
    const r3::MaterialDesc* paint = m.primitives[0].material >= 0 ? &m.materials[m.primitives[0].material] : nullptr;
    if (!paint || !paint->base_color_image || paint->base_color_image->width != 2 ||
        paint->base_color_image->rgba[0] != 255 || paint->base_color_image->rgba[1] != 0 ||
        paint->base_color_image->rgba[4 + 1] != 255) {
      return fail(W, "obj material texture (TGA via MTL)");
    }
    if (!windings_agree(m)) return fail(W, "obj winding");
    spit(dir / "quad.mtl", "newmtl paint\nKd 1 1 1\nmap_Kd gone.png\n");
    if (!load(dir / "quad.obj", &m, &warn) || m.materials[m.primitives[0].material].base_color_image != r3::missing_texture_image() ||
        warn.find("gone.png") == std::string::npos) {
      return fail(W, "obj missing texture -> magenta (" + warn + ")");
    }
    formats += ", OBJ";
    // Collada: Z up, skinned, animated.
    spit(dir / "bar.dae", kDae);
    if (!load(dir / "bar.dae", &m, &warn)) return fail(W, "dae: " + warn);
    if (vertex_count(m) != 8 || m.triangles() != 12 || m.skeleton.joints.size() < 2 || m.clips.empty() ||
        m.skeleton.find("tip") < 0) {
      return fail(W, "dae: " + std::to_string(vertex_count(m)) + " verts, " + std::to_string(m.skeleton.joints.size()) +
                         " joints, " + std::to_string(m.clips.size()) + " clips");
    }
    {
      const float rest = posed_top(m, nullptr, 0.0f), up = posed_top(m, &m.clips[0], 1.0f);
      if (!near(rest, 2.0f) || !near(up, 3.0f)) {
        return fail(W, "dae Z-up skinning: rest top " + std::to_string(rest) + ", animated " + std::to_string(up));
      }
    }
    formats += ", Collada (skinned + animated)";
    // FBX (ASCII 7.3).
    spit(dir / "tri.fbx", kFbx);
    if (!load(dir / "tri.fbx", &m, &warn)) return fail(W, "fbx: " + warn);
    if (vertex_count(m) < 4 || m.triangles() != 2 || m.materials.empty() || !near(m.materials[0].base_color[0], 1.0f) ||
        !near(m.materials[0].base_color[1], 0.0f)) {
      return fail(W, "fbx geometry / red material");
    }
    formats += ", FBX";
    // PLY + the shared settings (unit scale, up axis).
    spit(dir / "quad.ply", kPly);
    if (!load(dir / "quad.ply", &m, &warn) || vertex_count(m) != 4 || m.triangles() != 2) return fail(W, "ply: " + warn);
    {
      r3::ModelImportOptions o;
      o.unit_scale = 2.0f;
      o.up = r3::UpAxis::Z;
      if (!r3::load_model((dir / "quad.ply").string(), o, &m, &warn) || !near(m.bounds.extent().x, 2.0f) ||
          !near(m.bounds.extent().y, 0.0f) || !near(m.bounds.extent().z, 2.0f)) {
        return fail(W, "ply unit scale 2 + Z up");
      }
    }
    formats += ", PLY";
    spit(dir / "quad.stl", kStl);
    if (!load(dir / "quad.stl", &m, &warn) || m.triangles() != 2 || vertex_count(m) > 6) return fail(W, "stl: " + warn);
    formats += ", STL";
    spit(dir / "tri.x", kX);
    if (!load(dir / "tri.x", &m, &warn) || m.triangles() != 1) return fail(W, "x: " + warn);
    formats += ", DirectX .x";
    // Asset database: OBJ info + hot reload from the .mtl.
    {
      const fs::path proj = dir / "proj";
      spit(proj / "assets" / "quad.obj", kObj);
      spit(proj / "assets" / "quad.mtl", "newmtl paint\nKd 1 0 0\n");
      adb::AssetDb db;
      db.set_importer(adb::import_asset);
      db.open(proj.string());
      db.scan();
      const auto* e = db.by_path("assets/quad.obj");
      if (!e || !e->import_ok || e->meta.info.num("vertices", 0) != 4 || e->meta.info.str("format") != "Wavefront OBJ" ||
          e->meta.settings.str("up_axis") != "auto" || !near(e->meta.settings.flt("unit_scale", 0), 1.0f)) {
        return fail(W, "obj asset entry");
      }
      const fs::path mtl = proj / "assets" / "quad.mtl";
      spit(mtl, "newmtl paint\nKd 0 1 0\n");
      fs::last_write_time(mtl, fs::last_write_time(mtl, ec) + std::chrono::seconds(3), ec);
      const auto rep = db.scan();
      if (rep.changed.size() != 1 || rep.changed[0] != "assets/quad.obj") return fail(W, "hot reload after the .mtl changed");
    }
  }
  // .blend without Blender: the friendly message.
  {
    spit(dir / "thing.blend", std::string("BLENDER-v300") + std::string(64, '\0'));
    r3::ModelImportOptions o;
    o.blender_path = (dir / "no_blender_here" / "blender.exe").string();
    r3::Model m;
    std::string err;
    if (r3::load_model((dir / "thing.blend").string(), o, &m, &err) || err.find("Install Blender") == std::string::npos ||
        err.find("glTF") == std::string::npos) {
      return fail(W, ".blend without Blender should explain (" + err + ")");
    }
  }
  std::cout << "[smoke] models OK (" << formats << "; .blend without Blender explains itself"
            << (r3::find_blender("").empty() ? "" : "; Blender found at " + r3::find_blender(""))
            << "; unit scale + up axis shared by every format; hot reload)\n";
  return 0;
}
