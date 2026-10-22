// ts_admin --smoke-meta (also part of --smoke): the claim's metadata folder
// (.tombstone/meta) and sorted imports. New imports write their .meta in the
// folder and land in type folders (model packs, keep-pack off with rewritten
// texture paths, explicit folder, sorting off); a sidecar-layout claim is
// migrated (GUIDs + scene references kept, backup + log, idempotent);
// editor rename / move / delete carry the .meta; outside moves are relinked
// by content hash and by file name; orphans are reported, parked and
// recovered; Vault push / pull round trips with the new layout and with
// items uploaded from sidecar claims; the runtime model importer and the
// exporter find the .meta.
#include "SmokeMeta.h"

#include "core/AssetMeta.h"
#include "core/PngWrite.h"
#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "editor/assets/ImportSort.h"
#include "editor/vault/VaultProject.h"
#include "export/Exporter.h"
#include "render3d/ModelImport.h"
#include "scene/JsonTree.h"
#include "vault/Sha256.h"
#include "vault/VaultBackend.h"
#include "vault/VaultLock.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace adb = ts::tombstone::editor::assetdb;
namespace is = ts::tombstone::editor::importsort;
namespace vp = ts::tombstone::editor::vaultp;
namespace tv = ts::tombstone::vault;
namespace am = ts::tombstone::core::asset_meta;
using ts::tombstone::json::Value;

namespace {

int fail(const std::string& why) {
  std::cerr << "[smoke] meta FAILED: " << why << '\n';
  return 1;
}

void spit(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void png(const fs::path& p, int w, int h, unsigned char r, unsigned char g, unsigned char b) {
  std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
  for (int i = 0; i < w * h; ++i) {
    px[i * 4 + 0] = static_cast<unsigned char>(r ^ (i * 7));
    px[i * 4 + 1] = g;
    px[i * 4 + 2] = b;
    px[i * 4 + 3] = 255;
  }
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  ts::tombstone::write_png_rgba(p.string(), w, h, px);
}

void wav(const fs::path& p) {
  const int n = 2205;
  std::string d = "RIFF";
  auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) d += static_cast<char>((v >> (8 * i)) & 0xFF); };
  auto u16 = [&](std::uint16_t v) { d += static_cast<char>(v & 0xFF); d += static_cast<char>(v >> 8); };
  u32(36 + n);
  d += "WAVEfmt ";
  u32(16); u16(1); u16(1); u32(11025); u32(11025); u16(1); u16(8);
  d += "data";
  u32(n);
  for (int i = 0; i < n; ++i) d += static_cast<char>(128 + static_cast<int>(30 * std::sin(i * 0.3)));
  spit(p, d);
}

// One-triangle glTF, external .bin, one image uri.
void gltf(const fs::path& file, const std::string& image_uri) {
  std::string bin(36, '\0');
  const float v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::memcpy(bin.data(), v, sizeof(v));
  fs::path binp = file;
  binp.replace_extension(".bin");
  spit(binp, bin);
  spit(file, R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":")" + binp.filename().string() +
                 R"("}],"bufferViews":[{"buffer":0,"byteLength":36}],)"
                 R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
                 R"("images":[{"uri":")" + image_uri + R"("}],"textures":[{"source":0}],)"
                 R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
                 R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
}

void obj(const fs::path& file, const std::string& tex) {
  fs::path mtl = file;
  mtl.replace_extension(".mtl");
  spit(file, "mtllib " + mtl.filename().string() + "\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl rock\nf 1/1 2/2 3/3\n");
  spit(mtl, "newmtl rock\nKd 1 1 1\nmap_Kd " + tex + "\n");
}

fs::path fresh(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p / "assets", ec);
  spit(p / "project.json", R"({"name": ")" + p.filename().string() + R"(", "dimension": "3d"})");
  return p;
}

fs::path meta_of(const fs::path& claim, const std::string& rel) { return fs::u8path(am::meta_path(claim.string(), rel)); }

std::string guid_in(const fs::path& claim, const std::string& rel) {
  adb::Meta m;
  return adb::read_meta(meta_of(claim, rel).string(), &m) ? m.guid : std::string();
}

