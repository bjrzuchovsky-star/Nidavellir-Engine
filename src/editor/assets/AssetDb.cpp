#include "editor/assets/AssetDb.h"

#include "core/AssetMeta.h"
#include "render3d/ModelImport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {
namespace assetdb {

namespace fs = std::filesystem;
namespace am = core::asset_meta;
using json::Value;

namespace {

std::string lower_ext(const std::string& path) {
  std::string ext = fs::u8path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

bool slurp(const fs::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

bool spit(const fs::path& p, const std::string& text) {
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::string hex64(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string clean_rel(std::string rel) {
  std::replace(rel.begin(), rel.end(), '\\', '/');
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  while (!rel.empty() && rel.back() == '/') rel.pop_back();
  return rel;
}

// Files scanned for references.
bool ref_file(const std::string& name) {
  const std::string ext = lower_ext(name);
  return ext == ".json" || ext == ".lua";
}

std::size_t count_of(const std::string& text, const std::string& needle) {
  if (needle.empty()) return 0;
  std::size_t n = 0;
  for (std::size_t pos = text.find(needle); pos != std::string::npos;
       pos = text.find(needle, pos + needle.size())) {
    ++n;
  }
  return n;
}

std::size_t replace_all(std::string& text, const std::string& from, const std::string& to) {
  std::size_t n = 0;
  for (std::size_t pos = text.find(from); pos != std::string::npos;
       pos = text.find(from, pos + to.size())) {
    text.replace(pos, from.size(), to);
    ++n;
  }
  return n;
}

// A Source .mdl's sibling files (.vvd, .dx90.vtx, .phy...) as
// (path, suffix after the base name): they move and delete with it.
std::vector<std::pair<fs::path, std::string>> mdl_siblings(const std::string& abs_mdl) {
  std::vector<std::pair<fs::path, std::string>> out;
  if (lower_ext(abs_mdl) != ".mdl") return out;
  fs::path base = fs::u8path(abs_mdl);
  base.replace_extension();
  const std::string b = base.string();
  for (const std::string& d : render3d::model_dependencies(abs_mdl)) {
    if (d.size() > b.size() && d.compare(0, b.size(), b) == 0) out.emplace_back(fs::u8path(d), d.substr(b.size()));
  }
  return out;
}

std::string local_time(const char* fmt) {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), fmt, &tm);
  return buf;
}

// Rename, or copy + delete across volumes.
bool move_file(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (!ec) return true;
  ec.clear();
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  fs::remove(from, ec);
  return true;
}

}  // namespace

Kind kind_for(const std::string& path) {
  const std::string e = lower_ext(path);
  if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga" || e == ".gif" ||
      e == ".vtf") {
    return Kind::Texture;
  }
  // glTF, Source .mdl, Assimp formats, .blend (render3d/ModelImport.h).
  if (render3d::is_model_file(path)) return Kind::Model;
  if (e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac") return Kind::Audio;
  if (e == ".ttf" || e == ".otf" || e == ".ttc") return Kind::Font;
  return Kind::Unknown;
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Texture: return "texture";
    case Kind::Model: return "model";
    case Kind::Audio: return "audio";
    case Kind::Font: return "font";
    default: return "unknown";
  }
}

Kind kind_from_name(const std::string& n) {
  if (n == "texture") return Kind::Texture;
  if (n == "model") return Kind::Model;
  if (n == "audio") return Kind::Audio;
  if (n == "font") return Kind::Font;
  return Kind::Unknown;
}

Value default_settings(Kind k, const std::string& path) {
  if (k == Kind::Model) return render3d::default_model_settings(path);
  Value s = Value::object();
  switch (k) {
    case Kind::Texture:
      s.set("filter", Value::string("nearest"));  // pixel art friendly
      s.set("mipmaps", Value::boolean(false));
      s.set("srgb", Value::boolean(true));
      s.set("max_size", Value::number(4096));
      s.set("pixels_per_unit", Value::number(32));
      break;
    case Kind::Audio:
      s.set("bus", Value::string("sfx"));
      s.set("volume", Value::number(1));
      s.set("loop", Value::boolean(false));
      s.set("stream", Value::boolean(false));
      break;
    case Kind::Font:
      s.set("size", Value::number(32));
      s.set("oversample", Value::number(2));
      break;
    default: break;
  }
  return s;
}

std::string new_guid() {
  static std::mt19937_64 rng([] {
    std::random_device rd;
    const auto t = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ t;
  }());
  return hex64(rng()) + hex64(rng());
}

std::string file_hash(const std::string& abs_path) {
  std::ifstream in(fs::u8path(abs_path), std::ios::binary);
  if (!in) return {};
  std::uint64_t h = 1469598103934665603ULL;
  char buf[65536];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize n = in.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
      h ^= static_cast<unsigned char>(buf[i]);
      h *= 1099511628211ULL;
    }
  }
  return hex64(h);
}

