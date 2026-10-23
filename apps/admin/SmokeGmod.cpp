// Source engine formats smoke (see SmokeGmod.h). All fixtures generated here.
#include "SmokeGmod.h"
#include "SmokeModels.h"

#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "editor/assets/GmaImport.h"
#include "export/Exporter.h"
#include "export/Provenance.h"
#include "render3d/ModelImport.h"
#include "render3d/import/Gma.h"
#include "render3d/import/Lzma.h"
#include "render3d/import/SourceFormats.h"
#include "render3d/import/SourceMaterials.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {

namespace {

namespace fs = std::filesystem;
namespace r3 = render3d;
namespace src = render3d::source;
namespace gma = render3d::gma;
namespace prov = exporter::provenance;
using Bytes = std::vector<unsigned char>;

int fail(const std::string& what) {
  std::cerr << "[smoke] gmod FAILED: " << what << '\n';
  return 1;
}

void spit(const fs::path& p, const Bytes& d) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}
void spit(const fs::path& p, const std::string& s) { spit(p, Bytes(s.begin(), s.end())); }
Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

void put16(Bytes& b, std::size_t o, unsigned v) {
  b[o] = static_cast<unsigned char>(v);
  b[o + 1] = static_cast<unsigned char>(v >> 8);
}
void put32(Bytes& b, std::size_t o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[o + i] = static_cast<unsigned char>(v >> (8 * i));
}

// A VTF: header (7.0-7.2: 80 bytes, no thumbnail; 7.3+: resource table
// with the image resource) followed by `images` in file order.
Bytes make_vtf(int minor, int fmt, int w, int h, int mips, int frames, std::uint32_t flags, int first_frame,
               const Bytes& images) {
  const bool res = minor >= 3;
  const std::size_t header = res ? 80 + 8 : 80;
  Bytes b(header, 0);
  std::memcpy(b.data(), "VTF\0", 4);
  put32(b, 4, 7);
  put32(b, 8, static_cast<std::uint32_t>(minor));
  put32(b, 12, static_cast<std::uint32_t>(header));
  put16(b, 16, static_cast<unsigned>(w));
  put16(b, 18, static_cast<unsigned>(h));
  put32(b, 20, flags);
  put16(b, 24, static_cast<unsigned>(frames));
  put16(b, 26, static_cast<unsigned>(first_frame));
  put32(b, 52, static_cast<std::uint32_t>(fmt));
  b[56] = static_cast<unsigned char>(mips);
  put32(b, 57, 0xFFFFFFFFu);  // no low-res image
  if (minor >= 2) put16(b, 63, 1);
  if (res) {
    put32(b, 68, 1);
    b[80] = 0x30;
    put32(b, 84, static_cast<std::uint32_t>(header));
  }
  b.insert(b.end(), images.begin(), images.end());
  return b;
}

bool px(const r3::Image& im, int i, int r, int g, int bl, int a) {
  const unsigned char* p = &im.rgba[static_cast<std::size_t>(i) * 4];
  return std::abs(p[0] - r) <= 1 && std::abs(p[1] - g) <= 1 && std::abs(p[2] - bl) <= 1 && std::abs(p[3] - a) <= 1;
}

std::string pxs(const r3::Image& im, int i) {
  const unsigned char* p = &im.rgba[static_cast<std::size_t>(i) * 4];
  return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," + std::to_string(p[3]);
}