// Any .meta outside .tombstone? (the Asset Browser / Explorer view)
std::string stray_meta(const fs::path& claim) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(claim, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) && it->path().filename() == ".tombstone") {
      it.disable_recursion_pending();
      continue;
    }
    if (it->path().extension() == ".meta") return fs::relative(it->path(), claim, ec).generic_string();
  }
  return {};
}

bool open_db(adb::AssetDb* db, const fs::path& claim) {
  db->set_importer(adb::import_asset);
  db->open(claim.string());
  db->scan();
  return true;
}

// ---- sorted imports ----------------------------------------------------------

int check_sorted_imports(const fs::path& root) {
  const fs::path in = root / "incoming";
  std::error_code ec;
  fs::remove_all(in, ec);
  png(in / "hero.png", 8, 8, 200, 100, 40);
  wav(in / "bell.wav");
  spit(in / "dustbowl.ttf", "not really a font");
  spit(in / "crate.prefab.json", R"({"prefab": 1, "name": "Crate", "root": {"name": "Crate"}})");
  spit(in / "ai.lua", "-- brains\n");
  spit(in / "walk.anim.json", R"({"clips": []})");
  spit(in / "notes.txt", "hello");
  gltf(in / "barrel" / "barrel.gltf", "textures/barrel_d.png");
  png(in / "barrel" / "textures" / "barrel_d.png", 4, 4, 90, 60, 30);
  obj(in / "rock" / "rock.obj", "rock.png");
  png(in / "rock" / "rock.png", 4, 4, 120, 120, 120);

  const fs::path a = fresh(root / "sorted_a");
  is::Result r;
  const std::vector<std::string> src = {(in / "hero.png").string(),       (in / "bell.wav").string(),
                                        (in / "dustbowl.ttf").string(),   (in / "crate.prefab.json").string(),
                                        (in / "ai.lua").string(),         (in / "walk.anim.json").string(),
                                        (in / "notes.txt").string(),      (in / "barrel" / "barrel.gltf").string(),
                                        (in / "rock" / "rock.obj").string()};
  if (!is::import_files(a.string(), src, "", is::load_settings(a.string()), &r) || !r.errors.empty()) {
    return fail("sorted import: " + (r.errors.empty() ? std::string("nothing") : r.errors[0]));
  }
  for (const char* want : {"assets/Textures/hero.png", "assets/Audio/bell.wav", "assets/Fonts/dustbowl.ttf",
                           "prefabs/crate.prefab.json", "scripts/ai.lua", "assets/Animations/walk.anim.json",
                           "assets/Other/notes.txt", "assets/Models/barrel/barrel.gltf", "assets/Models/barrel/barrel.bin",
                           "assets/Models/barrel/textures/barrel_d.png", "assets/Models/rock/rock.obj",
                           "assets/Models/rock/rock.mtl", "assets/Models/rock/rock.png"}) {
    if (!fs::is_regular_file(a / want)) return fail(std::string("sorted import destination ") + want);
  }
  if (fs::exists(a / "assets/Textures/barrel_d.png") || fs::exists(a / "assets/Textures/rock.png")) {
    return fail("a model's textures travel with it (keep model packs)");
  }
  // The scan gives each asset a .meta in the metadata folder, none beside it.
  adb::AssetDb db;
  open_db(&db, a);
  for (const char* rel : {"assets/Textures/hero.png", "assets/Audio/bell.wav", "assets/Models/barrel/barrel.gltf",
                          "assets/Models/barrel/textures/barrel_d.png"}) {
    if (guid_in(a, rel).size() != 32) return fail(std::string("new import writes its .meta in .tombstone/meta: ") + rel);
  }
  if (!stray_meta(a).empty()) return fail("a .meta outside the metadata folder: " + stray_meta(a));
  const auto* barrel = db.by_path("assets/Models/barrel/barrel.gltf");
  if (!barrel || !barrel->import_ok) return fail("imported model pack imports");
  if (!fs::is_regular_file(a / am::kLayoutFile)) return fail("layout stamp .tombstone/meta.json");
  // Never overwrites: the second copy gets a new name / folder.
  is::import_files(a.string(), {(in / "hero.png").string(), (in / "barrel" / "barrel.gltf").string()}, "",
                   is::load_settings(a.string()), &r);
  if (!fs::exists(a / "assets/Textures/hero_2.png") || !fs::exists(a / "assets/Models/barrel_2/barrel.gltf") ||
      !fs::exists(a / "assets/Models/barrel_2/textures/barrel_d.png")) {
    return fail("re-import goes next to the first one (hero_2.png, Models/barrel_2/)");
  }
  // Dropped on a folder: exactly there; a dropped folder keeps its layout.
  is::import_files(a.string(), {(in / "hero.png").string(), (in / "rock").string()}, "assets/ui", is::load_settings(a.string()), &r);
  if (!fs::exists(a / "assets/ui/hero.png") || !fs::exists(a / "assets/ui/rock/rock.obj") ||
      !fs::exists(a / "assets/ui/rock/rock.png") || !fs::exists(a / "assets/ui/rock/rock.mtl")) {
    return fail("drop on a folder overrides sorting");
  }
  // Sorting off: assets/.
  is::Settings off;
  off.sort_imports = false;
  if (!is::save_settings(a.string(), off) || is::load_settings(a.string()).sort_imports) return fail("import settings persist");
  is::import_files(a.string(), {(in / "notes.txt").string()}, "", is::load_settings(a.string()), &r);
  if (!fs::exists(a / "assets/notes.txt")) return fail("sorting off imports into assets/");
  // 2D Supply Wagon image import follows the setting too.
  is::save_settings(a.string(), is::Settings{});

  // Keep model packs off: textures to assets/Textures/<Model>/, paths rewritten.
  const fs::path b = fresh(root / "sorted_b");
  is::Settings split;
  split.keep_model_packs = false;
  is::save_settings(b.string(), split);
  is::import_files(b.string(), {(in / "barrel" / "barrel.gltf").string(), (in / "rock" / "rock.obj").string()}, "",
                   is::load_settings(b.string()), &r);
  if (!fs::exists(b / "assets/Models/barrel/barrel.gltf") || !fs::exists(b / "assets/Models/barrel/barrel.bin") ||
      !fs::exists(b / "assets/Textures/barrel/barrel_d.png") || !fs::exists(b / "assets/Textures/rock/rock.png") ||
      !fs::exists(b / "assets/Models/rock/rock.mtl")) {
    return fail("keep model packs off: textures in assets/Textures/<Model>/");
  }
  if (slurp(b / "assets/Models/barrel/barrel.gltf").find("\"../../Textures/barrel/barrel_d.png\"") == std::string::npos) {
    return fail("glTF image uri rewritten");
  }
  if (slurp(b / "assets/Models/rock/rock.mtl").find("map_Kd ../../Textures/rock/rock.png") == std::string::npos) {
    return fail("OBJ .mtl map rewritten: " + slurp(b / "assets/Models/rock/rock.mtl"));
  }
  ts::tombstone::render3d::Model m;
  std::string warn;
  if (!ts::tombstone::render3d::load_model((b / "assets/Models/barrel/barrel.gltf").string(), &m, &warn) ||
      m.materials.empty() || !m.materials[0].base_color_image || warn.find("missing") != std::string::npos) {
    return fail("rewritten glTF still finds its texture: " + warn);
  }
  std::cout << "[smoke] meta: sorted imports (Models/<Name>/ packs, Textures / Audio / Fonts / Animations / Other, "
               "prefabs/ + scripts/, re-import renamed, folder drop, sorting off, split packs with rewritten paths)\n";
  return 0;
}

