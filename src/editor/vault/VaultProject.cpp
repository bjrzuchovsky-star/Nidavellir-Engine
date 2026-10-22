#include "editor/vault/VaultProject.h"

#include "core/AssetMeta.h"
#include "core/PngWrite.h"
#include "editor/assets/ImportSort.h"
#include "editor/assets/AssetImport.h"
#include "gfx/TextureCache.h"
#include "render3d/ModelImport.h"
#include "vault/Sha256.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace editor {
namespace vaultp {

namespace fs = std::filesystem;
namespace am = core::asset_meta;
using json::Value;
using vault::Backend;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && lower(s.substr(s.size() - tail.size())) == tail;
}

bool is_prefab(const std::string& rel) { return ends_with(rel, ".prefab.json"); }

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// "a/b/../c.png" -> "a/c.png"; "" when it leaves the project.
std::string norm_rel(const std::string& rel) {
  std::string r = rel;
  std::replace(r.begin(), r.end(), '\\', '/');
  const std::string n = fs::path(r).lexically_normal().generic_string();
  if (n.empty() || n == "." || n.rfind("..", 0) == 0 || n[0] == '/' || n.find(':') != std::string::npos) return {};
  return n;
}

std::string dir_of(const std::string& rel) {
  const auto s = rel.find_last_of('/');
  return s == std::string::npos ? std::string() : rel.substr(0, s);
}

std::string join_rel(const std::string& dir, const std::string& ref) {
  return norm_rel(dir.empty() ? ref : dir + "/" + ref);
}

std::string to_rel(const fs::path& root, const std::string& abs) {
  std::error_code ec;
  const std::string r = fs::relative(fs::u8path(abs), root, ec).generic_string();
  return ec ? std::string() : norm_rel(r);
}

// A Vault / lock file path on disk. A .meta is named by its logical
// sidecar path ("assets/a.png.meta", what libraries hold since the first
// Vault and what older claims had on disk) and lives in the claim's
// metadata folder (core/AssetMeta.h); a legacy sidecar is still found.
fs::path disk_path(const fs::path& root, const std::string& rel) {
  if (!am::is_meta_rel(rel)) return root / fs::u8path(rel);
  const std::string found = am::resolve(root.string(), rel);
  return fs::u8path(found.empty() ? am::meta_path(root.string(), am::asset_of(rel)) : found);
}

// Library paths from a claim that kept metadata in ".tombstone/meta/..."
// read as the logical "<asset>.meta".
std::string logical_rel(const std::string& rel) {
  const std::string store = std::string(am::kMetaDir) + "/";
  if (rel.rfind(store, 0) == 0 && am::is_meta_rel(rel)) return am::asset_of(rel) + am::kMetaExt;
  return rel;
}

std::string percent_decode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string display_name(const std::string& rel) {
  std::string n = fs::path(rel).filename().string();
  if (is_prefab(n)) return n.substr(0, n.size() - 12);
  const auto dot = n.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
}

// Texture files a model names: glTF images, OBJ material maps.
std::vector<std::string> model_texture_refs(const fs::path& root, const std::string& rel) {
  std::vector<std::string> out;
  const std::string abs = (root / fs::u8path(rel)).string();
  if (ends_with(rel, ".gltf")) {
    Value g;
    std::string err;
    if (json::parse(slurp(fs::u8path(abs)), &g, &err)) {
      for (const Value& img : g["images"].items()) {
        const std::string uri = img.str("uri");
        if (uri.empty() || uri.rfind("data:", 0) == 0) continue;
        const std::string r = join_rel(dir_of(rel), percent_decode(uri));
        if (!r.empty()) out.push_back(r);
      }
    }
  } else if (ends_with(rel, ".obj")) {
    for (const std::string& dep : render3d::model_dependencies(abs)) {
      if (!ends_with(dep, ".mtl")) continue;
      const std::string mtl_rel = to_rel(root, dep);
      std::istringstream in(slurp(fs::u8path(dep)));
      for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        const std::string k = lower(key);
        if (k.rfind("map_", 0) != 0 && k != "bump" && k != "disp" && k != "decal" && k != "norm" && k != "refl") continue;
        std::string tok, last;
        while (ls >> tok) last = tok;
        if (last.empty()) continue;
        const std::string r = join_rel(dir_of(mtl_rel), last);
        if (!r.empty()) out.push_back(r);
      }
    }
  }
  return out;
}