int vtf_checks() {
  // Uncompressed formats: two pixels each.
  struct Case {
    int fmt;
    Bytes data;
    int want[4];
  };
  const Case cases[] = {
      {0, {10, 20, 30, 40, 10, 20, 30, 40}, {10, 20, 30, 40}},      // RGBA8888
      {1, {40, 30, 20, 10, 40, 30, 20, 10}, {10, 20, 30, 40}},      // ABGR8888
      {2, {10, 20, 30, 10, 20, 30}, {10, 20, 30, 255}},             // RGB888
      {3, {30, 20, 10, 30, 20, 10}, {10, 20, 30, 255}},             // BGR888
      {5, {77, 77}, {77, 77, 77, 255}},                             // I8
      {6, {77, 99, 77, 99}, {77, 77, 77, 99}},                      // IA88
      {8, {99, 99}, {255, 255, 255, 99}},                           // A8
      {12, {30, 20, 10, 40, 30, 20, 10, 40}, {10, 20, 30, 40}},     // BGRA8888
      {16, {30, 20, 10, 0, 30, 20, 10, 0}, {10, 20, 30, 255}},      // BGRX8888
      {22, {100, 200, 100, 200}, {100, 200, 255, 255}},             // UV88
      {19, {0x23, 0x41, 0x23, 0x41}, {17, 34, 51, 68}},             // BGRA4444
  };
  for (const Case& c : cases) {
    for (int minor : {1, 4, 5}) {
      const Bytes v = make_vtf(minor, c.fmt, 2, 1, 1, 1, 0, 0, c.data);
      r3::Image im;
      std::string err;
      if (!src::decode_vtf(v.data(), v.size(), &im, &err)) {
        return fail("VTF 7." + std::to_string(minor) + " " + src::vtf_format_name(c.fmt) + ": " + err);
      }
      if (im.width != 2 || im.height != 1 || !px(im, 1, c.want[0], c.want[1], c.want[2], c.want[3])) {
        return fail("VTF " + src::vtf_format_name(c.fmt) + " pixel " + pxs(im, 1));
      }
    }
  }
  // DXT: one 4x4 block, colour 0 red / colour 1 blue, indices 0,1,2,3...
  Bytes color = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};  // c0 0xF800, c1 0x001F
  {
    const Bytes v = make_vtf(2, 13, 4, 4, 1, 1, 0, 0, color);
    r3::Image im;
    std::string err;
    if (!src::decode_vtf(v.data(), v.size(), &im, &err)) return fail("DXT1: " + err);
    if (!px(im, 0, 255, 0, 0, 255) || !px(im, 1, 0, 0, 255, 255) || !px(im, 2, 170, 0, 85, 255)) {
      return fail("DXT1 pixels " + pxs(im, 0) + " / " + pxs(im, 1) + " / " + pxs(im, 2));
    }
  }
  {
    Bytes block = {0xFF, 0x00, 0x88, 0, 0, 0, 0, 0};  // a0 255, a1 0; indices 0,1,2 (3 bits each)
    block.insert(block.end(), color.begin(), color.end());
    const Bytes v = make_vtf(5, 15, 4, 4, 1, 1, 0, 0, block);
    r3::Image im;
    std::string err;
    if (!src::decode_vtf(v.data(), v.size(), &im, &err)) return fail("DXT5: " + err);
    if (!px(im, 0, 255, 0, 0, 255) || !px(im, 1, 0, 0, 255, 0) || !px(im, 2, 170, 0, 85, 218)) {
      return fail("DXT5 pixels " + pxs(im, 0) + " / " + pxs(im, 1) + " / " + pxs(im, 2));
    }
  }
  {
    Bytes block = {0x0F, 0, 0, 0, 0, 0, 0, 0};  // pixel 0 alpha 15, pixel 1 alpha 0
    block.insert(block.end(), color.begin(), color.end());
    const Bytes v = make_vtf(3, 14, 4, 4, 1, 1, 0, 0, block);
    r3::Image im;
    std::string err;
    if (!src::decode_vtf(v.data(), v.size(), &im, &err) || !px(im, 0, 255, 0, 0, 255) || !px(im, 1, 0, 0, 255, 0)) {
      return fail("DXT3 alpha " + err);
    }
  }
  // Mips x frames: mip 1 (frame 0, 1) then mip 0 (frame 0, 1); want mip 0 frame 0.
  {
    Bytes d;
    for (unsigned char c : {1, 2}) d.insert(d.end(), {c, c, c, 255});                  // mip 1: 1x1 x 2 frames
    for (unsigned char c : {200, 50}) for (int i = 0; i < 4; ++i) d.insert(d.end(), {c, c, c, 255});  // mip 0 2x2
    const Bytes v = make_vtf(4, 0, 2, 2, 2, 2, 0, 0, d);
    r3::Image im;
    std::string err;
    if (!src::decode_vtf(v.data(), v.size(), &im, &err) || !px(im, 3, 200, 200, 200, 255)) {
      return fail("VTF mips + frames: want mip 0 frame 0 " + err);
    }
  }
  // Cube maps: 7.1 with a sphere map (7 faces), 7.5 (6 faces), 2 frames, 2 mips.
  for (int minor : {1, 5}) {
    const int faces = minor < 5 ? 7 : 6;
    Bytes d;
    for (int f = 0; f < 2 * faces; ++f) d.insert(d.end(), {9, 9, 9, 255});  // mip 1
    for (int f = 0; f < 2 * faces; ++f) {
      const unsigned char c = f == 0 ? 240 : 30;
      for (int i = 0; i < 4; ++i) d.insert(d.end(), {c, 0, 0, 255});
    }
    const Bytes v = make_vtf(minor, 0, 2, 2, 2, 2, 0x4000u, 0, d);
    r3::Image im;
    std::string err;
    src::VtfInfo vi;
    if (!src::vtf_info(v.data(), v.size(), &vi, &err) || vi.faces != faces || !vi.envmap()) {
      return fail("VTF cube faces 7." + std::to_string(minor));
    }
    if (!src::decode_vtf(v.data(), v.size(), &im, &err) || !px(im, 0, 240, 0, 0, 255)) {
      return fail("VTF cube map 7." + std::to_string(minor) + " first face " + err);
    }
  }
  // Truncated / wrong version / unknown format fail with a reason.
  {
    Bytes v = make_vtf(4, 0, 64, 64, 1, 1, 0, 0, Bytes(16, 0));
    r3::Image im;
    std::string err;
    if (src::decode_vtf(v.data(), v.size(), &im, &err) || err.find("truncated") == std::string::npos) {
      return fail("truncated VTF accepted (" + err + ")");
    }
    v = make_vtf(4, 0, 1, 1, 1, 1, 0, 0, Bytes(4, 0));
    put32(v, 8, 6);
    if (src::decode_vtf(v.data(), v.size(), &im, &err) || err.find("version") == std::string::npos) {
      return fail("VTF 7.6 accepted");
    }
  }
  return 0;
}