// ---- migration + relinks -------------------------------------------------------

int check_migration(const fs::path& root) {
  const fs::path c = fresh(root / "legacy");
  png(c / "assets/props/crate.png", 8, 8, 10, 20, 30);
  gltf(c / "assets/meshes/tri.gltf", "../props/crate.png");
  wav(c / "assets/sfx/bell.wav");
  auto legacy_meta = [&](const std::string& rel, adb::Kind k, const std::string& guid) {
    adb::Meta m;
    m.guid = guid;
    m.kind = k;
    m.path = rel;
    m.hash = adb::asset_hash((c / rel).string());
    m.settings = adb::default_settings(k, rel);
    adb::write_meta((c / (rel + ".meta")).string(), m);
  };
  const std::string g_crate = "11111111111111111111111111111111", g_tri = "22222222222222222222222222222222",
                    g_bell = "33333333333333333333333333333333", g_gone = "44444444444444444444444444444444";
  legacy_meta("assets/props/crate.png", adb::Kind::Texture, g_crate);
  legacy_meta("assets/meshes/tri.gltf", adb::Kind::Model, g_tri);
  legacy_meta("assets/sfx/bell.wav", adb::Kind::Audio, g_bell);
  {
    // A sidecar whose file was deleted long ago (legacy orphan).
    adb::Meta m;
    m.guid = g_gone;
    m.kind = adb::Kind::Texture;
    m.path = "assets/old/gone.png";
    m.hash = "0123456789abcdef";
    fs::create_directories(c / "assets/old");
    adb::write_meta((c / "assets/old/gone.png.meta").string(), m);
  }
  // The model's unit scale lives in its .meta (the runtime reads it).
  {
    adb::Meta m;
    adb::read_meta((c / "assets/meshes/tri.gltf.meta").string(), &m);
    m.settings.set("unit_scale", Value::number(2));
    adb::write_meta((c / "assets/meshes/tri.gltf.meta").string(), m);
  }
  const std::string scene = R"({"entities": [{"name": "Crate", "sprite": {"path": "assets/props/crate.png"}},)"
                            R"( {"name": "Tri", "mesh": {"model": "assets/meshes/tri.gltf"}}]})";
  spit(c / "scene3d.json", scene);

  adb::AssetDb db;
  db.set_importer(adb::import_asset);
  db.open(c.string());
  const auto& mr = db.last_migration();
  if (mr.moved != 4 || mr.backup_dir.empty()) return fail("migration moves the 4 sidecars (" + std::to_string(mr.moved) + ")");
  if (!stray_meta(c).empty()) return fail("sidecar left after migration: " + stray_meta(c));
  if (guid_in(c, "assets/props/crate.png") != g_crate || guid_in(c, "assets/meshes/tri.gltf") != g_tri ||
      guid_in(c, "assets/sfx/bell.wav") != g_bell) {
    return fail("migration keeps GUIDs");
  }
  for (const char* rel : {"assets/props/crate.png.meta", "assets/meshes/tri.gltf.meta", "assets/sfx/bell.wav.meta"}) {
    if (!fs::exists(c / mr.backup_dir / rel)) return fail(std::string("migration backup of ") + rel);
  }
  if (slurp(c / am::kLogFile).find("migrate: assets/props/crate.png.meta") == std::string::npos) return fail("migration log");
  if (slurp(c / am::kLayoutFile).find("\"meta_layout\": 2") == std::string::npos) return fail("layout stamp: " + slurp(c / am::kLayoutFile));
  auto rep = db.scan();
  if (!rep.created.empty() || !rep.moved.empty()) return fail("scan after migration: no new GUIDs, no moves");
  if (db.path_of(g_crate) != "assets/props/crate.png" || slurp(c / "scene3d.json") != scene) {
    return fail("migration keeps scene references");
  }
  if (rep.orphan_metas.size() != 1 || rep.orphan_metas[0] != "assets/old/gone.png") return fail("legacy orphan reported");
  // Idempotent: opening again moves nothing.
  {
    adb::AssetDb again;
    again.open(c.string());
    if (again.last_migration().any()) return fail("second open migrates again");
  }
  // Runtime importer reads the migrated model settings.
  ts::tombstone::render3d::ModelImportOptions o;
  if (!ts::tombstone::render3d::read_model_options((c / "assets/meshes/tri.gltf").string(), &o) ||
      std::fabs(o.unit_scale - 2.0f) > 1e-4f) {
    return fail("runtime model import reads the .meta from .tombstone/meta");
  }
  // Export content: the model's .meta (logical sidecar name), nothing from .tombstone.
  const auto content = ts::tombstone::exporter::content_files_with_metas(c.string());
  const std::set<std::string> cs(content.begin(), content.end());
  if (!cs.count("assets/meshes/tri.gltf.meta")) return fail("export sees the model's .meta");
  for (const std::string& f : content) {
    if (f.rfind(".tombstone", 0) == 0) return fail("export content includes " + f);
  }

  // Editor rename / move: the .meta moves in the mirror, empty folders go.
  if (!db.move("assets/sfx/bell.wav", "assets/Audio/bell_ring.wav")) return fail("editor move");
  if (guid_in(c, "assets/Audio/bell_ring.wav") != g_bell || fs::exists(meta_of(c, "assets/sfx/bell.wav")) ||
      fs::exists(meta_of(c, "assets/sfx/bell.wav").parent_path())) {
    return fail("editor move carries the .meta (and prunes the empty mirror folder)");
  }
  // Outside move (Explorer): relinked by content hash, references fixed.
  fs::create_directories(c / "assets/Textures");
  fs::rename(c / "assets/props/crate.png", c / "assets/Textures/crate.png");
  rep = db.scan();
  if (rep.relinked.size() != 1 || rep.relinked[0].how != "hash" || db.path_of(g_crate) != "assets/Textures/crate.png" ||
      !rep.created.empty()) {
    return fail("outside move relinked by hash");
  }
  db.fix_references(rep);
  if (slurp(c / "scene3d.json").find("\"assets/Textures/crate.png\"") == std::string::npos) return fail("outside move fixes the scene");
  if (fs::exists(meta_of(c, "assets/props/crate.png"))) return fail("relinked .meta leaves the old place");
  if (slurp(c / am::kLogFile).find("relink: assets/props/crate.png -> assets/Textures/crate.png") == std::string::npos) {
    return fail("relink logged");
  }
  // Moved and edited before the editor saw it: relinked by file name.
  fs::create_directories(c / "assets/Models/tri");
  fs::rename(c / "assets/meshes/tri.gltf", c / "assets/Models/tri/tri.gltf");
  fs::rename(c / "assets/meshes/tri.bin", c / "assets/Models/tri/tri.bin");
  spit(c / "assets/Models/tri/tri.gltf", slurp(c / "assets/Models/tri/tri.gltf") + "\n");
  rep = db.scan();
  if (rep.relinked.size() != 1 || rep.relinked[0].how != "name" || db.path_of(g_tri) != "assets/Models/tri/tri.gltf") {
    return fail("outside move + edit relinked by file name");
  }
  // Delete outside: an orphan; parked after confirmation; recovered when the file returns.
  const std::string bell = slurp(c / "assets/Audio/bell_ring.wav");
  fs::remove(c / "assets/Audio/bell_ring.wav");
  rep = db.scan();
  if (std::find(rep.orphan_metas.begin(), rep.orphan_metas.end(), "assets/Audio/bell_ring.wav") == rep.orphan_metas.end()) {
    return fail("deleted-outside file reported as an orphan");
  }
  if (db.clean_orphans(rep.orphan_metas, true) != 2 ||
      !fs::exists(c / am::kOrphanDir / "assets/Audio/bell_ring.wav.meta") || fs::exists(meta_of(c, "assets/Audio/bell_ring.wav"))) {
    return fail("orphans parked in .tombstone/meta_orphans");
  }
  rep = db.scan();
  if (!rep.orphan_metas.empty()) return fail("parked orphans are not reported again");
  spit(c / "assets/sfx/bell_back.wav", bell);
  rep = db.scan();
  if (db.path_of(g_bell) != "assets/sfx/bell_back.wav" || fs::exists(c / am::kOrphanDir / "assets/Audio/bell_ring.wav.meta")) {
    return fail("a returning file gets its parked GUID back");
  }
  // A sidecar copied in later (file + .meta from an old claim): adopted.
  png(c / "assets/Textures/sign.png", 4, 4, 1, 2, 3);
  adb::Meta sm;
  sm.guid = "55555555555555555555555555555555";
  sm.kind = adb::Kind::Texture;
  sm.path = "assets/Textures/sign.png";
  sm.hash = adb::asset_hash((c / "assets/Textures/sign.png").string());
  adb::write_meta((c / "assets/Textures/sign.png.meta").string(), sm);
  rep = db.scan();
  if (rep.adopted.size() != 1 || guid_in(c, "assets/Textures/sign.png") != sm.guid || !stray_meta(c).empty()) {
    return fail("late sidecar adopted into the metadata folder");
  }
  // Delete in the editor: the .meta goes too.
  if (!db.remove("assets/Textures/sign.png", true) || fs::exists(meta_of(c, "assets/Textures/sign.png"))) {
    return fail("editor delete removes the .meta");
  }
  std::cout << "[smoke] meta: sidecar claim migrated (4 .meta, GUIDs + scene kept, backup + log + layout stamp, idempotent), "
               "runtime + export read it, editor move / delete carry it, outside moves relinked by hash and by name, "
               "orphans reported / parked / recovered, late sidecar adopted\n";
  return 0;
}

