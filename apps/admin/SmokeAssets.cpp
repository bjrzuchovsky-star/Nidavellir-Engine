// Asset pipeline smoke: drives AssetDb / AssetBrowser and the editors the
// way the Asset Browser panel does, without a window.
#include "SmokeAssets.h"

#include "audio/Audio.h"
#include "editor/assets/AssetBrowser.h"
#include "editor/assets/AssetImport.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "scene/SceneJson.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace adb = ts::tombstone::editor::assetdb;
namespace sj = ts::tombstone::scene_json;
using ts::tombstone::Entity2D;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] assets FAILED: " << what << '\n';
  return 1;
}

ed::ProjectInfo fresh_project(const std::string& id, ed::ProjectKind kind) {
  const fs::path dir = fs::absolute(fs::path("TombStoneProjects") / id);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "assets", ec);
  ed::ProjectInfo info;
  info.id = id;
  info.name = id;
  info.kind = kind;
  info.path = dir.string();
  return info;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void spit(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

// ---- tiny writers ---------------------------------------------------------------

std::uint32_t crc32(const unsigned char* d, std::size_t n, std::uint32_t c = 0) {
  c = ~c;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void be32(std::string& s, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (i * 8)) & 0xFF);
}

// Uncompressed (stored-deflate) RGBA PNG.
std::string png(int w, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  std::string raw;
  for (int y = 0; y < h; ++y) {
    raw += '\0';
    for (int x = 0; x < w; ++x) {
      raw += static_cast<char>((x + y) % 2 ? r : r / 2);
      raw += static_cast<char>(g);
      raw += static_cast<char>(b);
      raw += static_cast<char>(a);
    }
  }
  std::string z = "\x78\x01";
  std::uint32_t s1 = 1, s2 = 0;
  for (unsigned char c : raw) {
    s1 = (s1 + c) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  for (std::size_t off = 0; off < raw.size() || off == 0; off += 65535) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
    const bool last = off + n >= raw.size();
    z += static_cast<char>(last ? 1 : 0);
    z += static_cast<char>(n & 0xFF);
    z += static_cast<char>(n >> 8);
    z += static_cast<char>(~n & 0xFF);
    z += static_cast<char>((~n >> 8) & 0xFF);
    z += raw.substr(off, n);
    if (last) break;
  }
  be32(z, (s2 << 16) | s1);
  auto chunk = [](std::string& out, const char* type, const std::string& data) {
    be32(out, static_cast<std::uint32_t>(data.size()));
    std::string td = std::string(type, 4) + data;
    out += td;
    be32(out, crc32(reinterpret_cast<const unsigned char*>(td.data()), td.size()));
  };
  std::string out = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  be32(ihdr, static_cast<std::uint32_t>(w));
  be32(ihdr, static_cast<std::uint32_t>(h));
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", "");
  return out;
}

const char* kTriangleGltf =
    R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],)"
    R"("bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";

// Minimal sfnt: maxp + name ("Dustbowl").
std::string font() {
  auto be16 = [](std::string& s, int v) {
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
  };
  std::string maxp;
  be32(maxp, 0x00005000u);
  be16(maxp, 42);
  std::string fam;
  for (char c : std::string("Dustbowl")) {
    fam += '\0';
    fam += c;
  }
  std::string name;
  be16(name, 0);
  be16(name, 1);
  be16(name, 6 + 12);
  be16(name, 3);
  be16(name, 1);
  be16(name, 0x409);
  be16(name, 1);
  be16(name, static_cast<int>(fam.size()));
  be16(name, 0);
  name += fam;
  std::string f;
  be32(f, 0x00010000u);
  be16(f, 2);
  be16(f, 32);
  be16(f, 1);
  be16(f, 0);
  const std::uint32_t off_maxp = 12 + 32, off_name = off_maxp + 8;
  be32(f, 0x6D617870u);  // maxp
  be32(f, 0);
  be32(f, off_maxp);
  be32(f, static_cast<std::uint32_t>(maxp.size()));
  be32(f, 0x6E616D65u);  // name
  be32(f, 0);
  be32(f, off_name);
  be32(f, static_cast<std::uint32_t>(name.size()));
  f += maxp;
  f += std::string(8 - maxp.size(), '\0');
  f += name;
  return f;
}

