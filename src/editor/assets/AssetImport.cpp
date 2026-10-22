#include "editor/assets/AssetImport.h"

#include "audio/Audio.h"
#include "gfx/TextureCache.h"
#include "render3d/Gltf.h"
#include "render3d/ModelImport.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace assetdb {

namespace fs = std::filesystem;
using json::Value;

namespace {

std::uint32_t be32(const unsigned char* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint16_t be16(const unsigned char* p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

}  // namespace

bool read_font_info(const std::string& abs_path, std::string* family, int* glyphs,
                    std::string* error) {
  std::ifstream in(fs::u8path(abs_path), std::ios::binary);
  std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (d.size() < 12) return fail("not a font (too short)");
  std::size_t base = 0;
  if (d[0] == 't' && d[1] == 't' && d[2] == 'c' && d[3] == 'f') {
    if (d.size() < 16) return fail("bad .ttc header");
    base = be32(&d[12]);  // first face
    if (base + 12 > d.size()) return fail("bad .ttc offset");
  }
  const std::uint32_t tag = be32(&d[base]);
  if (tag != 0x00010000u && tag != 0x4F54544Fu /*OTTO*/ && tag != 0x74727565u /*true*/) {
    return fail("not a TrueType / OpenType font");
  }
  const std::uint16_t n = be16(&d[base + 4]);
  if (base + 12 + std::size_t(n) * 16 > d.size()) return fail("truncated table directory");
  std::uint32_t name_off = 0, maxp_off = 0, name_len = 0;
  for (std::uint16_t i = 0; i < n; ++i) {
    const unsigned char* r = &d[base + 12 + std::size_t(i) * 16];
    const std::uint32_t t = be32(r), off = be32(r + 8), len = be32(r + 12);
    if (std::size_t(off) + len > d.size()) return fail("table outside the file");
    if (t == 0x6E616D65u) { name_off = off; name_len = len; }  // name
    if (t == 0x6D617870u) maxp_off = off;                      // maxp
  }
  if (glyphs) *glyphs = maxp_off && maxp_off + 6 <= d.size() ? be16(&d[maxp_off + 4]) : 0;
  std::string fam;
  if (name_off && name_len >= 6) {
    const std::uint16_t count = be16(&d[name_off + 2]), str_off = be16(&d[name_off + 4]);
    for (std::uint16_t i = 0; i < count && fam.empty(); ++i) {
      const std::size_t rec = name_off + 6 + std::size_t(i) * 12;
      if (rec + 12 > d.size()) break;
      const std::uint16_t platform = be16(&d[rec]), name_id = be16(&d[rec + 6]);
      const std::uint16_t len = be16(&d[rec + 8]), off = be16(&d[rec + 10]);
      const std::size_t s = name_off + str_off + off;
      if (name_id != 1 || s + len > d.size()) continue;
      if (platform == 3 || platform == 0) {  // UTF-16BE: keep ASCII
        for (std::size_t k = 0; k + 1 < len; k += 2) {
          if (d[s + k] == 0 && d[s + k + 1] >= 32 && d[s + k + 1] < 127) fam += char(d[s + k + 1]);
        }
      } else {
        fam.assign(reinterpret_cast<const char*>(&d[s]), len);
      }
    }
  }
  if (family) *family = fam.empty() ? fs::u8path(abs_path).stem().string() : fam;
  return true;
}

bool import_asset(const std::string& abs_path, Kind kind, const Value& settings, Value* info,
                  std::string* error) {
  Value o = Value::object();
  switch (kind) {
    case Kind::Texture: {
      std::vector<unsigned char> rgba;
      int w = 0, h = 0;
      if (!gfx::TextureCache::decode_file(abs_path, &rgba, &w, &h, error)) return false;
      bool alpha = false;
      for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] != 255) {
          alpha = true;
          break;
        }
      }
      o.set("width", Value::number(w));
      o.set("height", Value::number(h));
      o.set("alpha", Value::boolean(alpha));
      const double max_size = settings.num("max_size", 4096);
      if (w > max_size || h > max_size) o.set("warning", Value::string("larger than max_size"));
      break;
    }
    case Kind::Model: {
      // Every model format through the one importer (render3d/ModelImport.h)
      // with this asset's settings.
      render3d::Model m;
      std::string warn;
      const render3d::ModelImportOptions opt = render3d::model_options_from_settings(settings, abs_path);
      if (!render3d::load_model(abs_path, opt, &m, &warn)) {
        if (error) *error = warn;
        return false;
      }
      std::size_t verts = 0;
      for (const render3d::ModelPrimitive& p : m.primitives) verts += p.mesh.vertices.size();
      o.set("format", Value::string(render3d::model_format_name(abs_path)));
      o.set("vertices", Value::number(static_cast<double>(verts)));
      o.set("bones", Value::number(static_cast<double>(m.skeleton.joints.size())));
      o.set("primitives", Value::number(static_cast<double>(m.primitives.size())));
      o.set("triangles", Value::number(static_cast<double>(m.triangles())));
      o.set("materials", Value::number(static_cast<double>(m.materials.size())));
      o.set("textures", Value::number(static_cast<double>(m.textures)));
      o.set("skinned", Value::boolean(m.skinned()));
      o.set("clips", Value::number(static_cast<double>(m.clips.size())));
      const float sz[3] = {m.bounds.max.x - m.bounds.min.x, m.bounds.max.y - m.bounds.min.y,
                           m.bounds.max.z - m.bounds.min.z};
      o.set("size", json::floats(sz, 3));
      if (m.lods > 1) o.set("lods", Value::number(m.lods));
      if (m.skin_families > 1) o.set("skins", Value::number(m.skin_families));
      if (!m.body_groups.empty()) {
        Value groups = Value::array();
        for (const render3d::ModelBodyGroup& g : m.body_groups) {
          Value gv = Value::object();
          gv.set("name", Value::string(g.name));
          Value opts = Value::array();
          for (const std::string& n : g.options) opts.push(Value::string(n));
          gv.set("options", opts);
          groups.push(gv);
        }
        o.set("body_groups", groups);
      }
      if (!m.collision_hulls.empty()) o.set("hulls", Value::number(static_cast<double>(m.collision_hulls.size())));
      if (!m.hitboxes.empty()) {
        o.set("hitboxes", Value::number(static_cast<double>(m.hitboxes.size())));
        o.set("hitbox_set", Value::string(m.hitbox_set));
      }
      // Bumped when what the importer reports changes (2: bounds of what
      // is drawn - rest pose, import scale, no collision / LOD meshes);
      // older .meta info is reimported on the next scan.
      o.set("importer_version", Value::number(assetdb::kModelImporterVersion));
      if (!m.clips.empty()) {
        Value names = Value::array();
        for (const render3d::Clip& c : m.clips) names.push(Value::string(c.name));
        o.set("clip_names", names);
      }
      if (!warn.empty()) o.set("warning", Value::string(warn));
      break;
    }
    case Kind::Audio: {
      ::ts::audio::Clip c;
      if (!::ts::audio::decode_file(abs_path, 48000, &c, error)) return false;
      o.set("channels", Value::number(c.channels));
      o.set("seconds", Value::number(static_cast<float>(c.seconds())));
      break;
    }
    case Kind::Font: {
      std::string fam;
      int glyphs = 0;
      if (!read_font_info(abs_path, &fam, &glyphs, error)) return false;
      o.set("family", Value::string(fam));
      o.set("glyphs", Value::number(glyphs));
      break;
    }
    default:
      if (error) *error = "no importer for this file type";
      return false;
  }
  *info = std::move(o);
  return true;
}

