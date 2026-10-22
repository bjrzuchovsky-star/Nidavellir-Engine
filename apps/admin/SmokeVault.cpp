// The Vault smoke (ts_admin --smoke-vault, also part of --smoke): the local
// backend, then the remote one against an in-process ts_vault on loopback.
// Uploads a model with a texture dependency, pulls it into two separate
// temp claims (same GUIDs, same SHA-256), publishes v2 and checks the
// update badge, pins, resumes an interrupted upload, rejects a bad API
// key, and checks that exports leave out pulled items the game never uses.
#include "VaultCli.h"

#include "core/AssetMeta.h"
#include "core/PngWrite.h"
#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "editor/vault/VaultPanel.h"
#include "editor/vault/VaultProject.h"
#include "export/Exporter.h"
#include "vault/Http.h"
#include "vault/Sha256.h"
#include "vault/VaultBackend.h"
#include "vault/VaultLock.h"
#include "vault/VaultServer.h"
#include "vault/VaultStore.h"
#include "scene/JsonTree.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace adb = ts::tombstone::editor::assetdb;
namespace vp = ts::tombstone::editor::vaultp;
namespace tv = ts::tombstone::vault;

namespace ts {
namespace tombstone {

namespace {

int fail(const std::string& why) {
  std::cerr << "[smoke] vault FAILED: " << why << '\n';
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

// Patterned RGBA texture (planks / checks / rings) for thumbnails that read.
void texture(const fs::path& p, int w, int h, unsigned char r, unsigned char g, unsigned char b, int style) {
  std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float k = 1.0f;
      if (style == 0) k = ((y / std::max(1, h / 6)) % 2 ? 0.78f : 1.0f) * (x % std::max(1, w / 3) == 0 ? 0.6f : 1.0f);
      if (style == 1) k = ((x / std::max(1, w / 8) + y / std::max(1, h / 8)) % 2) ? 0.55f : 1.0f;
      if (style == 2) {
        const float dx = x - w / 2.0f, dy = y - h / 2.0f;
        k = 0.65f + 0.35f * std::sin(std::sqrt(dx * dx + dy * dy) * 0.6f);
      }
      if (style == 3) k = 0.6f + 0.4f * ((x * 7 + y * 13) % 17) / 17.0f;
      unsigned char* q = &px[(static_cast<std::size_t>(y) * w + x) * 4];
      q[0] = static_cast<unsigned char>(r * k);
      q[1] = static_cast<unsigned char>(g * k);
      q[2] = static_cast<unsigned char>(b * k);
      q[3] = 255;
    }
  }
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  write_png_rgba(p.string(), w, h, px);
}

// A one-triangle glTF with an external buffer and a base colour texture.
void model(const fs::path& gltf, const std::string& texture_uri) {
  std::string bin(36, '\0');
  const float v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::memcpy(bin.data(), v, sizeof(v));
  fs::path binp = gltf;
  binp.replace_extension(".bin");
  spit(binp, bin);
  spit(gltf, R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":")" + binp.filename().string() +
                 R"("}],"bufferViews":[{"buffer":0,"byteLength":36}],)"
                 R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
                 R"("images":[{"uri":")" + texture_uri + R"("}],"textures":[{"source":0}],)"
                 R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
                 R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
}

void wav(const fs::path& p) {
  // 0.25 s of 8-bit mono silence-ish.
  const int n = 2756;
  std::string d = "RIFF";
  auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) d += static_cast<char>((v >> (8 * i)) & 0xFF); };
  auto u16 = [&](std::uint16_t v) { d += static_cast<char>(v & 0xFF); d += static_cast<char>(v >> 8); };
  u32(36 + n);
  d += "WAVEfmt ";
  u32(16);
  u16(1);
  u16(1);
  u32(11025);
  u32(11025);
  u16(1);
  u16(8);
  d += "data";
  u32(n);
  for (int i = 0; i < n; ++i) d += static_cast<char>(128 + static_cast<int>(40 * std::sin(i * 0.2)));
  spit(p, d);
}