std::string asset_hash(const std::string& abs_path) {
  std::string h = file_hash(abs_path);
  if (h.empty() || !render3d::is_model_file(abs_path)) return h;
  const std::vector<std::string> deps = render3d::model_dependencies(abs_path);
  if (deps.empty()) return h;
  // Sibling files (Source .vvd / .vtx / .phy, OBJ .mtl...) count too, so
  // editing one of them hot-reloads the model.
  std::uint64_t x = 1469598103934665603ULL;
  for (const std::string& part : deps) h += "|" + file_hash(part);
  for (unsigned char c : h) {
    x ^= c;
    x *= 1099511628211ULL;
  }
  return hex64(x);
}

void asset_stat(const std::string& abs_path, std::filesystem::file_time_type* mtime,
                std::uintmax_t* size) {
  std::error_code ec;
  *mtime = fs::last_write_time(fs::u8path(abs_path), ec);
  *size = fs::file_size(fs::u8path(abs_path), ec);
  if (!render3d::is_model_file(abs_path)) return;
  for (const std::string& d : render3d::model_dependencies(abs_path)) {
    const auto t = fs::last_write_time(fs::u8path(d), ec);
    if (!ec && t > *mtime) *mtime = t;
    const auto n = fs::file_size(fs::u8path(d), ec);
    if (!ec) *size += n;
  }
}

bool read_meta(const std::string& meta_abs, Meta* out, std::string* error) {
  std::string text;
  if (!slurp(fs::u8path(meta_abs), &text)) {
    if (error) *error = "cannot read " + meta_abs;
    return false;
  }
  Value v;
  std::string err;
  if (!json::parse(text, &v, &err) || !v.is_object() || v.str("guid").size() != 32) {
    if (error) *error = "bad .meta " + meta_abs + (err.empty() ? "" : ": " + err);
    return false;
  }
  Meta m;
  m.guid = v.str("guid");
  m.kind = kind_from_name(v.str("kind"));
  m.path = v.str("path");
  m.hash = v.str("hash");
  if (v["settings"].is_object()) m.settings = v["settings"];
  if (v["info"].is_object()) m.info = v["info"];
  // Missing settings keys come back with their defaults.
  const Value defaults = default_settings(m.kind, m.path);
  for (const auto& [k, d] : defaults.members()) {
    if (!m.settings.has(k)) m.settings.set(k, d);
  }
  *out = std::move(m);
  return true;
}

bool write_meta(const std::string& meta_abs, const Meta& m, std::string* error) {
  Value v = Value::object();
  v.set("meta", Value::number(1));
  v.set("guid", Value::string(m.guid));
  v.set("kind", Value::string(kind_name(m.kind)));
  v.set("path", Value::string(m.path));
  v.set("hash", Value::string(m.hash));
  v.set("settings", m.settings);
  v.set("info", m.info);
  if (!spit(fs::u8path(meta_abs), json::write(v))) {
    if (error) *error = "cannot write " + meta_abs;
    return false;
  }
  return true;
}

bool AssetDb::ignored_dir(const std::string& n) {
  return n == "build" || n == "export" || n == "exports" || n == ".git" || n == ".tombstone" ||
         n == ".vault" || n == "TombStoneConfig" || n == "bin" || n == "obj" || n == ".vs" ||
         n == "node_modules" || n == "Shipped";
}

void AssetDb::open(const std::string& project_dir) {
  dir_ = project_dir;
  entries_.clear();
  extra_folders_.clear();
  migration_ = MigrationReport{};
  if (!dir_.empty()) migration_ = migrate();
}

std::string AssetDb::meta_abs(const std::string& rel) const { return am::meta_path(dir_, clean_rel(rel)); }