void collect_strings(const Value& v, std::vector<std::string>* out) {
  if (v.is_string()) out->push_back(v.as_string());
  else if (v.is_array()) for (const Value& x : v.items()) collect_strings(x, out);
  else if (v.is_object()) for (const auto& [k, x] : v.members()) collect_strings(x, out);
}

std::unique_ptr<assetdb::AssetDb> own_db(const std::string& project_dir) {
  auto db = std::make_unique<assetdb::AssetDb>();
  db->set_importer(assetdb::import_asset);
  db->open(project_dir);
  db->scan();
  return db;
}

bool fail(std::string* error, const std::string& e) {
  if (error) *error = e;
  return false;
}

std::string temp_file(const std::string& ext) {
  return (fs::temp_directory_path() / ("ts_vault_" + vault::random_hex(8) + ext)).string();
}

}  // namespace

std::string vault_kind(const std::string& rel) {
  if (is_prefab(rel)) return "prefab";
  const assetdb::Kind k = assetdb::kind_for(rel);
  return k == assetdb::Kind::Unknown ? std::string() : assetdb::kind_name(k);
}

bool plan_push(const std::string& project_dir, assetdb::AssetDb& db, const std::string& rel_in,
               std::vector<Package>* out, std::string* error) {
  out->clear();
  const fs::path root = fs::u8path(project_dir);
  vault::ProjectLock lock;
  lock.load(project_dir);
  std::set<std::string> visiting;
  std::function<bool(const std::string&)> add = [&](const std::string& rel) -> bool {
    for (const Package& p : *out) {
      if (p.main == rel) return true;
    }
    if (!visiting.insert(rel).second) return true;  // a cycle: already on the way
    Package p;
    p.main = rel;
    std::set<std::string> have;
    auto add_file = [&](const std::string& r, const std::string& role, const std::string& guid) {
      if (r.empty() || !have.insert(r).second) return;
      std::error_code ec;
      if (!fs::is_regular_file(disk_path(root, r), ec)) return;
      PackFile f;
      f.rel = r;
      f.role = role;
      f.guid = guid;
      p.files.push_back(f);
    };
    std::vector<std::string> dep_candidates;
    if (const assetdb::AssetDb::Entry* e = db.by_path(rel)) {
      p.item = e->meta.guid;
      p.guid = e->meta.guid;
      p.kind = assetdb::kind_name(e->meta.kind);
      add_file(rel, "main", p.guid);
      add_file(rel + assetdb::kMetaExt, "meta", p.guid);
      if (e->meta.kind == assetdb::Kind::Model) {
        for (const std::string& dep : render3d::model_dependencies(db.abs(rel))) add_file(to_rel(root, dep), "file", "");
        for (const std::string& t : model_texture_refs(root, rel)) {
          if (db.by_path(t)) dep_candidates.push_back(t);
          else add_file(t, "file", "");  // e.g. a .dds the browser does not import
        }
      }
    } else if (is_prefab(rel)) {
      std::error_code ec;
      if (!fs::is_regular_file(root / fs::u8path(rel), ec)) return fail(error, rel + " does not exist");
      const vault::LockEntry* le = lock.find_by_path(rel);
      p.item = le ? le->item : assetdb::new_guid();
      p.guid = p.item;
      p.kind = "prefab";
      add_file(rel, "main", p.guid);
      Value doc;
      std::string err;
      if (!json::parse(slurp(root / fs::u8path(rel)), &doc, &err)) return fail(error, rel + ": " + err);
      std::vector<std::string> strs;
      collect_strings(doc, &strs);
      for (std::string s : strs) {
        if (s.rfind("prefab:", 0) == 0) s = s.substr(7);
        const std::string r = norm_rel(s);
        if (r.empty() || r == rel) continue;
        if (db.by_path(r) || (is_prefab(r) && fs::is_regular_file(root / fs::u8path(r), ec))) {
          if (std::find(dep_candidates.begin(), dep_candidates.end(), r) == dep_candidates.end()) dep_candidates.push_back(r);
        }
      }
    } else {
      return fail(error, "can't send " + rel + " to the Vault (textures, models, sounds, fonts and prefabs can go)");
    }
    for (const std::string& d : dep_candidates) {
      if (!add(d)) return false;
      if (std::find(p.dep_mains.begin(), p.dep_mains.end(), d) == p.dep_mains.end()) p.dep_mains.push_back(d);
    }
    for (PackFile& f : p.files) {
      f.sha = vault::sha256_file(disk_path(root, f.rel).string(), &f.size);
      if (f.sha.empty()) return fail(error, "cannot read " + f.rel);
    }
    out->push_back(std::move(p));
    return true;
  };
  const std::string rel = norm_rel(rel_in);
  if (rel.empty()) return fail(error, "not a project path: " + rel_in);
  return add(rel);
}