int vmt_checks() {
  // Comments, unquoted tokens, case-insensitive keys, conditionals,
  // fallback blocks, proxies ignored.
  const std::string base =
      "// generated\n\"VertexLitGeneric\"\n{\n  \"$BaseTexture\" \"models/x/base\" // trailing\n"
      "  $Color \"[1 0.5 0.25]\"\n  \"$alphatest\" \"1\" [$X360]\n  \"$phong\" 1\n"
      "  \">=dx90\" { \"$bumpmap\" \"models/x/base_n\" }\n"
      "  \"Proxies\" { \"AnimatedTexture\" { \"animatedtexturevar\" \"$basetexture\" } }\n}\n";
  src::Vmt v;
  std::string err;
  if (!src::parse_vmt(base, &v, &err) || v.shader != "vertexlitgeneric" || v.param("$basetexture") != "models/x/base" ||
      v.param("$color") != "[1 0.5 0.25]" || !v.flag("$alphatest") || v.param("$bumpmap") != "models/x/base_n" ||
      v.params.count("animatedtexturevar")) {
    return fail("VMT parse (" + err + ")");
  }
  // Patch chain: top -> mid (patch) -> base.
  const std::string mid =
      "patch { include \"materials/models/x/base.vmt\" insert { \"$nocull\" 1 \"$basetexture\" \"ignored\" } "
      "replace { \"$alphatest\" 0 } }";
  const std::string top =
      "\"Patch\"\n{\n\t\"include\" \"materials\\\\models\\\\x\\\\mid.vmt\"\n\t\"replace\"\n\t{\n\t\t\"$basetexture\" "
      "\"models/x/replaced\"\n\t}\n\t\"insert\" { \"$translucent\" \"1\" \"$nocull\" \"0\" }\n}\n";
  std::vector<std::string> chain;
  const bool ok = src::resolve_vmt(
      top,
      [&](const std::string& inc, std::string* t) {
        const std::string c = src::clean_material_name(inc);
        if (c == "models/x/mid") *t = mid;
        else if (c == "models/x/base") *t = base;
        else return false;
        return true;
      },
      &v, &err, &chain);
  if (!ok || v.shader != "vertexlitgeneric" || chain.size() != 2) return fail("VMT patch chain (" + err + ")");
  // replace wins; inner insert added ($nocull 1 from mid, before the outer
  // insert, which only adds when missing); inner replace turns alphatest off.
  if (v.param("$basetexture") != "models/x/replaced" || v.param("$nocull") != "1" || v.flag("$alphatest") ||
      !v.flag("$translucent") || v.param("$color") != "[1 0.5 0.25]") {
    return fail("VMT patch insert / replace: basetexture " + v.param("$basetexture") + " nocull " + v.param("$nocull"));
  }
  // Missing include: fails, names it, keeps the patch's own params.
  if (src::resolve_vmt(top, [](const std::string&, std::string*) { return false; }, &v, &err) ||
      err.find("mid.vmt") == std::string::npos || v.param("$basetexture") != "models/x/replaced") {
    return fail("VMT missing include: " + err);
  }
  return 0;
}