void AssetDb::log(const std::string& line) const {
  if (dir_.empty()) return;
  const fs::path p = fs::u8path(dir_) / fs::u8path(am::kLogFile);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::app);
  out << local_time("%Y-%m-%d %H:%M:%S") << "  " << line << "\n";
}

void AssetDb::prune_meta_dirs(const std::string& asset_rel) const {
  // Empty mirror folders left by a move / delete go too.
  std::error_code ec;
  const fs::path store = fs::u8path(am::store_dir(dir_));
  fs::path d = fs::u8path(meta_abs(asset_rel)).parent_path();
  while (!d.empty() && d != store && d.string().size() > store.string().size()) {
    if (!fs::is_directory(d, ec) || !fs::is_empty(d, ec)) break;
    fs::remove(d, ec);
    d = d.parent_path();
  }
}

void AssetDb::adopt_sidecars(const std::vector<std::string>& sidecars, MigrationReport* rep) {
  std::error_code ec;
  const fs::path root = fs::u8path(dir_);
  for (const std::string& side_rel : sidecars) {
    const std::string asset = side_rel.substr(0, side_rel.size() - std::string(kMetaExt).size());
    const fs::path side = root / fs::u8path(side_rel);
    Meta sm;
    if (!read_meta(side.string(), &sm)) continue;  // not ours (no GUID): leave it alone
    if (rep->backup_dir.empty()) {
      rep->backup_dir = std::string(am::kBackupDir) + "/" + local_time("%Y%m%d-%H%M%S");
    }
    const fs::path backup = root / fs::u8path(rep->backup_dir) / fs::u8path(side_rel);
    fs::create_directories(backup.parent_path(), ec);
    fs::copy_file(side, backup, fs::copy_options::overwrite_existing, ec);
    const fs::path dest = fs::u8path(meta_abs(asset));
    Meta have;
    if (fs::exists(dest, ec) && read_meta(dest.string(), &have)) {
      if (have.guid == sm.guid) {
        ++rep->duplicates;
        log("migrate: " + side_rel + " already in the metadata folder (GUID " + sm.guid + "), sidecar removed");
      } else {
        ++rep->conflicts;
        log("migrate: CONFLICT " + side_rel + " has GUID " + sm.guid + " but the metadata folder has " + have.guid +
            "; kept the folder's, sidecar saved in " + rep->backup_dir);
      }
      fs::remove(side, ec);
    } else if (move_file(side, dest)) {
      ++rep->moved;
      log("migrate: " + side_rel + " -> " + std::string(am::kMetaDir) + "/" + asset + kMetaExt + " (GUID " + sm.guid + ")");
    } else {
      log("migrate: FAILED to move " + side_rel + " (left in place)");
      continue;
    }
    rep->assets.push_back(asset);
  }
}

AssetDb::MigrationReport AssetDb::migrate() {
  MigrationReport rep;
  std::error_code ec;
  const fs::path root = fs::u8path(dir_);
  if (dir_.empty() || !fs::is_directory(root, ec)) return rep;
  std::vector<std::string> sidecars;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (ignored_dir(name) || it.depth() > 8) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec) || lower_ext(name) != kMetaExt) continue;
    const std::string rel = fs::relative(it->path(), root, ec).generic_string();
    if (kind_for(rel.substr(0, rel.size() - std::string(kMetaExt).size())) != Kind::Unknown) sidecars.push_back(rel);
  }
  std::sort(sidecars.begin(), sidecars.end());
  adopt_sidecars(sidecars, &rep);
  // Layout stamp: the claim's metadata now lives in the folder.
  const fs::path stamp = root / fs::u8path(am::kLayoutFile);
  std::string text;
  Value v;
  std::string err;
  const bool have = slurp(stamp, &text) && json::parse(text, &v, &err) && v.is_object() &&
                    v["meta_layout"].as_number(0) >= am::kLayoutVersion;
  if (!have || rep.any()) {
    if (!have) {
      v = Value::object();
      v.set("meta_layout", Value::number(am::kLayoutVersion));
      v.set("folder", Value::string(am::kMetaDir));
      v.set("since", Value::string(local_time("%Y-%m-%dT%H:%M:%S")));
    }
    if (rep.any()) {
      v.set("migrated", Value::number(v["migrated"].as_number(0) + rep.moved + rep.duplicates + rep.conflicts));
      v.set("last_backup", Value::string(rep.backup_dir));
    }
    fs::create_directories(stamp.parent_path(), ec);
    spit(stamp, json::write(v));
  }
  if (rep.any()) {
    log("migrate: " + std::to_string(rep.moved) + " moved, " + std::to_string(rep.duplicates) + " duplicate(s), " +
        std::to_string(rep.conflicts) + " conflict(s); backup " + rep.backup_dir);
  }
  return rep;
}