fs::path fresh(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p / "assets", ec);
  spit(p / "project.json", R"({"name": ")" + p.filename().string() + R"(", "kind": "3d"})");
  // These claims check that pulls keep the library's paths; sorted pulls
  // (type folders) are covered by --smoke-meta.
  spit(p / ".tombstone/import.json", R"({"sort_imports": false, "keep_model_packs": true})");
  return p;
}

std::string guid_of(const fs::path& project, const std::string& rel) {
  adb::Meta m;
  const std::string file = ts::tombstone::core::asset_meta::find_for_asset(project.string(), rel);
  if (file.empty() || !adb::read_meta(file, &m)) return {};
  return m.guid;
}

std::string sha_of(const fs::path& project, const std::string& rel) {
  return tv::sha256_file((project / rel).string());
}

// The source claim: crate model (+ .bin) using a crate texture, a prefab
// stacking crates, a bell sound.
void seed_source(const fs::path& a) {
  texture(a / "assets/textures/crate.png", 64, 64, 176, 120, 64, 0);
  model(a / "assets/models/crate.gltf", "../textures/crate.png");
  spit(a / "assets/prefabs/crate_stack.prefab.json",
       R"({"prefab": 1, "name": "Crate stack", "root": {"name": "Stack", "children": [)"
       R"({"name": "Crate", "mesh": {"model": "assets/models/crate.gltf"}}]}})");
  wav(a / "assets/sfx/bell.wav");
}

bool scan(const fs::path& p, adb::AssetDb* db) {
  db->set_importer(adb::import_asset);
  db->open(p.string());
  db->scan();
  return true;
}

// Checks a claim holds the crate exactly like the source one.
int same_crate(const fs::path& src, const fs::path& dst, const std::string& label) {
  for (const char* rel : {"assets/models/crate.gltf", "assets/models/crate.bin", "assets/textures/crate.png"}) {
    if (!fs::exists(dst / rel)) return fail(label + ": " + rel + " missing after pull");
    if (sha_of(src, rel) != sha_of(dst, rel)) return fail(label + ": " + rel + " hash differs");
  }
  for (const char* rel : {"assets/models/crate.gltf", "assets/textures/crate.png"}) {
    if (guid_of(src, rel).empty() || guid_of(src, rel) != guid_of(dst, rel)) {
      return fail(label + ": GUID of " + rel + " not kept (" + guid_of(src, rel) + " vs " + guid_of(dst, rel) + ")");
    }
  }
  // The claim's own scan agrees: no new GUIDs, no moves, the model imports.
  adb::AssetDb db;
  db.set_importer(adb::import_asset);
  db.open(dst.string());
  const auto rep = db.scan();
  if (!rep.moved.empty() || !rep.duplicated.empty()) return fail(label + ": scan saw moves / copies after a pull");
  for (const std::string& c : rep.created) {
    if (c.find("crate") != std::string::npos) return fail(label + ": scan gave " + c + " a fresh GUID");
  }
  const auto* m = db.by_path("assets/models/crate.gltf");
  if (!m || m->meta.guid != guid_of(src, "assets/models/crate.gltf")) return fail(label + ": asset db GUID");
  if (!m->import_ok) return fail(label + ": pulled model does not import: " + m->import_error);
  return 0;
}