void write_wav(const fs::path& p, float seconds) {
  ts::audio::Clip c;
  c.channels = 1;
  c.rate = 22050;
  const int n = static_cast<int>(seconds * c.rate);
  for (int i = 0; i < n; ++i) c.samples.push_back(0.3f * std::sin(i * 0.05f));
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  ts::audio::write_wav(p.string(), c);
}

void bump_mtime(const fs::path& p) {
  std::error_code ec;
  fs::last_write_time(p, fs::last_write_time(p, ec) + std::chrono::seconds(3), ec);
}

// ---- database level ---------------------------------------------------------------

int check_db() {
  const ed::ProjectInfo info = fresh_project("_assets_db_smoke", ed::ProjectKind::TwoD);
  const fs::path dir = info.path;
  spit(dir / "assets/hero.png", png(16, 8, 200, 120, 40, 255));
  spit(dir / "assets/models/tri.gltf", kTriangleGltf);
  write_wav(dir / "assets/sfx/bell.wav", 0.5f);
  spit(dir / "assets/fonts/dustbowl.ttf", font());
  spit(dir / "assets/broken.png", "not a png");
  // A scene and a Lua script that use the hero sprite / the bell.
  sj::SceneDoc doc;
  Entity2D hero;
  hero.id = 1;
  hero.name = "Hero";
  hero.sprite = ts::tombstone::SpriteData{};
  hero.sprite->path = "assets/hero.png";
  doc.entities.push_back(hero);
  sj::save_file((dir / "scene.json").string(), doc);
  spit(dir / "scripts/bell.lua", "function on_use() audio.play('assets/sfx/bell.wav') end\n");

  adb::AssetDb db;
  db.set_importer(adb::import_asset);
  db.open(info.path);
  adb::AssetDb::ScanReport rep = db.scan();
  if (rep.created.size() != 5 || db.entries().size() != 5) return fail("first scan creates 5 metas");
  for (const auto& e : db.entries()) {
    if (!fs::exists(dir / (e.rel + ".meta")) || e.meta.guid.size() != 32) return fail("meta for " + e.rel);
  }
  const auto* png_e = db.by_path("assets/hero.png");
  const auto* gl = db.by_path("assets/models/tri.gltf");
  const auto* wav = db.by_path("assets/sfx/bell.wav");
  const auto* ttf = db.by_path("assets/fonts/dustbowl.ttf");
  if (!png_e || png_e->meta.kind != adb::Kind::Texture || png_e->meta.info.num("width", 0) != 16 ||
      png_e->meta.info.num("height", 0) != 8) {
    return fail("png import info");
  }
  if (!gl || !gl->import_ok || gl->meta.info.num("triangles", 0) != 1) return fail("gltf import");
  if (!wav || !wav->import_ok || std::fabs(wav->meta.info.num("seconds", 0) - 0.5) > 0.02) {
    return fail("wav import");
  }
  if (!ttf || !ttf->import_ok || ttf->meta.info.str("family") != "Dustbowl" ||
      ttf->meta.info.num("glyphs", 0) != 42) {
    return fail("font import: " + (ttf ? ttf->import_error : std::string("missing")));
  }
  const auto* broken = db.by_path("assets/broken.png");
  if (!broken || broken->import_ok) return fail("broken png reports an import error");
  const std::string hero_guid = png_e->meta.guid;
  const std::string bell_guid = wav->meta.guid;

  // GUIDs survive a fresh database (they live in the .meta files).
  {
    adb::AssetDb db2;
    db2.open(info.path);
    const auto r2 = db2.scan();
    if (!r2.created.empty() || !db2.by_guid(hero_guid) || db2.path_of(hero_guid) != "assets/hero.png") {
      return fail("guid stable across scans");
    }
  }
  // Import settings persist.
  ts::tombstone::json::Value s = png_e->meta.settings;
  s.set("filter", ts::tombstone::json::Value::string("linear"));
  if (!db.set_settings("assets/hero.png", s)) return fail("set_settings");
  if (slurp(dir / "assets/hero.png.meta").find("\"linear\"") == std::string::npos) {
    return fail("settings written to .meta");
  }
  // References.
  auto refs = db.find_references("assets/hero.png");
  if (refs.size() != 1 || refs[0].file != "scene.json") return fail("find references (scene)");
  refs = db.find_references("assets/sfx/bell.wav");
  if (refs.size() != 1 || refs[0].file != "scripts/bell.lua") return fail("find references (lua)");

  // 1) Rename in the editor: file + meta move, scene follows, GUID kept.
  std::size_t n = 0;
  if (!db.move("assets/hero.png", "assets/chars/hero_idle.png", nullptr, &n) || n != 1) {
    return fail("move in editor");
  }
  if (slurp(dir / "scene.json").find("assets/chars/hero_idle.png") == std::string::npos ||
      !fs::exists(dir / "assets/chars/hero_idle.png.meta") || db.path_of(hero_guid) != "assets/chars/hero_idle.png") {
    return fail("editor move kept references + guid");
  }
  // 2) Moved outside the editor, with its .meta (Explorer, git mv).
  fs::create_directories(dir / "assets/audio");
  fs::rename(dir / "assets/sfx/bell.wav", dir / "assets/audio/bell.wav");
  fs::rename(dir / "assets/sfx/bell.wav.meta", dir / "assets/audio/bell.wav.meta");
  rep = db.scan();
  if (rep.moved.size() != 1 || rep.moved[0].second != "assets/audio/bell.wav") return fail("outside move seen");
  if (db.fix_references(rep) != 1 ||
      slurp(dir / "scripts/bell.lua").find("'assets/audio/bell.wav'") == std::string::npos ||
      db.path_of(bell_guid) != "assets/audio/bell.wav") {
    return fail("outside move fixed references");
  }
  // 3) Renamed outside without its .meta: matched by content hash.
  fs::rename(dir / "assets/chars/hero_idle.png", dir / "assets/chars/hero.png");
  rep = db.scan();
  if (rep.moved.size() != 1 || db.path_of(hero_guid) != "assets/chars/hero.png" ||
      fs::exists(dir / "assets/chars/hero_idle.png.meta") || !rep.created.empty()) {
    return fail("rename without meta adopts the guid");
  }
  db.fix_references(rep);
  if (slurp(dir / "scene.json").find("\"assets/chars/hero.png\"") == std::string::npos) {
    return fail("rename without meta fixed the scene");
  }
  // 4) A copy (file + meta) gets its own GUID.
  fs::copy_file(dir / "assets/chars/hero.png", dir / "assets/chars/hero_copy.png");
  fs::copy_file(dir / "assets/chars/hero.png.meta", dir / "assets/chars/hero_copy.png.meta");
  rep = db.scan();
  const auto* copy = db.by_path("assets/chars/hero_copy.png");
  if (rep.duplicated.size() != 1 || !copy || copy->meta.guid == hero_guid ||
      db.path_of(hero_guid) != "assets/chars/hero.png") {
    return fail("copy gets a new guid");
  }
  // 5) Hot reload: content change is reimported.
  spit(dir / "assets/chars/hero.png", png(32, 32, 10, 200, 90, 128));
  bump_mtime(dir / "assets/chars/hero.png");
  rep = db.scan();
  if (rep.changed.size() != 1 || db.by_path("assets/chars/hero.png")->meta.info.num("width", 0) != 32 ||
      db.path_of(hero_guid) != "assets/chars/hero.png") {
    return fail("changed file reimported (hot reload)");
  }
  // 6) Delete: refused while referenced, allowed when forced / unused.
  std::vector<adb::Ref> in_use;
  if (db.remove("assets/chars/hero.png", false, &in_use) || in_use.empty() ||
      !fs::exists(dir / "assets/chars/hero.png")) {
    return fail("delete refuses an asset in use");
  }
  if (!db.remove("assets/chars/hero_copy.png", false) || fs::exists(dir / "assets/chars/hero_copy.png.meta")) {
    return fail("delete unused asset + meta");
  }
  if (!db.remove("assets/broken.png", true)) return fail("forced delete");
  // Folders.
  if (!db.make_folder("assets/props/barrels")) return fail("make folder");
  const auto folders = db.folders();
  if (std::find(folders.begin(), folders.end(), "assets/props/barrels") == folders.end()) {
    return fail("folders list");
  }
  return 0;
}