int AssetDb::clean_orphans(const std::vector<std::string>& rels, bool keep) {
  int n = 0;
  std::error_code ec;
  for (const std::string& r : rels) {
    const std::string rel = clean_rel(r);
    if (rel.empty() || rel.find("..") != std::string::npos || by_path(rel)) continue;
    const fs::path m = fs::u8path(meta_abs(rel));
    if (!fs::is_regular_file(m, ec)) continue;
    if (keep) {
      const fs::path to = fs::u8path(dir_) / fs::u8path(am::kOrphanDir) / fs::u8path(rel + kMetaExt);
      if (!move_file(m, to)) continue;
      log("orphan: " + rel + kMetaExt + " moved to " + am::kOrphanDir);
    } else {
      fs::remove(m, ec);
      if (ec) continue;
      log("orphan: " + rel + kMetaExt + " deleted");
    }
    prune_meta_dirs(rel);
    ++n;
  }
  return n;
}

std::string AssetDb::abs(const std::string& rel) const {
  return (fs::u8path(dir_) / fs::u8path(rel)).string();
}

void AssetDb::sort_entries() {
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) { return a.rel < b.rel; });
}

void AssetDb::import_entry(Entry* e) {
  e->import_ok = true;
  e->import_error.clear();
  Value info = Value::object();
  if (importer_) {
    std::string err;
    if (!importer_(abs(e->rel), e->meta.kind, e->meta.settings, &info, &err)) {
      e->import_ok = false;
      e->import_error = err.empty() ? "import failed" : err;
      info = Value::object();
      info.set("error", Value::string(e->import_error));
    }
  }
  e->meta.info = info;
}