// ---- Vault -----------------------------------------------------------------

// Publishes an item the way a sidecar-layout claim did (files incl.
// "<asset>.meta" read from beside the asset) - or with a meta_path override.
bool publish_raw(tv::Backend& lib, const fs::path& claim, const std::string& rel, const std::string& kind,
                 const std::string& guid, const std::string& meta_disk, const std::string& meta_lib_path, std::string* err) {
  tv::Item it;
  it.id = guid;
  it.kind = kind;
  it.name = fs::path(rel).stem().string();
  tv::Version v;
  v.note = "from an old claim";
  auto add = [&](const fs::path& disk, const std::string& path, const std::string& role) -> bool {
    std::uint64_t size = 0;
    const std::string sha = tv::sha256_file(disk.string(), &size);
    if (sha.empty()) return false;
    std::vector<std::string> missing;
    if (!lib.missing({sha}, &missing, err)) return false;
    if (!missing.empty() && !lib.upload(disk.string(), sha, size, err)) return false;
    tv::FileRec f;
    f.path = path;
    f.sha256 = sha;
    f.size = size;
    f.role = role;
    f.guid = guid;
    v.files.push_back(f);
    return true;
  };
  if (!add(claim / rel, rel, "main") || !add(fs::u8path(meta_disk), meta_lib_path, "meta")) return false;
  tv::PublishResult pr;
  return lib.publish(it, v, &pr, err);
}