int check_local(const fs::path& root) {
  const fs::path a = fresh(root / "claim_a");
  seed_source(a);
  tv::LocalBackend lib((root / "local_vault").string());
  std::string err;
  // Plan: the model brings its .bin and .meta, and depends on the texture.
  adb::AssetDb db;
  scan(a, &db);
  std::vector<vp::Package> plan;
  if (!vp::plan_push(a.string(), db, "assets/models/crate.gltf", &plan, &err)) return fail("plan: " + err);
  if (plan.size() != 2 || plan[0].main != "assets/textures/crate.png" || plan[1].kind != "model") {
    return fail("plan: texture dependency first, then the model");
  }
  bool has_bin = false, has_meta = false;
  for (const auto& f : plan[1].files) {
    has_bin |= f.rel == "assets/models/crate.bin";
    has_meta |= f.role == "meta" && f.guid == guid_of(a, "assets/models/crate.gltf");
  }
  if (!has_bin || !has_meta || plan[1].dep_mains.size() != 1) return fail("plan: model files / deps");

  vp::PushOptions po;
  po.author = "Brenden";
  po.licence = "CC0-1.0";
  po.tags = {"props", "western"};
  po.dims = {"3d", "vr"};
  po.note = "first cut";
  vp::PushResult pr;
  if (!vp::push_asset(a.string(), &db, "assets/models/crate.gltf", lib, po, &pr, &err)) return fail("local push: " + err);
  if (pr.version != 1 || pr.pushed.size() != 2 || pr.item != guid_of(a, "assets/models/crate.gltf")) {
    return fail("local push result");
  }
  std::vector<tv::Item> items;
  if (!lib.list({}, &items, &err) || items.size() != 2) return fail("local list: 2 items");
  tv::Item crate;
  if (!lib.get(pr.item, &crate, &err)) return fail("local get: " + err);
  if (crate.kind != "model" || crate.author != "Brenden" || crate.licence != "CC0-1.0" || crate.dims.size() != 2 ||
      crate.tags.size() != 2 || crate.thumbnail.empty() || crate.versions.size() != 1 ||
      crate.versions[0].deps.size() != 1) {
    return fail("local item metadata (kind, author, licence, dims, tags, thumbnail, deps)");
  }
  tv::Query q;
  q.dim = "vr";
  q.text = "west";
  if (!lib.list(q, &items, &err) || items.size() != 2) return fail("search by tag text + dim");
  q.kind = "texture";
  if (!lib.list(q, &items, &err) || items.size() != 1) return fail("filter by kind");
  // Unchanged content: no new version.
  if (!vp::push_asset(a.string(), &db, "assets/models/crate.gltf", lib, po, &pr, &err) || pr.created || pr.version != 1) {
    return fail("re-push of unchanged content keeps v1");
  }
  // Pull into two separate claims.
  const fs::path b = fresh(root / "claim_b"), c = fresh(root / "claim_c");
  for (const fs::path& p : {b, c}) {
    vp::PullResult r;
    if (!vp::pull_item(p.string(), nullptr, lib, crate.id, {}, &r, &err)) return fail("local pull: " + err);
    if (r.main != "assets/models/crate.gltf" || r.items.size() != 2) return fail("local pull result");
    if (const int rc = same_crate(a, p, "local " + p.filename().string())) return rc;
  }
  // Prefab: prefab + its model (already present in c: skipped, not re-copied).
  if (!vp::push_asset(a.string(), &db, "assets/prefabs/crate_stack.prefab.json", lib, po, &pr, &err)) {
    return fail("prefab push: " + err);
  }
  tv::Item stack;
  lib.get(pr.item, &stack, &err);
  if (stack.kind != "prefab" || stack.versions.empty() || stack.versions[0].deps.size() != 1 ||
      stack.versions[0].deps[0].item != crate.id) {
    return fail("prefab item depends on the crate model");
  }
  vp::PullResult r;
  if (!vp::pull_item(c.string(), nullptr, lib, stack.id, {}, &r, &err)) return fail("prefab pull: " + err);
  if (r.written.size() != 1 || slurp(c / "assets/prefabs/crate_stack.prefab.json") != slurp(a / "assets/prefabs/crate_stack.prefab.json")) {
    return fail("prefab pull writes just the prefab when the model is already there");
  }
  const std::string stack_id_again = pr.item;
  if (!vp::push_asset(a.string(), &db, "assets/prefabs/crate_stack.prefab.json", lib, po, &pr, &err) ||
      pr.item != stack_id_again || pr.created) {
    return fail("prefab keeps its library id across pushes (lock)");
  }
  // A conflicting file (different asset at the same path) is not overwritten.
  const fs::path d = fresh(root / "claim_d");
  texture(d / "assets/textures/crate.png", 8, 8, 10, 200, 10, 1);
  if (vp::pull_item(d.string(), nullptr, lib, crate.id, {}, &r, &err)) return fail("pull over a different file must refuse");
  if (err.find("conflict") == std::string::npos) return fail("conflict message: " + err);

  // Exports: b uses the crate in its scene; a pulled bell it never uses stays home.
  if (!vp::push_asset(a.string(), &db, "assets/sfx/bell.wav", lib, po, &pr, &err)) return fail("push bell: " + err);
  if (!vp::pull_item(b.string(), nullptr, lib, pr.item, {}, &r, &err)) return fail("pull bell: " + err);
  spit(b / "scene3d.json", R"({"entities": [{"name": "Crate", "mesh": {"model": "assets/models/crate.gltf"}}]})");
  const std::vector<std::string> content = exporter::content_files_with_metas(b.string());
  const std::set<std::string> unused = tv::unused_pulled_files(b.string(), content);
  if (!unused.count("assets/sfx/bell.wav") || !unused.count("assets/sfx/bell.wav.meta")) {
    return fail("export leaves out the unused pulled sound");
  }
  for (const std::string& f : unused) {
    if (f.find("crate") != std::string::npos) return fail("export keeps the used crate and its texture (" + f + ")");
  }
  for (const std::string& f : content) {
    if (f.rfind(".vault/", 0) == 0) return fail("the lock file stays out of the pak");
  }
  std::cout << "[smoke] vault local: model + texture dep pushed, pulled into 2 claims (GUIDs + SHA-256 kept), "
               "prefab + deps, conflict refused, unused pull pruned from exports\n";
  return 0;
}