AssetDb::ScanReport AssetDb::scan() {
  ScanReport rep;
  std::map<std::string, Entry> known;
  for (Entry& e : entries_) known.emplace(e.rel, std::move(e));
  std::vector<std::string> files;
  std::vector<std::string> sidecars;
  std::error_code ec;
  const fs::path root = fs::u8path(dir_);
  if (dir_.empty() || !fs::is_directory(root, ec)) {
    entries_.clear();
    return rep;
  }
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (ignored_dir(name) || it.depth() > 8) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    const std::string rel = fs::relative(it->path(), root, ec).generic_string();
    if (lower_ext(name) == kMetaExt) {
      if (kind_for(rel.substr(0, rel.size() - std::string(kMetaExt).size())) != Kind::Unknown) sidecars.push_back(rel);
    } else if (kind_for(name) != Kind::Unknown) {
      files.push_back(rel);
    }
  }
  if (!sidecars.empty()) {
    // A sidecar .meta (copied in from an older claim, or written by an old
    // build): into the metadata folder, GUID kept.
    std::sort(sidecars.begin(), sidecars.end());
    MigrationReport mr;
    adopt_sidecars(sidecars, &mr);
    rep.adopted = mr.assets;
  }
  std::sort(files.begin(), files.end());
  const std::set<std::string> file_set(files.begin(), files.end());
  // .meta files whose asset is gone: candidates for "moved outside the
  // editor"; then the cleaned-up orphans (a file that comes back).
  struct Orphan {
    std::string asset;     // asset rel the .meta was for
    fs::path file;         // the .meta on disk
    Meta meta;
    bool parked = false;   // in .tombstone/meta_orphans
    bool used = false;
  };
  std::vector<Orphan> orphans;
  auto collect = [&](const fs::path& base, bool parked) {
    if (!fs::is_directory(base, ec)) return;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec) || lower_ext(it->path().filename().string()) != kMetaExt) continue;
      const std::string mrel = fs::relative(it->path(), base, ec).generic_string();
      const std::string asset = mrel.substr(0, mrel.size() - std::string(kMetaExt).size());
      if ((!parked && file_set.count(asset)) || kind_for(asset) == Kind::Unknown) continue;
      Orphan o;
      o.asset = asset;
      o.file = it->path();
      o.parked = parked;
      if (read_meta(o.file.string(), &o.meta)) orphans.push_back(std::move(o));
    }
  };
  collect(fs::u8path(am::store_dir(dir_)), false);
  collect(root / fs::u8path(am::kOrphanDir), true);
  auto file_name = [](const std::string& rel) {
    const auto slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
  };

  std::vector<Entry> out;
  std::set<std::string> guids;
  std::set<std::string> moved_from;
  for (const std::string& rel : files) {
    Entry e;
    e.rel = rel;
    const std::string a = abs(rel);
    const std::string meta_file = meta_abs(rel);
    asset_stat(a, &e.mtime, &e.size);
    const Kind kind = kind_for(rel);
    const auto old = known.find(rel);
    const bool unchanged_stat =
        old != known.end() && old->second.mtime == e.mtime && old->second.size == e.size;
    Meta m;
    bool dirty = false, reimport = false;
    const bool have_meta = fs::exists(fs::u8path(meta_file), ec) && read_meta(meta_file, &m);
    if (have_meta) {
      if (guids.count(m.guid)) {
        m.guid = new_guid();  // a copy of another asset
        rep.duplicated.push_back(rel);
        dirty = true;
      } else if (!m.path.empty() && m.path != rel) {
        Meta other;
        if (file_set.count(m.path) && read_meta(meta_abs(m.path), &other) && other.guid == m.guid) {
          m.guid = new_guid();  // copied next to an original that still exists
          rep.duplicated.push_back(rel);
        } else {
          rep.moved.emplace_back(m.path, rel);
          moved_from.insert(m.path);
        }
        dirty = true;
      }
      const std::string h = unchanged_stat ? old->second.meta.hash : asset_hash(a);
      if (h != m.hash) {
        m.hash = h;
        reimport = dirty = true;
        if (!m.hash.empty() && old != known.end()) rep.changed.push_back(rel);
      }
      if (m.kind != kind) {
        m.kind = kind;
        dirty = reimport = true;
      }
      if (kind == Kind::Model && m.info.num("importer_version", 1) < kModelImporterVersion) {
        dirty = reimport = true;  // info from an older importer (bounds)
      }
    } else {
      const std::string h = asset_hash(a);
      auto usable = [&](const Orphan& o) { return !o.used && o.meta.kind == kind && !guids.count(o.meta.guid); };
      // 1) Same content (moved / renamed outside the editor).
      auto orphan = std::find_if(orphans.begin(), orphans.end(),
                                 [&](const Orphan& o) { return usable(o) && !o.meta.hash.empty() && o.meta.hash == h; });
      std::string how = "hash";
      if (orphan == orphans.end()) {
        // 2) Same file name + type, and only one such orphan (moved, then
        // edited before the editor saw it).
        const std::string fname = file_name(rel);
        int matches = 0;
        auto pick = orphans.end();
        for (auto it = orphans.begin(); it != orphans.end(); ++it) {
          if (usable(*it) && !it->parked && file_name(it->asset) == fname) {
            ++matches;
            pick = it;
          }
        }
        if (matches == 1) {
          orphan = pick;
          how = "name";
        }
      }
      if (orphan != orphans.end()) {
        orphan->used = true;
        m = orphan->meta;
        fs::remove(orphan->file, ec);
        if (!orphan->parked) prune_meta_dirs(orphan->asset);
        const std::string from = m.path.empty() ? orphan->asset : m.path;
        rep.moved.emplace_back(from, rel);
        rep.relinked.push_back({from, rel, how});
        moved_from.insert(from);
        log("relink: " + from + " -> " + rel + " (GUID " + m.guid + ", matched by " +
            (how == "hash" ? "content hash" : "file name") + (orphan->parked ? ", from meta_orphans" : "") + ")");
        if (m.hash != h) {
          m.hash = h;
          reimport = true;
        }
      } else {
        m.guid = new_guid();
        m.kind = kind;
        m.settings = default_settings(kind, rel);
        m.hash = h;
        reimport = true;
        rep.created.push_back(rel);
      }
      dirty = true;
    }
    if (m.path != rel) {
      m.path = rel;
      dirty = true;
    }
    e.meta = std::move(m);
    if (reimport) {
      import_entry(&e);
    } else if (old != known.end()) {
      e.import_ok = old->second.import_ok;
      e.import_error = old->second.import_error;
    } else {
      e.import_ok = !e.meta.info.has("error");
      e.import_error = e.meta.info.str("error");
    }
    if (dirty || reimport) {
      fs::create_directories(fs::u8path(meta_file).parent_path(), ec);
      write_meta(meta_file, e.meta);
    }
    guids.insert(e.meta.guid);
    out.push_back(std::move(e));
  }
  for (const auto& [rel, e] : known) {
    if (!file_set.count(rel) && !moved_from.count(rel)) rep.removed.push_back(rel);
  }
  for (const Orphan& o : orphans) {
    if (!o.used && !o.parked) rep.orphan_metas.push_back(o.asset);
  }
  std::sort(rep.orphan_metas.begin(), rep.orphan_metas.end());
  entries_ = std::move(out);
  sort_entries();
  return rep;
}