Bytes tiny_wav() {
  Bytes w;
  auto s = [&](const char* t) { w.insert(w.end(), t, t + 4); };
  auto u = [&](std::uint32_t v, int n) {
    for (int i = 0; i < n; ++i) w.push_back(static_cast<unsigned char>(v >> (8 * i)));
  };
  const int n = 64;
  s("RIFF");
  u(36 + n * 2, 4);
  s("WAVE");
  s("fmt ");
  u(16, 4);
  u(1, 2);
  u(1, 2);
  u(22050, 4);
  u(22050 * 2, 4);
  u(2, 2);
  u(16, 2);
  s("data");
  u(n * 2, 4);
  for (int i = 0; i < n; ++i) u(static_cast<std::uint32_t>(static_cast<std::int16_t>((i % 8) * 1000)), 2);
  return w;
}

}  // namespace

int run_gmod_smoke() {
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "ts_smoke_gmod";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  // CRC32 / LZMA basics.
  {
    const std::string t = "123456789";
    if (gma::crc32(reinterpret_cast<const unsigned char*>(t.data()), t.size()) != 0xCBF43926u) return fail("crc32");
    // A stream with matches + an end marker (made by a stock LZMA encoder
    // from our own text), and the literal-only encoder with a known size.
    const unsigned char blob[] = {0x5d, 0x00, 0x00, 0x00, 0x04, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
                                  0x2a, 0x1b, 0xc9, 0xa6, 0x34, 0x82, 0x9f, 0x15, 0x63, 0x2a, 0xac, 0x9c, 0xed, 0xbe,
                                  0xfb, 0x3c, 0xb4, 0x66, 0x5e, 0xbe, 0x8e, 0x45, 0x9e, 0x26, 0x7a, 0xaa, 0x5f, 0xbc,
                                  0x72, 0x16, 0x26, 0x34, 0x4a, 0x9e, 0xef, 0x18, 0xee, 0xff, 0xf4, 0x17, 0xc0, 0x00};
    std::string want = "TombStone smoke: ";
    for (int i = 0; i < 20; ++i) want += "abcabcabcabc ";
    want += "the end.";
    Bytes out;
    std::string err;
    if (!r3::lzma::decode_alone(blob, sizeof(blob), &out, &err) || std::string(out.begin(), out.end()) != want) {
      return fail("LZMA decode (matches, end marker): " + err);
    }
    const Bytes lit = r3::lzma::encode_alone_literals(reinterpret_cast<const unsigned char*>(want.data()), want.size());
    if (!r3::lzma::decode_alone(lit.data(), lit.size(), &out, &err) || std::string(out.begin(), out.end()) != want) {
      return fail("LZMA decode (known size): " + err);
    }
    if (r3::lzma::decode_alone(blob, 30, &out, &err)) return fail("truncated LZMA accepted");
  }
  if (int r = vtf_checks()) return r;
  if (int r = vmt_checks()) return r;

  // ---- the synthetic addon -------------------------------------------------
  // Model: $cdmaterials "models/ts_gmod_first/" then "models/ts_gmod/";
  // materials "crate" (only in ts_gmod: second cd path) and "glass" (in
  // both: the first cd path must win).
  const SmokeSourceModel mdl = smoke_source_model({"models\\ts_gmod_first\\", "models/ts_gmod"}, "crate", "glass");
  const Bytes dxt1 = smoke_vtf_dxt1_8x8_green(), bgra = smoke_vtf_bgra_2x2();
  std::vector<gma::NewFile> files = {
      {"models/ts_gmod/crate.mdl", mdl.mdl},
      {"models/ts_gmod/crate.vvd", mdl.vvd},
      {"models/ts_gmod/crate.dx90.vtx", mdl.vtx},
      {"models/ts_gmod/crate.phy", mdl.phy},
      {"materials/models/ts_gmod/base_crate.vmt",
       bytes("\"VertexLitGeneric\"\n{\n\t\"$basetexture\" \"models/ts_gmod/crate_dxt1\"\n\t\"$color2\" \"[1 0.5 0.25]\"\n}\n")},
      {"materials/models/ts_gmod/crate.vmt",
       bytes("patch\n{\n\tinclude \"materials/models/ts_gmod/base_crate.vmt\"\n\treplace { \"$basetexture\" "
             "\"models\\\\ts_gmod\\\\crate_bgra\" }\n\tinsert { \"$nocull\" \"1\" }\n}\n")},
      {"materials/models/ts_gmod_first/glass.vmt",
       bytes("UnlitGeneric { $basetexture models/ts_gmod/crate_dxt1 $translucent 1 }")},
      {"materials/models/ts_gmod/glass.vmt", bytes("VertexLitGeneric { $basetexture models/ts_gmod/crate_bgra $alphatest 1 }")},
      {"materials/models/ts_gmod/crate_dxt1.vtf", dxt1},
      {"materials/models/ts_gmod/crate_bgra.vtf", bgra},
      {"sound/ts_gmod/beep.wav", tiny_wav()},
      {"lua/autorun/ts_gmod.lua", bytes("print('hello')\n")},
      {"../evil.txt", bytes("nope")},
  };
  const Bytes addon = gma::write("TS Smoke Addon!", "{\"description\":\"generated\",\"type\":\"model\",\"tags\":[\"fun\"]}",
                                 "TombStone smoke", files, 76561190000000000ull, 1700000000ull);
  const fs::path gma_path = root / "ts_smoke_addon.gma";
  spit(gma_path, addon);
  gma::Archive a;
  std::string err;
  if (!gma::read_file(gma_path.string(), &a, &err)) return fail("read .gma: " + err);
  if (a.version != 3 || a.name != "TS Smoke Addon!" || a.author != "TombStone smoke" || a.type != "model" ||
      a.tags.size() != 1 || a.files.size() != files.size() || a.lzma || !gma::archive_crc_ok(a) ||
      a.steamid != 76561190000000000ull) {
    return fail(".gma header / table");
  }
  for (std::size_t i = 0; i < a.files.size(); ++i) {
    if (a.files[i].name != files[i].name || a.files[i].size != files[i].data.size() || !gma::entry_crc_ok(a, a.files[i]) ||
        std::memcmp(a.data(a.files[i]), files[i].data.data(), files[i].data.size()) != 0) {
      return fail(".gma entry " + files[i].name);
    }
  }
  if (gma::category("Models/x.mdl") != "models" || gma::category("materials/a.vtf") != "materials" ||
      gma::category("sound/b.wav") != "sound" || gma::category("lua/c.lua") != "other") {
    return fail(".gma categories");
  }
  if (!gma::safe_relative_path("../evil.txt").empty() || !gma::safe_relative_path("C:/x").empty() ||
      !gma::safe_relative_path("/abs").empty() || gma::safe_relative_path("models\\a\\b.mdl") != "models/a/b.mdl") {
    return fail(".gma path safety");
  }
  // Corrupt one byte of a file's data: its CRC fails, the rest is fine.
  {
    Bytes bad = addon;
    bad[a.files[8].offset + 70] ^= 0x5A;  // crate_dxt1.vtf
    gma::Archive b;
    if (!gma::read(bad, &b, &err) || gma::entry_crc_ok(b, b.files[8]) || !gma::entry_crc_ok(b, b.files[9]) ||
        gma::archive_crc_ok(b)) {
      return fail(".gma CRC mismatch not detected");
    }
    Bytes cut(addon.begin(), addon.begin() + static_cast<std::ptrdiff_t>(addon.size() / 2));
    if (gma::read(cut, &b, &err) || err.find("truncated") == std::string::npos) return fail("cut .gma accepted: " + err);
  }
  // LZMA-compressed download (.bin).
  {
    const fs::path bin = root / "ts_smoke_addon_download.bin";
    spit(bin, r3::lzma::encode_alone_literals(addon.data(), addon.size()));
    gma::Archive c;
    if (!gma::read_file(bin.string(), &c, &err) || !c.lzma || c.files.size() != a.files.size() ||
        !gma::entry_crc_ok(c, c.files[0])) {
      return fail("LZMA .gma: " + err);
    }
    Bytes junk(64, 0x42);
    if (gma::read(junk, &c, &err) || err.find("GMAD") == std::string::npos) return fail("junk accepted as .gma");
  }

  // ---- extraction into a claim ---------------------------------------------
  const fs::path claim = root / "claim";
  spit(claim / "project.json", std::string("{\"name\": \"Gmod Smoke\", \"dimension\": \"3d\", \"version\": \"1.0.0\"}\n"));
  editor::gmaimport::Plan plan;
  if (!editor::gmaimport::open(gma_path.string(), &plan, &err) || plan.bad_crc != 0) return fail("plan: " + err);
  if (editor::gmaimport::addon_folder_name(plan) != "TS_Smoke_Addon") return fail("addon folder " + editor::gmaimport::addon_folder_name(plan));
  int unsafe = 0;
  for (const auto& it : plan.items) unsafe += it.safe ? 0 : 1;
  if (unsafe != 1) return fail("unsafe items " + std::to_string(unsafe));
  // Pick models only, then materials + sound: the .mdl brings its siblings.
  editor::gmaimport::select(&plan, "", false);
  for (auto& it : plan.items) it.selected = it.name == "models/ts_gmod/crate.mdl";
  editor::gmaimport::select(&plan, "materials", true);
  editor::gmaimport::select(&plan, "sound", true);
  editor::gmaimport::Result res;
  if (!editor::gmaimport::extract(claim.string(), plan, "", &res) || !res.errors.empty()) return fail("extract");
  const std::string dest = "assets/Models/TS_Smoke_Addon";
  if (res.dest != dest || res.models.size() != 1 || res.models[0] != dest + "/models/ts_gmod/crate.mdl" ||
      !fs::exists(claim / dest / "models/ts_gmod/crate.vvd") || !fs::exists(claim / dest / "models/ts_gmod/crate.dx90.vtx") ||
      !fs::exists(claim / dest / "materials/models/ts_gmod/crate_bgra.vtf") || !fs::exists(claim / dest / "sound/ts_gmod/beep.wav") ||
      fs::exists(claim / dest / "lua") || fs::exists(claim / "assets/Models/evil.txt") || fs::exists(claim / "evil.txt")) {
    return fail("extracted files (" + std::to_string(res.written.size()) + " written)");
  }
  if (editor::gmaimport::default_destination(claim.string(), plan) != dest + "_2") return fail("second import folder");
  // Re-extract into the same folder: nothing overwritten.
  {
    editor::gmaimport::Result again;
    editor::gmaimport::extract(claim.string(), plan, dest, &again);
    if (!again.written.empty() || again.skipped.empty()) return fail("re-extract overwrote files");
  }
  // Provenance ledger.
  {
    prov::Ledger l;
    if (!prov::load(claim.string(), &l)) return fail("provenance ledger missing");
    const auto it = l.assets.find(dest + "/models/ts_gmod/crate.mdl");
    if (it == l.assets.end() || it->second.origin != "gma:TS Smoke Addon!" || it->second.author != "TombStone smoke" ||
        it->second.format != "source-engine" || l.assets.size() != res.written.size()) {
      return fail("provenance records");
    }
  }

  // ---- materials through the search order ----------------------------------
  const fs::path model = claim / dest / "models/ts_gmod/crate.mdl";
  const fs::path extra = root / "search_folder";  // stands in for a game's content folder
  spit(extra / "garrysmod/materials/models/ts_gmod/crate.vmt",
       std::string("VertexLitGeneric { $basetexture models/ts_gmod/elsewhere }"));  // the claim's must win
  spit(extra / "garrysmod/addons/some_pack/materials/models/ts_gmod_first/lonely.vmt",
       std::string("VertexLitGeneric { \"$basetexture\" \"models/ts_gmod/lonely\" }"));
  spit(extra / "garrysmod/addons/some_pack/materials/models/ts_gmod/lonely.vtf", bgra);
  src::set_content_search_folders(src::split_folder_list(" " + extra.string() + " ;; "));
  {
    const auto roots = src::content_roots(model.string(), {});
    std::string order;
    for (const auto& r : roots) order += r.why + " | ";
    auto pos = [&](const std::string& why) { return order.find(why); };
    if (roots.empty() || fs::u8path(roots[0].dir).filename() != "TS_Smoke_Addon" || pos("claim") == std::string::npos ||
        pos("Source content search folder") == std::string::npos || pos("claim") > pos("Source content search folder") ||
        pos("(addons)") == std::string::npos) {
      return fail("content root order: " + order);
    }
  }
  r3::Model m;
  std::string warn;
  if (!r3::load_model(model.string(), &m, &warn)) return fail("load extracted model: " + warn);
  if (m.materials.size() != 2 || m.material_sources.size() != 2) return fail("material count");
  {
    const r3::MaterialDesc& crate = m.materials[0];
    // Patch: replace -> BGRA (orange 0,128,255 BGRA = rgb 0,128,255? bytes B=255 G=128 R=0), insert $nocull,
    // base's $color2 tint.
    if (!crate.base_color_image || crate.base_color_image->width != 2 || !px(*crate.base_color_image, 0, 0, 128, 255, 255) ||
        !crate.double_sided || std::fabs(crate.base_color[1] - 0.5f) > 1e-3f || std::fabs(crate.base_color[2] - 0.25f) > 1e-3f ||
        m.material_sources[0].status != "ok" || m.material_sources[0].material.find("ts_gmod") == std::string::npos ||
        m.material_sources[0].material.find("search_folder") != std::string::npos) {
      return fail("crate material via patch (" + m.material_sources[0].status + " " + m.material_sources[0].material + ")");
    }
    const r3::MaterialDesc& glass = m.materials[1];
    if (glass.alpha_mode != r3::MaterialDesc::kAlphaBlend || !glass.unlit || !glass.base_color_image ||
        glass.base_color_image->width != 8 || m.material_sources[1].material.find("ts_gmod_first") == std::string::npos) {
      return fail("$cdmaterials order: glass should come from ts_gmod_first (" + m.material_sources[1].material + ")");
    }
  }
  // Missing material: logged with the files and folders tried; the search
  // folder's addons/ resolve one; a relink fixes another.
  {
    const SmokeSourceModel m2 = smoke_source_model({"models\\ts_gmod_first\\"}, "lonely", "nothere");
    const fs::path dir = claim / dest / "models/ts_gmod";
    spit(dir / "second.mdl", m2.mdl);
    spit(dir / "second.vvd", m2.vvd);
    spit(dir / "second.dx90.vtx", m2.vtx);
    r3::Model mm;
    if (!r3::load_model((dir / "second.mdl").string(), &mm, &warn)) return fail("second model: " + warn);
    const r3::MaterialSource& lonely = mm.material_sources[0];
    const r3::MaterialSource& nothere = mm.material_sources[1];
    if (lonely.status != "ok" || lonely.material.find("some_pack") == std::string::npos) {
      return fail("search folder addons/ lookup: " + lonely.status + " " + lonely.searched);
    }
    if (nothere.status != "missing" || mm.materials[1].base_color_image != r3::missing_texture_image() ||
        nothere.searched.find("materials/models/ts_gmod_first/nothere.vmt") == std::string::npos ||
        nothere.searched.find("(claim)") == std::string::npos || nothere.searched.find("search_folder") == std::string::npos ||
        warn.find("nothere") == std::string::npos) {
      return fail("missing-material log: " + nothere.searched);
    }
    r3::ModelImportOptions o;
    r3::read_model_options((dir / "second.mdl").string(), &o);
    o.material_overrides["nothere"] = "../../materials/models/ts_gmod/crate.vmt";
    if (!r3::load_model((dir / "second.mdl").string(), o, &mm, &warn) || mm.material_sources[1].status != "override" ||
        !mm.materials[1].base_color_image || mm.materials[1].base_color_image->width != 2) {
      return fail("relink override: " + mm.material_sources[1].status + " " + mm.material_sources[1].searched);
    }
  }
  src::set_content_search_folders({});
  {
    // Without the search folder, 'lonely' is missing again.
    r3::Model mm;
    r3::load_model((claim / dest / "models/ts_gmod/second.mdl").string(), &mm, &warn);
    if (mm.material_sources.empty() || mm.material_sources[0].status != "missing") return fail("search folder setting not applied");
  }

  // ---- asset database ------------------------------------------------------
  {
    editor::assetdb::AssetDb db;
    db.set_importer(editor::assetdb::import_asset);
    db.open(claim.string());
    db.scan();
    const auto* e = db.by_path(dest + "/models/ts_gmod/crate.mdl");
    const auto* t = db.by_path(dest + "/materials/models/ts_gmod/crate_dxt1.vtf");
    const auto* s = db.by_path(dest + "/sound/ts_gmod/beep.wav");
    if (!e || !e->import_ok || e->meta.info.str("origin") != "source-engine" ||
        e->meta.info["source_materials"].size() != 2 || e->meta.info.num("importer_version", 0) < 3) {
      return fail("asset db model info " + (e ? e->import_error : std::string("(no entry)")));
    }
    if (!t || t->meta.kind != editor::assetdb::Kind::Texture || !t->import_ok || t->meta.info.num("width", 0) != 8 ||
        t->meta.info.str("format").find("DXT1") == std::string::npos) {
      return fail("asset db .vtf texture");
    }
    if (!s || !s->import_ok) return fail("asset db sound");
    const auto* sec = db.by_path(dest + "/models/ts_gmod/second.mdl");
    if (!sec || sec->meta.info.num("missing_materials", 0) != 2) return fail("asset db missing_materials");
  }

  // ---- Ship It rights warning -------------------------------------------------
  {
    auto rights = [&](exporter::ValidationReport* r) {
      *r = exporter::validate_project(claim.string(), "dev");
      for (const std::string& w : r->warnings) {
        if (w.find("Rights check") != std::string::npos) return w;
      }
      return std::string();
    };
    exporter::ValidationReport rep;
    const std::string w = rights(&rep);
    if (w.empty() || w.find("crate.mdl") == std::string::npos || w.find("gma:TS Smoke Addon!") == std::string::npos ||
        w.find("second.mdl (source-engine)") == std::string::npos) {
      return fail("rights warning: " + w);
    }
    const auto shipped = prov::shipped_third_party(claim.string(), exporter::content_files(claim.string()));
    if (shipped.empty()) return fail("shipped list");
    for (const auto& s : shipped) prov::acknowledge(claim.string(), s.path, true);
    if (!rights(&rep).empty()) return fail("per-asset acknowledgement");
    prov::acknowledge(claim.string(), shipped[0].path, false);
    if (rights(&rep).find(shipped[0].path) == std::string::npos) return fail("revoked acknowledgement");
    prov::acknowledge(claim.string(), "", true);
    if (!rights(&rep).empty()) return fail("claim-wide acknowledgement");
    bool noted = false;
    for (const std::string& n : rep.notes) noted |= n.find("rights acknowledged") != std::string::npos;
    if (!noted) return fail("acknowledged note");
    // The ledger itself is editor-only (never shipped).
    for (const std::string& f : exporter::content_files(claim.string())) {
      if (f.find("provenance") != std::string::npos) return fail("ledger would ship");
    }
  }
  std::cout << "[smoke] gmod: .gma (CRC, LZMA, extract, provenance), VTF/VMT, search order, rights OK\n";
  return 0;
}

}  // namespace tombstone
}  // namespace ts