std::string info_summary(const Value& info, Kind kind) {
  if (info.has("error")) return "import error: " + info.str("error");
  char buf[160];
  switch (kind) {
    case Kind::Texture:
      std::snprintf(buf, sizeof(buf), "%dx%d%s", int(info.num("width", 0)), int(info.num("height", 0)),
                    info.flag("alpha", false) ? ", alpha" : "");
      return buf;
    case Kind::Model:
      std::snprintf(buf, sizeof(buf), "%s%s%d prim, %d tris%s%s%s", info.str("format").c_str(),
                    info.str("format").empty() ? "" : ": ", int(info.num("primitives", 0)),
                    int(info.num("triangles", 0)), info.flag("skinned", false) ? ", skinned" : "",
                    info.num("clips", 0) > 0 ? ", animated" : "",
                    info.num("hulls", 0) > 0 ? ", hulls" : "");
      return buf;
    case Kind::Audio:
      std::snprintf(buf, sizeof(buf), "%.2f s %s", info.num("seconds", 0),
                    info.num("channels", 1) > 1 ? "stereo" : "mono");
      return buf;
    case Kind::Font:
      std::snprintf(buf, sizeof(buf), "%s, %d glyphs", info.str("family").c_str(),
                    int(info.num("glyphs", 0)));
      return buf;
    default: return {};
  }
}

}  // namespace assetdb
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