std::size_t AssetDb::fix_references(const ScanReport& report) {
  std::size_t n = 0;
  for (const auto& [from, to] : report.moved) n += rewrite_references(from, to);
  return n;
}

const AssetDb::Entry* AssetDb::by_path(const std::string& rel) const {
  const std::string r = clean_rel(rel);
  for (const Entry& e : entries_) {
    if (e.rel == r) return &e;
  }
  return nullptr;
}

const AssetDb::Entry* AssetDb::by_guid(const std::string& guid) const {
  for (const Entry& e : entries_) {
    if (e.meta.guid == guid) return &e;
  }
  return nullptr;
}

std::string AssetDb::path_of(const std::string& guid) const {
  const Entry* e = by_guid(guid);
  return e ? e->rel : std::string();
}

std::vector<std::string> AssetDb::folders() const {
  std::set<std::string> out;
  auto add_with_parents = [&](std::string d) {
    while (!d.empty()) {
      out.insert(d);
      const auto slash = d.find_last_of('/');
      if (slash == std::string::npos) break;
      d.resize(slash);
    }
  };
  for (const Entry& e : entries_) {
    const auto slash = e.rel.find_last_of('/');
    if (slash != std::string::npos) add_with_parents(e.rel.substr(0, slash));
  }
  for (const std::string& f : extra_folders_) add_with_parents(f);
  std::error_code ec;
  const fs::path assets = fs::u8path(dir_) / "assets";
  if (fs::is_directory(assets, ec)) {
    out.insert("assets");
    for (fs::recursive_directory_iterator it(assets, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec)) {
        out.insert(fs::relative(it->path(), fs::u8path(dir_), ec).generic_string());
      }
    }
  }
  return {out.begin(), out.end()};
}

bool AssetDb::set_settings(const std::string& rel, const Value& settings, std::string* error) {
  for (Entry& e : entries_) {
    if (e.rel != clean_rel(rel)) continue;
    e.meta.settings = settings;
    import_entry(&e);
    std::error_code ec;
    fs::create_directories(fs::u8path(meta_abs(e.rel)).parent_path(), ec);
    return write_meta(meta_abs(e.rel), e.meta, error);
  }
  if (error) *error = "unknown asset " + rel;
  return false;
}

bool AssetDb::reimport(const std::string& rel) {
  for (Entry& e : entries_) {
    if (e.rel != clean_rel(rel)) continue;
    e.meta.hash = asset_hash(abs(e.rel));
    import_entry(&e);
    std::error_code ec;
    fs::create_directories(fs::u8path(meta_abs(e.rel)).parent_path(), ec);
    write_meta(meta_abs(e.rel), e.meta);
    return e.import_ok;
  }
  return false;
}

std::vector<Ref> AssetDb::find_references(const std::string& rel_in) const {
  const std::string rel = clean_rel(rel_in);
  std::vector<Ref> out;
  std::error_code ec;
  const fs::path root = fs::u8path(dir_);
  const std::string dq = "\"" + rel + "\"", sq = "'" + rel + "'";
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (ignored_dir(name)) it.disable_recursion_pending();
      continue;
    }
    if (!ref_file(name)) continue;
    std::string text;
    if (!slurp(it->path(), &text)) continue;
    const std::size_t n = count_of(text, dq) + (lower_ext(name) == ".lua" ? count_of(text, sq) : 0);
    if (n > 0) {
      out.push_back({fs::relative(it->path(), root, ec).generic_string(), static_cast<int>(n)});
    }
  }
  std::sort(out.begin(), out.end(), [](const Ref& a, const Ref& b) { return a.file < b.file; });
  return out;
}