int check_remote(const fs::path& root) {
  tv::VaultStore store;
  std::string err;
  if (!store.open((root / "server").string(), &err)) return fail("server store: " + err);
  const std::string key = store.add_key("brenden", true, &err);
  const std::string ro_key = store.add_key("viewer", false, &err);
  if (key.empty() || ro_key.empty()) return fail("add keys: " + err);
  std::vector<std::string> log;
  std::mutex log_mu;
  tv::VaultServer server(store);
  tv::ServerOptions so;
  so.port = 0;
  so.log = [&](const std::string& l) {
    std::lock_guard<std::mutex> lock(log_mu);
    log.push_back(l);
  };
  if (!server.start(so, &err)) return fail("start ts_vault: " + err);
  const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
  int rc = 0;
  auto check = [&]() -> int {
    // Bad key: rejected, and the key never shows up in errors or logs.
    tv::RemoteBackend bad(url, "tsv_not_a_real_key_123");
    std::vector<tv::Item> items;
    if (bad.list({}, &items, &err)) return fail("a bad API key was accepted");
    if (err.find("401") == std::string::npos) return fail("bad key error: " + err);
    if (err.find("tsv_not_a_real_key_123") != std::string::npos) return fail("error text leaks the key");
    std::string info;
    if (bad.ping(&info, &err)) return fail("ping with a bad key");
    tv::RemoteBackend nokey(url, "");
    if (nokey.list({}, &items, &err)) return fail("no API key was accepted");
    tv::ClientResult health;
    if (!tv::http_request("GET", url + "/api/v1/health", {}, "", &health, &err) || health.status != 200) {
      return fail("health without a key");
    }
    tv::RemoteBackend ro(url, ro_key);
    // Remote push with small chunks.
    tv::RemoteOptions opt;
    opt.chunk_size = 4096;
    tv::RemoteBackend lib(url, key, opt);
    if (!lib.ping(&info, &err)) return fail("ping: " + err);
    const fs::path a = fresh(root / "rclaim_a");
    seed_source(a);
    vp::PushOptions po;
    po.author = "Brenden";
    po.licence = "CC-BY-4.0";
    po.dims = {"3d"};
    po.tags = {"props"};
    vp::PushResult pr;
    if (vp::push_asset(a.string(), nullptr, "assets/models/crate.gltf", ro, po, &pr, &err)) {
      return fail("a read-only key pushed");
    }
    if (!vp::push_asset(a.string(), nullptr, "assets/models/crate.gltf", lib, po, &pr, &err)) return fail("remote push: " + err);
    if (pr.version != 1 || pr.pushed.size() != 2) return fail("remote push result");
    if (lib.stats().chunks < 3) return fail("the texture went up in several chunks");
    const std::string crate_id = pr.item;
    // Pull into two separate claims (read-only key is enough to pull).
    const fs::path b = fresh(root / "rclaim_b"), c = fresh(root / "rclaim_c");
    vp::PullResult r;
    if (!vp::pull_item(b.string(), nullptr, lib, crate_id, {}, &r, &err)) return fail("remote pull b: " + err);
    if (!vp::pull_item(c.string(), nullptr, ro, crate_id, {}, &r, &err)) return fail("remote pull c (read-only key): " + err);
    if (const int e = same_crate(a, b, "remote b")) return e;
    if (const int e = same_crate(a, c, "remote c")) return e;
    // v2: a new texture -> texture v2 and model v2 (its dependency moved).
    texture(a / "assets/textures/crate.png", 64, 64, 90, 140, 200, 1);
    po.note = "blue crates";
    if (!vp::push_asset(a.string(), nullptr, "assets/models/crate.gltf", lib, po, &pr, &err)) return fail("push v2: " + err);
    if (pr.version != 2 || !pr.created) return fail("model v2 after its texture changed");
    std::vector<vp::ItemStatus> st;
    if (!vp::project_status(b.string(), lib, &st, &err) || st.size() != 2) return fail("status: " + err);
    for (const auto& s : st) {
      if (!s.update_available || s.latest != 2 || s.version != 1) return fail("update available for " + s.name);
    }
    // The panel's badge (Asset Browser tiles) says the same.
    {
      ed::VaultPanel panel;
      ed::VaultHost host;
      panel.open(b.string(), host);
      panel.use_library("remote", "", url, key);
      panel.refresh();
      for (int i = 0; i < 500 && panel.busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        panel.update();
      }
      panel.update();
      if (panel.badge("assets/models/crate.gltf") != 2) return fail("update badge on the pulled model");
      if (panel.origin_label("assets/models/crate.gltf").find("update v2") == std::string::npos) {
        return fail("origin label: " + panel.origin_label("assets/models/crate.gltf"));
      }
    }
    // Pin in c, then update both.
    if (!vp::set_pinned(c.string(), crate_id, true, &err)) return fail("pin: " + err);
    std::vector<std::string> done;
    if (!vp::update_all(b.string(), nullptr, lib, &done, &err) || done.size() != 2) return fail("update b: " + err);
    if (sha_of(a, "assets/textures/crate.png") != sha_of(b, "assets/textures/crate.png")) return fail("b has the v2 texture");
    if (guid_of(a, "assets/textures/crate.png") != guid_of(b, "assets/textures/crate.png")) return fail("v2 keeps the GUID");
    if (!vp::project_status(b.string(), lib, &st, &err)) return fail("status after update");
    for (const auto& s : st) {
      if (s.update_available || s.version != 2 || s.modified) return fail("b is current after the update");
    }
    done.clear();
    if (!vp::update_all(c.string(), nullptr, lib, &done, &err)) return fail("update c: " + err);
    tv::ProjectLock lc;
    lc.load(c.string());
    if (!lc.find(crate_id) || lc.find(crate_id)->version != 1 || !lc.find(crate_id)->pinned) {
      return fail("a pinned model stays at v1");
    }
    // Explicit older version pins it.
    const fs::path e = fresh(root / "rclaim_e");
    vp::PullOptions old;
    old.version = 1;
    if (!vp::pull_item(e.string(), nullptr, lib, crate_id, old, &r, &err) || r.version != 1) return fail("pull v1: " + err);
    tv::ProjectLock le;
    le.load(e.string());
    if (!le.find(crate_id) || !le.find(crate_id)->pinned) return fail("pulling an older version pins it");
    // Resumable upload: drop the connection after 3 chunks, then resume.
    texture(a / "assets/textures/big_sign.png", 192, 160, 200, 60, 40, 2);
    const std::uint64_t big = fs::file_size(a / "assets/textures/big_sign.png");
    tv::RemoteOptions cut = opt;
    cut.chunk_size = 16384;
    cut.abort_after_chunks = 3;
    tv::RemoteBackend flaky(url, key, cut);
    if (vp::push_asset(a.string(), nullptr, "assets/textures/big_sign.png", flaky, po, &pr, &err)) {
      return fail("the interrupted upload should fail");
    }
    if (err.find("interrupted") == std::string::npos) return fail("interrupted upload error: " + err);
    const std::string big_sha = sha_of(a, "assets/textures/big_sign.png");
    if (store.has_blob(big_sha)) return fail("half an upload must not become a blob");
    tv::RemoteOptions resume = opt;
    resume.chunk_size = 16384;
    tv::RemoteBackend again(url, key, resume);
    if (!vp::push_asset(a.string(), nullptr, "assets/textures/big_sign.png", again, po, &pr, &err)) {
      return fail("resumed push: " + err);
    }
    if (again.stats().resumed < 1) return fail("the second push resumed the open upload");
    // No byte went up twice: what the first try sent is not sent again.
    tv::Item sign;
    if (!again.get(pr.item, &sign, &err)) return fail("get the resumed item: " + err);
    std::uint64_t thumb = sign.thumbnail.empty() ? 0 : store.blob_size(sign.thumbnail);
    for (const auto& f : sign.versions.back().files) thumb += f.path == "assets/textures/big_sign.png" ? 0 : f.size;
    const std::uint64_t sent = flaky.stats().bytes_uploaded + again.stats().bytes_uploaded;
    if (sent > big + thumb || flaky.stats().bytes_uploaded == 0) {
      return fail("resume re-sent data: " + std::to_string(sent) + " bytes for " + std::to_string(big + thumb));
    }
    if (!store.has_blob(big_sha) || tv::sha256_file(store.blob_path(big_sha)) != big_sha) return fail("resumed blob hash");
    if (!vp::pull_item(b.string(), nullptr, lib, pr.item, {}, &r, &err) ||
        sha_of(b, "assets/textures/big_sign.png") != big_sha) {
      return fail("pull the resumed texture: " + err);
    }
    // A chunk at the wrong offset gets 409 with where to continue.
    tv::Headers h{{"Authorization", "Bearer " + key}, {"Content-Type", "application/json"}};
    tv::ClientResult up;
    const std::string body = R"({"sha256":")" + tv::sha256_hex("abcdefgh") + R"(","size":8})";
    if (!tv::http_request("POST", url + "/api/v1/uploads", h, body, &up, &err) || up.status != 200) return fail("open upload");
    json::Value uj;
    json::parse(up.body, &uj, &err);
    tv::ClientResult gap;
    tv::http_request("PUT", url + "/api/v1/uploads/" + uj.str("id") + "?offset=4", {{"Authorization", "Bearer " + key}},
                     "efgh", &gap, &err);
    if (gap.status != 409) return fail("chunk past the received offset -> 409");
    // A chunk that lies about its hash is discarded.
    tv::ClientResult put, done_r;
    tv::http_request("PUT", url + "/api/v1/uploads/" + uj.str("id") + "?offset=0", {{"Authorization", "Bearer " + key}},
                     "XXXXXXXX", &put, &err);
    tv::http_request("POST", url + "/api/v1/uploads/" + uj.str("id") + "/complete", {{"Authorization", "Bearer " + key}}, "",
                     &done_r, &err);
    if (put.status != 200 || done_r.status != 422) return fail("hash mismatch on complete -> 422");
    {
      std::lock_guard<std::mutex> lock(log_mu);
      for (const std::string& l : log) {
        if (l.find(key) != std::string::npos || l.find("tsv_not_a_real_key_123") != std::string::npos) {
          return fail("the server log contains an API key");
        }
      }
    }
    std::cout << "[smoke] vault remote: in-process ts_vault on " << url << ", " << lib.stats().chunks
              << " chunk(s) up, pulled into 2 claims (GUIDs + SHA-256 kept), v2 + update badge, pin, "
              << "upload cut after 3 chunks then resumed (" << flaky.stats().bytes_uploaded << " + "
              << again.stats().bytes_uploaded << " bytes for a " << big << "-byte texture + .meta + thumbnail), bad / missing / read-only keys refused\n";
    return 0;
  };
  rc = check();
  server.stop();
  return rc;
}

}  // namespace