int check_vault(const fs::path& root) {
  std::error_code ec;
  fs::remove_all(root / "lib", ec);
  tv::LocalBackend lib((root / "lib").string());
  std::string err;
  // New layout: a sorted model pack pushed and pulled into a sorted claim.
  const fs::path in = root / "incoming_v";
  fs::remove_all(in, ec);
  gltf(in / "lamp" / "lamp.gltf", "lamp_d.png");
  png(in / "lamp" / "lamp_d.png", 4, 4, 250, 200, 90);
  const fs::path a = fresh(root / "vault_a");
  is::Result ir;
  is::import_files(a.string(), {(in / "lamp" / "lamp.gltf").string()}, "", is::load_settings(a.string()), &ir);
  adb::AssetDb db;
  open_db(&db, a);
  const std::string g_lamp = guid_in(a, "assets/Models/lamp/lamp.gltf");
  const std::string g_tex = guid_in(a, "assets/Models/lamp/lamp_d.png");
  if (g_lamp.empty() || g_tex.empty()) return fail("vault source scanned");
  vp::PushResult pr;
  vp::PushOptions po;
  po.author = "smoke";
  if (!vp::push_asset(a.string(), &db, "assets/Models/lamp/lamp.gltf", lib, po, &pr, &err)) return fail("push (new layout): " + err);
  tv::Item item;
  lib.get(g_lamp, &item, &err);
  bool meta_logical = false;
  for (const auto& f : item.versions.back().files) {
    if (f.role == "meta") meta_logical = f.path == "assets/Models/lamp/lamp.gltf.meta";
  }
  if (!meta_logical) return fail("pushed .meta is named by its logical sidecar path");
  const fs::path b = fresh(root / "vault_b");
  vp::PullResult r;
  if (!vp::pull_item(b.string(), nullptr, lib, g_lamp, {}, &r, &err)) return fail("pull (new layout): " + err);
  if (r.main != "assets/Models/lamp/lamp.gltf" || guid_in(b, "assets/Models/lamp/lamp.gltf") != g_lamp ||
      guid_in(b, "assets/Models/lamp/lamp_d.png") != g_tex || !fs::exists(b / "assets/Models/lamp/lamp.bin")) {
    return fail("pull (new layout) lands the pack with its GUIDs in .tombstone/meta (" + r.main + ")");
  }
  if (!stray_meta(b).empty()) return fail("pull wrote a sidecar: " + stray_meta(b));
  {
    adb::AssetDb bdb;
    open_db(&bdb, b);
    const auto rep = bdb.scan();
    if (!rep.created.empty() || !rep.moved.empty() || bdb.path_of(g_lamp) != "assets/Models/lamp/lamp.gltf") {
      return fail("pulled claim scans clean");
    }
  }
  std::vector<vp::ItemStatus> st;
  if (!vp::project_status(b.string(), lib, &st, &err) || st.empty()) return fail("status: " + err);
  for (const auto& s : st) {
    if (s.modified) return fail("pulled item reads as modified");
  }
  // Round trip: B pushes it back unchanged (same item, no new version).
  {
    adb::AssetDb bdb;
    open_db(&bdb, b);
    vp::PushResult pr2;
    if (!vp::push_asset(b.string(), &bdb, "assets/Models/lamp/lamp.gltf", lib, po, &pr2, &err) || pr2.created ||
        pr2.item != g_lamp) {
      return fail("round trip push from the pulled claim is unchanged: " + err);
    }
  }

  // Sorted pull of a pack from a lowercase layout: Models/<Name>/ keeps the relative layout.
  const fs::path s = fresh(root / "vault_src_old_layout");
  is::save_settings(s.string(), is::Settings{false, true});
  png(s / "assets/textures/crate.png", 4, 4, 160, 110, 60);
  gltf(s / "assets/props/crate.gltf", "../textures/crate.png");
  adb::AssetDb sdb;
  open_db(&sdb, s);
  if (!vp::push_asset(s.string(), &sdb, "assets/props/crate.gltf", lib, po, &pr, &err)) return fail("push crate: " + err);
  const std::string g_crate = guid_in(s, "assets/props/crate.gltf");
  const fs::path c = fresh(root / "vault_c");
  if (!vp::pull_item(c.string(), nullptr, lib, g_crate, {}, &r, &err)) return fail("sorted pull: " + err);
  if (r.main != "assets/Models/crate/props/crate.gltf" || !fs::exists(c / "assets/Models/crate/textures/crate.png") ||
      guid_in(c, "assets/Models/crate/props/crate.gltf") != g_crate) {
    return fail("sorted pull of a model pack (" + r.main + ")");
  }
  {
    ts::tombstone::render3d::Model m;
    std::string warn;
    if (!ts::tombstone::render3d::load_model((c / r.main).string(), &m, &warn) || m.materials.empty() ||
        !m.materials[0].base_color_image || warn.find("missing") != std::string::npos) {
      return fail("sorted pull keeps the model's texture path working: " + warn);
    }
  }

  // Legacy: items uploaded from sidecar claims ("<asset>.meta" beside the file).
  const fs::path old = fresh(root / "vault_old_claim");
  png(old / "assets/props/sign.png", 4, 4, 9, 9, 9);
  adb::Meta m;
  m.guid = "66666666666666666666666666666666";
  m.kind = adb::Kind::Texture;
  m.path = "assets/props/sign.png";
  m.hash = adb::asset_hash((old / "assets/props/sign.png").string());
  m.settings = adb::default_settings(adb::Kind::Texture);
  m.settings.set("filter", Value::string("linear"));
  adb::write_meta((old / "assets/props/sign.png.meta").string(), m);
  if (!publish_raw(lib, old, "assets/props/sign.png", "texture", m.guid, (old / "assets/props/sign.png.meta").string(),
                   "assets/props/sign.png.meta", &err)) {
    return fail("legacy publish: " + err);
  }
  const fs::path d = fresh(root / "vault_d");
  if (!vp::pull_item(d.string(), nullptr, lib, m.guid, {}, &r, &err)) return fail("legacy pull: " + err);
  adb::Meta got;
  if (r.main != "assets/Textures/sign.png" || !adb::read_meta(meta_of(d, "assets/Textures/sign.png").string(), &got) ||
      got.guid != m.guid || got.path != "assets/Textures/sign.png" || got.settings.str("filter") != "linear" ||
      !stray_meta(d).empty()) {
    return fail("legacy item pulled: sorted, .meta converted into .tombstone/meta (" + r.main + ")");
  }
  // Legacy pull with sorting off keeps the library path, still no sidecar.
  const fs::path e = fresh(root / "vault_e");
  is::save_settings(e.string(), is::Settings{false, true});
  if (!vp::pull_item(e.string(), nullptr, lib, m.guid, {}, &r, &err) || r.main != "assets/props/sign.png" ||
      guid_in(e, "assets/props/sign.png") != m.guid || !stray_meta(e).empty()) {
    return fail("legacy pull (sorting off) converts the .meta: " + err);
  }
  // An item whose .meta was uploaded under ".tombstone/meta/..." reads the same.
  png(old / "assets/props/flag.png", 4, 4, 5, 50, 5);
  m.guid = "77777777777777777777777777777777";
  m.path = "assets/props/flag.png";
  m.hash = adb::asset_hash((old / "assets/props/flag.png").string());
  adb::write_meta((old / "flag.meta.tmp").string(), m);
  if (!publish_raw(lib, old, "assets/props/flag.png", "texture", m.guid, (old / "flag.meta.tmp").string(),
                   ".tombstone/meta/assets/props/flag.png.meta", &err) ||
      !vp::pull_item(e.string(), nullptr, lib, m.guid, {}, &r, &err) || guid_in(e, "assets/props/flag.png") != m.guid) {
    return fail("item with a .tombstone/meta path pulls: " + err);
  }
  // Exports: the pulled-but-unused sign is left out with its logical .meta.
  const auto content = ts::tombstone::exporter::content_files_with_metas(e.string());
  const auto unused = tv::unused_pulled_files(e.string(), content);
  if (!unused.count("assets/props/sign.png") || !unused.count("assets/props/sign.png.meta")) {
    return fail("export prunes an unused pull and its .meta");
  }
  std::cout << "[smoke] meta: Vault push / pull round trip with .tombstone/meta (logical .meta paths, GUIDs, clean status, "
               "unchanged re-push), sorted pull of a model pack, legacy sidecar items converted on pull (sorted + unsorted)\n";
  return 0;
}

}  // namespace