bool make_thumbnail(const std::string& image_abs, const std::string& out_png, std::string* error) {
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
  std::string err;
  if (!gfx::TextureCache::decode_file(image_abs, &rgba, &w, &h, &err) || w <= 0 || h <= 0) {
    return fail(error, "thumbnail: " + err);
  }
  const int m = std::max(w, h);
  const int tw = m > 128 ? std::max(1, w * 128 / m) : w;
  const int th = m > 128 ? std::max(1, h * 128 / m) : h;
  std::vector<unsigned char> out(static_cast<std::size_t>(tw) * th * 4);
  for (int y = 0; y < th; ++y) {
    for (int x = 0; x < tw; ++x) {
      // Box filter over the source pixels this one covers.
      const int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw);
      const int y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
      unsigned acc[4] = {0, 0, 0, 0}, n = 0;
      for (int sy = y0; sy < y1; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          const unsigned char* p = &rgba[(static_cast<std::size_t>(sy) * w + sx) * 4];
          for (int c = 0; c < 4; ++c) acc[c] += p[c];
          ++n;
        }
      }
      for (int c = 0; c < 4; ++c) out[(static_cast<std::size_t>(y) * tw + x) * 4 + c] = static_cast<unsigned char>(acc[c] / n);
    }
  }
  if (!write_png_rgba(out_png, tw, th, out)) return fail(error, "cannot write " + out_png);
  return true;
}

std::string cached_thumbnail(Backend& backend, const std::string& sha, const std::string& cache_dir) {
  if (!vault::is_sha256_hex(sha)) return {};
  const fs::path p = fs::u8path(cache_dir) / (sha + ".png");
  std::error_code ec;
  if (fs::is_regular_file(p, ec)) return p.string();
  fs::create_directories(p.parent_path(), ec);
  std::string err;
  if (!backend.download(sha, p.string(), &err)) return {};
  return p.string();
}