std::size_t AssetDb::rewrite_references(const std::string& old_in, const std::string& new_in) const {
  const std::string from = clean_rel(old_in), to = clean_rel(new_in);
  if (from == to || from.empty()) return 0;
  std::size_t total = 0;
  for (const Ref& r : find_references(from)) {
    const fs::path p = fs::u8path(abs(r.file));
    std::string text;
    if (!slurp(p, &text)) continue;
    std::size_t n = replace_all(text, "\"" + from + "\"", "\"" + to + "\"");
    if (lower_ext(r.file) == ".lua") n += replace_all(text, "'" + from + "'", "'" + to + "'");
    if (n > 0 && spit(p, text)) total += n;
  }
  return total;
}

bool AssetDb::move(const std::string& old_in, const std::string& new_in, std::string* error,
                   std::size_t* refs_rewritten) {
  const std::string from = clean_rel(old_in), to = clean_rel(new_in);
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (from == to) return fail("same path");
  if (to.empty() || to.find("..") != std::string::npos) return fail("bad destination");
  if (kind_for(to) != kind_for(from)) return fail("keep the file extension");
  Entry* e = nullptr;
  for (Entry& x : entries_) {
    if (x.rel == from) e = &x;
  }
  if (!e) return fail("unknown asset " + from);
  std::error_code ec;
  const fs::path src = fs::u8path(abs(from)), dst = fs::u8path(abs(to));
  if (fs::exists(dst, ec)) return fail(to + " already exists");
  const auto siblings = mdl_siblings(abs(from));
  fs::path dst_base = dst;
  dst_base.replace_extension();
  for (const auto& [p, suffix] : siblings) {
    if (fs::exists(fs::u8path(dst_base.string() + suffix), ec)) return fail(dst_base.filename().string() + suffix + " already exists");
  }
  fs::create_directories(dst.parent_path(), ec);
  fs::rename(src, dst, ec);
  if (ec) return fail("rename failed: " + ec.message());
  for (const auto& [p, suffix] : siblings) fs::rename(p, fs::u8path(dst_base.string() + suffix), ec);
  // The .meta follows in the metadata folder (and a stray sidecar goes).
  move_file(fs::u8path(meta_abs(from)), fs::u8path(meta_abs(to)));
  fs::remove(fs::u8path(abs(from) + kMetaExt), ec);
  prune_meta_dirs(from);
  e->rel = to;
  e->meta.path = to;
  e->mtime = fs::last_write_time(dst, ec);
  fs::create_directories(fs::u8path(meta_abs(to)).parent_path(), ec);
  write_meta(meta_abs(to), e->meta);
  const std::size_t n = rewrite_references(from, to);
  if (refs_rewritten) *refs_rewritten = n;
  sort_entries();
  return true;
}

bool AssetDb::remove(const std::string& rel_in, bool force, std::vector<Ref>* refs_out,
                     std::string* error) {
  const std::string rel = clean_rel(rel_in);
  std::vector<Ref> refs = find_references(rel);
  if (refs_out) *refs_out = refs;
  if (!refs.empty() && !force) {
    if (error) *error = rel + " is used by " + std::to_string(refs.size()) + " file(s)";
    return false;
  }
  std::error_code ec;
  const auto siblings = mdl_siblings(abs(rel));
  if (!fs::remove(fs::u8path(abs(rel)), ec) || ec) {
    if (error) *error = "cannot delete " + rel;
    return false;
  }
  for (const auto& [p, suffix] : siblings) fs::remove(p, ec);
  fs::remove(fs::u8path(meta_abs(rel)), ec);
  fs::remove(fs::u8path(abs(rel) + kMetaExt), ec);
  prune_meta_dirs(rel);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) { return e.rel == rel; }),
                 entries_.end());
  return true;
}

bool AssetDb::make_folder(const std::string& rel_in, std::string* error) {
  const std::string rel = clean_rel(rel_in);
  std::error_code ec;
  if (rel.empty() || rel.find("..") != std::string::npos ||
      (!fs::create_directories(fs::u8path(abs(rel)), ec) && ec)) {
    if (error) *error = "cannot create folder " + rel;
    return false;
  }
  extra_folders_.push_back(rel);
  return true;
}

}  // namespace assetdb
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