std::string make_sorted_demo_claim(const std::string& root_dir) {
  const fs::path root = fs::u8path(root_dir);
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path in = root / "downloads";
  png(in / "crate_wood.png", 64, 64, 176, 120, 64);
  png(in / "sign_saloon.png", 64, 32, 120, 40, 30);
  png(in / "sky_dusk.png", 64, 64, 80, 60, 140);
  wav(in / "bell.wav");
  wav(in / "spurs.wav");
  spit(in / "rye.ttf", "font");
  spit(in / "crate_stack.prefab.json", R"({"prefab": 1, "name": "Crate stack", "root": {"name": "Stack"}})");
  spit(in / "door.lua", "-- door\n");
  spit(in / "walk.anim.json", R"({"clips": []})");
  gltf(in / "lantern" / "lantern.gltf", "textures/lantern_d.png");
  png(in / "lantern" / "textures" / "lantern_d.png", 32, 32, 230, 180, 60);
  gltf(in / "barrel" / "barrel.gltf", "barrel_d.png");
  png(in / "barrel" / "barrel_d.png", 32, 32, 140, 90, 50);
  const fs::path c = fresh(root / "Dusty Gulch");
  // An old asset with a sidecar .meta: migrated on open.
  png(c / "assets/Textures/old_poster.png", 32, 32, 200, 180, 150);
  adb::Meta m;
  m.guid = adb::new_guid();
  m.kind = adb::Kind::Texture;
  m.path = "assets/Textures/old_poster.png";
  m.hash = adb::asset_hash((c / m.path).string());
  adb::write_meta((c / (m.path + ".meta")).string(), m);
  is::Result r;
  std::vector<std::string> src;
  for (const char* f : {"crate_wood.png", "sign_saloon.png", "sky_dusk.png", "bell.wav", "spurs.wav", "rye.ttf",
                        "crate_stack.prefab.json", "door.lua", "walk.anim.json", "lantern/lantern.gltf", "barrel/barrel.gltf"}) {
    src.push_back((in / f).string());
  }
  is::import_files(c.string(), src, "", is::Settings{}, &r);
  return c.string();
}

int run_meta_smoke() {
  const fs::path root = fs::absolute("TombStoneProjects") / "_meta_smoke";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (check_sorted_imports(root) != 0) return 1;
  if (check_migration(root) != 0) return 1;
  if (check_vault(root) != 0) return 1;
  fs::remove_all(root, ec);
  std::cout << "[smoke] meta OK\n";
  return 0;
}