bool push_asset(const std::string& project_dir, assetdb::AssetDb* db_in, const std::string& rel,
                Backend& backend, const PushOptions& opt, PushResult* out, std::string* error) {
  *out = PushResult{};
  std::unique_ptr<assetdb::AssetDb> mine;
  assetdb::AssetDb* db = db_in;
  if (!db) {
    mine = own_db(project_dir);
    db = mine.get();
  }
  std::vector<Package> plan;
  if (!plan_push(project_dir, *db, rel, &plan, error)) return false;
  const fs::path root = fs::u8path(project_dir);
  vault::ProjectLock lock;
  std::string err;
  if (!lock.load(project_dir, &err)) return fail(error, err);
  std::map<std::string, vault::Dep> done;          // main rel -> item@version
  std::map<std::string, std::string> thumbs;       // main rel -> thumbnail sha
  auto upload_if_missing = [&](const std::vector<std::pair<std::string, std::string>>& files) -> bool {
    std::vector<std::string> shas;
    for (const auto& f : files) shas.push_back(f.second);
    std::vector<std::string> missing;
    if (!backend.missing(shas, &missing, error)) return false;
    for (const auto& [abs, sha] : files) {
      if (std::find(missing.begin(), missing.end(), sha) == missing.end()) continue;
      std::uint64_t size = 0;
      if (vault::sha256_file(abs, &size) != sha) return fail(error, abs + " changed while sending");
      if (!backend.upload(abs, sha, size, error)) return false;
      out->bytes += size;
      missing.erase(std::remove(missing.begin(), missing.end(), sha), missing.end());
    }
    return true;
  };
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const Package& p = plan[i];
    const bool is_main = i + 1 == plan.size();
    std::vector<std::pair<std::string, std::string>> files;
    for (const PackFile& f : p.files) files.emplace_back(disk_path(root, f.rel).string(), f.sha);
    // Thumbnail: the user's picture, the texture itself, or a texture it uses.
    std::string thumb_sha;
    std::string thumb_src = is_main && !opt.thumbnail_png.empty() ? opt.thumbnail_png
                            : p.kind == "texture" ? (root / fs::u8path(p.main)).string() : std::string();
    if (!thumb_src.empty()) {
      const std::string tmp = temp_file(".png");
      std::string terr;
      if (make_thumbnail(thumb_src, tmp, &terr)) {
        thumb_sha = vault::sha256_file(tmp);
        files.emplace_back(tmp, thumb_sha);
      }
    }
    if (thumb_sha.empty()) {
      for (const std::string& d : p.dep_mains) {
        if (thumbs.count(d) && !thumbs[d].empty()) {
          thumb_sha = thumbs[d];
          break;
        }
      }
    }
    const bool ok = upload_if_missing(files);
    for (const auto& [abs, sha] : files) {
      if (abs.find("ts_vault_") != std::string::npos && fs::path(abs).parent_path() == fs::temp_directory_path()) {
        std::error_code ec;
        fs::remove(fs::u8path(abs), ec);
      }
    }
    if (!ok) return false;
    thumbs[p.main] = thumb_sha;
    vault::Item meta;
    meta.id = p.item;
    meta.kind = p.kind;
    meta.name = is_main && !opt.name.empty() ? opt.name : display_name(p.main);
    meta.author = opt.author;
    meta.licence = opt.licence;
    meta.description = is_main ? opt.description : "Used by " + display_name(plan.back().main);
    meta.tags = opt.tags;
    meta.dims = vault::normalise_dims(opt.dims);
    vault::Version v;
    v.note = is_main ? opt.note : "with " + display_name(plan.back().main);
    v.uploader = opt.author;
    v.thumbnail = thumb_sha;
    for (const PackFile& f : p.files) {
      vault::FileRec r;
      r.path = f.rel;
      r.sha256 = f.sha;
      r.size = f.size;
      r.role = f.role;
      r.guid = f.guid;
      v.files.push_back(r);
    }
    for (const std::string& d : p.dep_mains) {
      if (done.count(d)) v.deps.push_back(done[d]);
    }
    vault::PublishResult pr;
    if (!backend.publish(meta, v, &pr, error)) return false;
    done[p.main] = vault::Dep{p.item, pr.version};
    out->pushed.push_back(meta.name + " v" + std::to_string(pr.version) + (pr.created ? "" : " (unchanged)"));
    // Lock: this project now holds exactly that version.
    const vault::LockEntry* cur = lock.find(p.item);
    vault::LockEntry e;
    e.item = p.item;
    e.name = meta.name;
    e.kind = p.kind;
    e.origin = cur ? cur->origin : "pushed";
    e.library = backend.id();
    e.version = pr.version;
    e.pinned = cur ? cur->pinned : false;
    e.main = p.main;
    e.guid = p.guid;
    for (const vault::Dep& d : v.deps) e.deps.push_back(d.item);
    e.when = vault::utc_now_iso();
    for (const PackFile& f : p.files) e.files.push_back({f.rel, f.sha, f.guid});
    lock.upsert(e);
    if (is_main) {
      out->item = p.item;
      out->version = pr.version;
      out->created = pr.created;
    }
  }
  if (!lock.save(&err)) return fail(error, err);
  return true;
}