bool vault_demo(const std::string& root_s, std::string* project, std::string* library, std::string* model_item,
                std::string* error) {
  const fs::path root = fs::u8path(root_s);
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path src = fresh(root / "workshop");
  seed_source(src);
  texture(src / "assets/textures/barn_wood.png", 96, 96, 150, 92, 52, 0);
  texture(src / "assets/textures/saloon_tiles.png", 96, 96, 190, 170, 120, 1);
  texture(src / "assets/textures/brass_plate.png", 96, 96, 214, 170, 70, 2);
  texture(src / "assets/textures/dust_ground.png", 96, 96, 168, 140, 100, 3);
  texture(src / "assets/textures/wanted_poster.png", 80, 108, 225, 205, 160, 3);
  texture(src / "assets/textures/lantern_glass.png", 96, 96, 240, 180, 60, 2);
  tv::LocalBackend lib((root / "vault").string());
  vp::PushOptions po;
  po.author = "Brenden";
  po.licence = "CC0-1.0";
  vp::PushResult pr;
  struct Up {
    const char* rel;
    const char* tags;
    const char* dims;
  };
  const Up ups[] = {{"assets/textures/barn_wood.png", "wood, western", "2d,3d"},
                    {"assets/textures/saloon_tiles.png", "floor, saloon", "2d,3d"},
                    {"assets/textures/brass_plate.png", "metal", "3d,vr"},
                    {"assets/textures/dust_ground.png", "ground, desert", "2d"},
                    {"assets/textures/wanted_poster.png", "props, ui", "2d,3d,vr"},
                    {"assets/textures/lantern_glass.png", "props, light", "3d,vr"},
                    {"assets/models/crate.gltf", "props, western", "3d,vr"},
                    {"assets/prefabs/crate_stack.prefab.json", "props", "3d"},
                    {"assets/sfx/bell.wav", "sfx, town", "2d,3d,vr"}};
  for (const Up& u : ups) {
    po.tags = tv::split_tags(u.tags);
    po.dims = tv::split_tags(u.dims);
    if (!vp::push_asset(src.string(), nullptr, u.rel, lib, po, &pr, error)) return false;
    if (std::string(u.rel) == "assets/models/crate.gltf") *model_item = pr.item;
  }
  const fs::path claim = fresh(root / "claim");
  vp::PullResult r;
  if (!vp::pull_item(claim.string(), nullptr, lib, *model_item, {}, &r, error)) return false;
  // A newer crate texture: the claim's crate gets an update badge.
  texture(src / "assets/textures/crate.png", 64, 64, 200, 140, 70, 1);
  po.tags = {"props", "western"};
  po.dims = {"3d", "vr"};
  po.note = "rebalanced planks";
  if (!vp::push_asset(src.string(), nullptr, "assets/models/crate.gltf", lib, po, &pr, error)) return false;
  *project = claim.string();
  *library = (root / "vault").string();
  return true;
}

int run_vault_smoke() {
  const fs::path root = fs::temp_directory_path() / ("ts_vault_smoke_" + tv::random_hex(4));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  int rc = check_local(root);
  if (rc == 0) rc = check_remote(root);
  fs::remove_all(root, ec);
  if (rc == 0) std::cout << "[smoke] vault OK\n";
  return rc;
}

}  // namespace tombstone
}  // namespace ts