// ---- editors -----------------------------------------------------------------------

int check_2d() {
  const ed::ProjectInfo info = fresh_project("_assets2d_smoke", ed::ProjectKind::TwoD);
  const fs::path dir = info.path;
  spit(dir / "assets/crate.png", png(8, 8, 120, 80, 30, 255));
  auto scr = std::make_unique<ed::Editor2DScreen>(info);
  scr->on_enter();
  ed::AssetBrowser& ab = scr->asset_browser();
  if (!ab.db().by_path("assets/crate.png")) return fail("2D browser scanned assets/");
  const std::uint64_t id = scr->create_entity("Crate");
  scr->workspace().find(id)->sprite = ts::tombstone::SpriteData{};
  scr->workspace().find(id)->sprite->path = "assets/crate.png";
  if (!ab.move("assets/crate.png", "assets/props/crate.png")) return fail("2D browser move");
  const Entity2D* e = scr->workspace().find(id);
  if (!e || !e->sprite || e->sprite->path != "assets/props/crate.png") {
    return fail("2D open scene follows the move");
  }
  // Outside move picked up by the poll path.
  fs::create_directories(dir / "assets/props/wood");
  fs::rename(dir / "assets/props/crate.png", dir / "assets/props/wood/crate.png");
  fs::rename(dir / "assets/props/crate.png.meta", dir / "assets/props/wood/crate.png.meta");
  ab.rescan();
  e = scr->workspace().find(id);
  if (!e || !e->sprite || e->sprite->path != "assets/props/wood/crate.png") {
    return fail("2D open scene follows an outside move");
  }
  std::vector<adb::Ref> refs;
  if (ab.remove("assets/props/wood/crate.png", false, &refs) || refs.empty()) {
    return fail("2D delete warns for an asset in use");
  }
  scr->on_exit();
  return 0;
}