namespace {

struct PullCtx {
  fs::path root;
  assetdb::AssetDb* db = nullptr;
  Backend* backend = nullptr;
  vault::ProjectLock lock;
  PullOptions opt;
  PullResult* out = nullptr;
  std::set<std::string> done;
  std::map<std::string, std::string> remap;  // library path -> sorted claim path
};

bool files_present(const PullCtx& c, const vault::LockEntry& e) {
  std::error_code ec;
  for (const vault::LockFile& f : e.files) {
    std::string p = f.path;
    if (!f.guid.empty() && c.db) {
      const std::string now = c.db->path_of(f.guid);
      if (!now.empty()) p = ends_with(f.path, ".meta") ? now + ".meta" : now;
    }
    if (!fs::exists(disk_path(c.root, p), ec)) return false;
  }
  return true;
}

bool pull_one(PullCtx& c, const std::string& id, int version, bool is_root, std::string* error) {
  if (c.done.count(id)) return true;
  c.done.insert(id);
  vault::Item item;
  if (!c.backend->get(id, &item, error)) return false;
  const vault::Version* ver = item.version(version);
  if (!ver) return fail(error, item.name + ": no version " + std::to_string(version));
  for (const vault::Dep& d : ver->deps) {
    const vault::LockEntry* have = c.lock.find(d.item);
    if (have && have->version >= d.version && files_present(c, *have)) continue;
    if (!pull_one(c, d.item, d.version, false, error)) return false;
  }
  const vault::FileRec* main = nullptr;
  for (const vault::FileRec& f : ver->files) {
    if (f.role == "main") main = &f;
  }
  if (!main) return fail(error, item.name + " has no main file");
  // Where this asset already lives here (it may have been moved since).
  std::string from_dir, to_dir;
  if (c.db && !main->guid.empty()) {
    const std::string now = c.db->path_of(main->guid);
    if (!now.empty() && now != main->path) {
      from_dir = dir_of(main->path);
      to_dir = dir_of(now);
    }
  }
  auto dest_of = [&](const std::string& lib_path) {
    const std::string p = logical_rel(lib_path);
    const auto sorted = c.remap.find(p);
    if (sorted != c.remap.end()) return sorted->second;
    if (from_dir != to_dir && (from_dir.empty() || p.rfind(from_dir + "/", 0) == 0)) {
      const std::string tail = from_dir.empty() ? p : p.substr(from_dir.size() + 1);
      return to_dir.empty() ? tail : to_dir + "/" + tail;
    }
    return p;
  };
  const vault::LockEntry* prev = c.lock.find(id);
  struct Todo {
    const vault::FileRec* f;
    std::string dest;
    bool download;
  };
  std::vector<Todo> todo;
  for (const vault::FileRec& f : ver->files) {
    const std::string dest = dest_of(f.path);
    if (!vault::safe_rel_path(dest)) return fail(error, "unsafe path in the library: " + f.path);
    const fs::path abs = disk_path(c.root, dest);
    std::error_code ec;
    bool download = true;
    if (fs::exists(abs, ec)) {
      if (f.role == "meta") {
        Value m;
        std::string err;
        const bool ok = json::parse(slurp(abs), &m, &err);
        if (ok && !f.guid.empty() && m.str("guid") != f.guid && !c.opt.force) {
          return fail(error, "conflict: " + dest + " belongs to a different asset in this project (GUID " +
                                 m.str("guid") + "); move it away or pull with force");
        }
      } else {
        const std::string cur = vault::sha256_file(abs.string());
        if (cur == f.sha256) {
          download = false;
        } else {
          bool ours = false;
          if (prev) {
            for (const vault::LockFile& lf : prev->files) {
              if ((lf.path == dest || lf.path == f.path) && lf.sha256 == cur) ours = true;
            }
          }
          if (!ours && !c.opt.force) {
            return fail(error, prev ? dest + " was changed in this project since it was pulled; pull with force to replace it"
                                    : "conflict: " + dest + " already exists in this project and is a different file");
          }
        }
      }
    }
    todo.push_back({&f, dest, download});
  }
  const std::string main_dest = dest_of(main->path);
  for (const Todo& t : todo) {
    // A .meta goes to the metadata folder (a library item from a sidecar
    // claim converts here); any stray sidecar for it goes.
    const bool is_meta = t.f->role == "meta" || am::is_meta_rel(t.dest);
    const fs::path abs = is_meta ? fs::u8path(am::meta_path(c.root.string(), am::asset_of(t.dest)))
                                 : c.root / fs::u8path(t.dest);
    std::error_code dec;
    fs::create_directories(abs.parent_path(), dec);
    if (is_meta) {
      const fs::path side = fs::u8path(am::sidecar_path(c.root.string(), am::asset_of(t.dest)));
      if (fs::exists(side, dec) && fs::exists(abs, dec)) fs::remove(side, dec);
      if (fs::exists(side, dec) && !fs::exists(abs, dec)) {
        fs::rename(side, abs, dec);
      }
    }
    if (t.download) {
      if (!c.backend->download(t.f->sha256, abs.string(), error)) return false;
      c.out->written.push_back(t.dest);
    }
    if (t.f->role == "meta") {
      // The .meta keeps its GUID and settings; its path names where it is now.
      Value m;
      std::string err;
      if (json::parse(slurp(abs), &m, &err) && m.is_object()) {
        const std::string asset = t.dest.size() > 5 ? t.dest.substr(0, t.dest.size() - 5) : main_dest;
        if (m.str("path") != asset) {
          m.set("path", Value::string(asset));
          std::ofstream o(abs, std::ios::binary | std::ios::trunc);
          o << json::write(m);
        }
      }
    }
  }
  vault::LockEntry e;
  e.item = id;
  e.name = item.name;
  e.kind = item.kind;
  e.origin = prev ? prev->origin : "pulled";
  e.library = c.backend->id();
  e.version = ver->version;
  e.pinned = (is_root && c.opt.pin) || (is_root && version > 0 && version < item.latest) || (prev && prev->pinned);
  e.main = main_dest;
  e.guid = main->guid;
  for (const vault::Dep& d : ver->deps) e.deps.push_back(d.item);
  e.when = vault::utc_now_iso();
  for (const Todo& t : todo) e.files.push_back({t.dest, t.f->sha256, t.f->guid});
  c.lock.upsert(e);
  c.out->items.push_back(item.name + " v" + std::to_string(ver->version));
  if (is_root) {
    c.out->item = id;
    c.out->main = main_dest;
    c.out->kind = item.kind;
    c.out->version = ver->version;
  }
  return true;
}

// Sorted pulls: where a new item's files land in this claim. A model and
// the textures it brings keep their layout relative to each other under
// assets/Models/<Name>/; textures, sounds, fonts on their own go to their
// type folder. Items already here, and prefabs (which name their parts by
// path), keep the library's paths.
void plan_sorted_pull(PullCtx& c, const std::string& id, int version) {
  std::error_code ec;
  auto get_ver = [&](const std::string& item_id, int v, vault::Item* it) -> const vault::Version* {
    std::string err;
    if (!c.backend->get(item_id, it, &err)) return nullptr;
    return it->version(v);
  };
  auto main_of = [](const vault::Version& v) -> const vault::FileRec* {
    for (const vault::FileRec& f : v.files) {
      if (f.role == "main") return &f;
    }
    return nullptr;
  };
  auto here = [&](const std::string& item_id, const vault::FileRec* main) {
    if (c.lock.find(item_id)) return true;
    return main && c.db && !main->guid.empty() && !c.db->path_of(main->guid).empty();
  };
  vault::Item root_item;
  const vault::Version* rv = get_ver(id, version, &root_item);
  if (!rv) return;
  const vault::FileRec* rmain = main_of(*rv);
  if (!rmain || here(id, rmain) || root_item.kind == "prefab") return;
  const std::string main_path = logical_rel(rmain->path);
  auto asset_part = [](const std::string& p) { return am::is_meta_rel(p) ? am::asset_of(p) : p; };
  auto free_dest = [&](const std::string& rel) {
    return !fs::exists(c.root / fs::u8path(asset_part(rel)), ec);
  };
  // One item into its type folder (files under its main's folder keep
  // their place below it).
  auto sort_item = [&](const vault::Version& v, const vault::FileRec& m) {
    const std::string mp = logical_rel(m.path);
    const std::string from = dir_of(mp);
    const std::string to = importsort::type_folder(mp);
    std::map<std::string, std::string> mine;
    for (const vault::FileRec& f : v.files) {
      const std::string p = logical_rel(f.path);
      std::string dest = from.empty() ? to + "/" + p
                         : p.rfind(from + "/", 0) == 0 ? to + p.substr(from.size())
                                                       : to + "/" + fs::u8path(p).filename().string();
      if (!free_dest(dest) && asset_part(dest) != asset_part(p)) return;  // taken: keep the library paths
      mine[p] = dest;
    }
    c.remap.insert(mine.begin(), mine.end());
  };
  if (root_item.kind != "model") {
    sort_item(*rv, *rmain);
    return;
  }
  // A model pack: the model's files and the texture items it depends on.
  std::vector<std::string> pack;
  for (const vault::FileRec& f : rv->files) pack.push_back(logical_rel(f.path));
  for (const vault::Dep& d : rv->deps) {
    vault::Item di;
    const vault::Version* dv = get_ver(d.item, d.version, &di);
    if (!dv) continue;
    const vault::FileRec* dm = main_of(*dv);
    if (!dm || here(d.item, dm)) continue;
    if (di.kind == "texture") {
      for (const vault::FileRec& f : dv->files) pack.push_back(logical_rel(f.path));
    } else if (di.kind != "prefab") {
      sort_item(*dv, *dm);
    }
  }
  // Their common folder becomes assets/Models/<Name>/.
  std::string common = dir_of(main_path);
  for (const std::string& p : pack) {
    while (!common.empty() && p.rfind(common + "/", 0) != 0) common = dir_of(common);
  }
  std::string stem = fs::u8path(main_path).stem().string();
  if (stem.empty()) stem = "model";
  std::string target = "assets/Models/" + stem;
  for (int n = 2; fs::exists(c.root / fs::u8path(target), ec) && n < 1000; ++n) {
    target = "assets/Models/" + stem + "_" + std::to_string(n);
  }
  for (const std::string& p : pack) c.remap[p] = target + "/" + (common.empty() ? p : p.substr(common.size() + 1));
}

}  // namespace

bool pull_item(const std::string& project_dir, assetdb::AssetDb* db_in, Backend& backend, const std::string& item,
               const PullOptions& opt, PullResult* out, std::string* error) {
  *out = PullResult{};
  std::unique_ptr<assetdb::AssetDb> mine;
  PullCtx c;
  c.root = fs::u8path(project_dir);
  c.db = db_in;
  if (!c.db) {
    mine = own_db(project_dir);
    c.db = mine.get();
  }
  c.backend = &backend;
  c.opt = opt;
  c.out = out;
  std::string err;
  if (!c.lock.load(project_dir, &err)) return fail(error, err);
  if (opt.sort && importsort::load_settings(project_dir).sort_imports) plan_sorted_pull(c, item, opt.version);
  const bool ok = pull_one(c, item, opt.version, true, error);
  // Record what did land even when a later file failed.
  if (!c.lock.save(&err) && ok) return fail(error, err);
  return ok;
}

bool project_status(const std::string& project_dir, Backend& backend, std::vector<ItemStatus>* out,
                    std::string* error) {
  out->clear();
  vault::ProjectLock lock;
  std::string err;
  if (!lock.load(project_dir, &err)) return fail(error, err);
  if (lock.entries().empty()) return true;
  std::vector<vault::Item> items;
  if (!backend.list({}, &items, error)) return false;
  std::map<std::string, int> latest;
  for (const vault::Item& it : items) latest[it.id] = it.latest;
  const fs::path root = fs::u8path(project_dir);
  for (const vault::LockEntry& e : lock.entries()) {
    if (e.library != backend.id()) continue;
    ItemStatus s;
    s.item = e.item;
    s.name = e.name;
    s.kind = e.kind;
    s.main = e.main;
    s.origin = e.origin;
    s.library = e.library;
    s.version = e.version;
    s.pinned = e.pinned;
    const auto it = latest.find(e.item);
    s.missing = it == latest.end();
    s.latest = s.missing ? 0 : it->second;
    s.update_available = !s.missing && s.latest > e.version;
    for (const vault::LockFile& f : e.files) {
      if (ends_with(f.path, ".meta")) continue;
      std::error_code ec;
      const fs::path p = root / fs::u8path(f.path);
      if (!fs::exists(p, ec) || vault::sha256_file(p.string()) != f.sha256) {
        s.modified = true;
        break;
      }
    }
    out->push_back(s);
  }
  return true;
}

bool update_all(const std::string& project_dir, assetdb::AssetDb* db, Backend& backend,
                std::vector<std::string>* updated, std::string* error) {
  std::vector<ItemStatus> st;
  if (!project_status(project_dir, backend, &st, error)) return false;
  for (const ItemStatus& s : st) {
    if (!s.update_available || s.pinned || s.origin != "pulled") continue;
    PullResult r;
    if (!pull_item(project_dir, db, backend, s.item, {}, &r, error)) return false;
    if (updated) updated->push_back(s.name + " v" + std::to_string(s.version) + " -> v" + std::to_string(r.version));
  }
  return true;
}

bool set_pinned(const std::string& project_dir, const std::string& item, bool pinned, std::string* error) {
  vault::ProjectLock lock;
  std::string err;
  if (!lock.load(project_dir, &err)) return fail(error, err);
  vault::LockEntry* e = lock.find(item);
  if (!e) return fail(error, "this project has not pulled " + item);
  e->pinned = pinned;
  if (!lock.save(&err)) return fail(error, err);
  return true;
}

}  // namespace vaultp
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