int check_3d() {
  const ed::ProjectInfo info = fresh_project("_assets3d_smoke", ed::ProjectKind::ThreeD);
  const fs::path dir = info.path;
  spit(dir / "assets/models/tri.gltf", kTriangleGltf);
  write_wav(dir / "assets/sfx/creak.wav", 0.25f);
  auto e = std::make_unique<ed::Editor3DScreen>(info);
  e->on_enter();
  ed::AssetBrowser& ab = e->asset_browser();
  auto st = ab.db().by_path("assets/models/tri.gltf")->meta.settings;
  st.set("scale", ts::tombstone::json::Value::number(2.5));
  st.set("generate_collider", ts::tombstone::json::Value::boolean(false));
  ab.db().set_settings("assets/models/tri.gltf", st);
  const std::uint64_t m = e->drop_asset("assets/models/tri.gltf", {1, 0, 1});
  if (!m || std::fabs(e->find(m)->transform.scale.x - 2.5f) > 1e-4f || e->find(m)->collider) {
    return fail("3D model drop uses import settings");
  }
  auto as = ab.db().by_path("assets/sfx/creak.wav")->meta.settings;
  as.set("bus", ts::tombstone::json::Value::string("ambience"));
  as.set("loop", ts::tombstone::json::Value::boolean(true));
  ab.db().set_settings("assets/sfx/creak.wav", as);
  if (e->drop_asset("assets/sfx/creak.wav", {}, m) != m || !e->find(m)->audio ||
      e->find(m)->audio->clip != "assets/sfx/creak.wav" || e->find(m)->audio->bus != "ambience" ||
      !e->find(m)->audio->loop) {
    return fail("3D sound drop onto an entity");
  }
  if (!ab.move("assets/models/tri.gltf", "assets/models/props/triangle.gltf")) return fail("3D move");
  const auto* moved = e->find(m);
  if (!moved || !moved->mesh || moved->mesh->source != "assets/models/props/triangle.gltf") {
    return fail("3D scene follows the move");
  }
  const auto refs = ab.db().find_references("assets/sfx/creak.wav");
  if (refs.size() != 1 || refs[0].file != "scene.json") return fail("3D find references");
  e->on_exit();
  return 0;
}

}  // namespace

int run_assets_smoke() {
  if (check_db() != 0) return 1;
  if (check_2d() != 0) return 1;
  if (check_3d() != 0) return 1;
  std::cout << "[smoke] assets OK (.meta + stable GUIDs, PNG / glTF / WAV / font import info, "
               "import errors, settings persisted, find references (JSON + Lua), moves in the "
               "editor and outside it with / without .meta, copies re-GUIDed, hot reload "
               "reimport, delete-in-use refusal, folders; 2D + 3D open scenes follow moves, "
               "3D drops use model scale / collider and sound bus / loop)\n";
  return 0;
}
