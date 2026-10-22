#include "vault/VaultLock.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace ts {
namespace tombstone {
namespace vault {

namespace fs = std::filesystem;
using json::Value;

namespace {

bool slurp(const fs::path& p, std::string* out, std::uintmax_t cap = 64u << 20) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  if (ec || n > cap) return false;
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

std::string lower_ext(const std::string& rel) {
  std::string e = fs::path(rel).extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

// Files whose text can name other files (scenes, prefabs, scripts, UI,
// glTF, material libraries...). Binary assets never reference by path.
bool referencing_file(const std::string& rel) {
  static const char* kBinary[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd", ".wav", ".ogg",
                                  ".mp3", ".flac", ".ttf", ".otf", ".ttc", ".glb", ".bin", ".mdl", ".vvd",
                                  ".vtx", ".phy", ".vtf", ".fbx", ".blend", ".3ds", ".ply", ".stl", ".dds",
                                  ".ktx", ".ktx2", ".meta", ".tspak", ".zip"};
  const std::string e = lower_ext(rel);
  for (const char* b : kBinary) {
    if (e == b) return false;
  }
  return true;
}

}  // namespace

std::string ProjectLock::lock_path(const std::string& project_dir) {
  return (fs::u8path(project_dir) / ".vault" / "lock.json").string();
}

bool ProjectLock::load(const std::string& project_dir, std::string* error) {
  dir_ = project_dir;
  entries_.clear();
  std::string text;
  const fs::path p = fs::u8path(lock_path(project_dir));
  std::error_code ec;
  if (!fs::exists(p, ec)) return true;
  if (!slurp(p, &text)) {
    if (error) *error = "cannot read " + p.string();
    return false;
  }
  Value root;
  std::string err;
  if (!json::parse(text, &root, &err)) {
    if (error) *error = p.string() + ": " + err;
    return false;
  }
  for (const Value& j : root["items"].items()) {
    LockEntry e;
    e.item = j.str("item");
    if (e.item.empty()) continue;
    e.name = j.str("name");
    e.kind = j.str("kind");
    e.origin = j.str("origin", "pulled");
    e.library = j.str("library");
    e.version = static_cast<int>(j.num("version", 0));
    e.pinned = j.flag("pinned", false);
    e.main = j.str("main");
    e.guid = j.str("guid");
    e.deps = json_strings(j["deps"]);
    e.when = j.str("when");
    for (const Value& f : j["files"].items()) {
      LockFile lf{f.str("path"), f.str("sha256"), f.str("guid")};
      if (!lf.path.empty()) e.files.push_back(lf);
    }
    entries_.push_back(std::move(e));
  }
  return true;
}

bool ProjectLock::save(std::string* error) const {
  Value root = Value::object();
  root.set("vault_lock", Value::number(1));
  Value items = Value::array();
  for (const LockEntry& e : entries_) {
    Value j = Value::object();
    j.set("item", Value::string(e.item));
    j.set("name", Value::string(e.name));
    j.set("kind", Value::string(e.kind));
    j.set("origin", Value::string(e.origin));
    j.set("library", Value::string(e.library));
    j.set("version", Value::number(e.version));
    j.set("pinned", Value::boolean(e.pinned));
    j.set("main", Value::string(e.main));
    j.set("guid", Value::string(e.guid));
    j.set("deps", strings_json(e.deps));
    j.set("when", Value::string(e.when));
    Value files = Value::array();
    for (const LockFile& f : e.files) {
      Value x = Value::object();
      x.set("path", Value::string(f.path));
      x.set("sha256", Value::string(f.sha256));
      if (!f.guid.empty()) x.set("guid", Value::string(f.guid));
      files.push(x);
    }
    j.set("files", files);
    items.push(j);
  }
  root.set("items", items);
  const fs::path p = fs::u8path(lock_path(dir_));
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json::write(root);
    if (!out) {
      if (error) *error = "cannot write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
  }
  if (ec && error) *error = "cannot write " + p.string() + ": " + ec.message();
  return !ec;
}

LockEntry* ProjectLock::find(const std::string& item) {
  for (LockEntry& e : entries_) {
    if (e.item == item) return &e;
  }
  return nullptr;
}

const LockEntry* ProjectLock::find(const std::string& item) const {
  return const_cast<ProjectLock*>(this)->find(item);
}

const LockEntry* ProjectLock::find_by_guid(const std::string& guid) const {
  if (guid.empty()) return nullptr;
  for (const LockEntry& e : entries_) {
    if (e.guid == guid) return &e;
  }
  return nullptr;
}

const LockEntry* ProjectLock::find_by_path(const std::string& rel) const {
  for (const LockEntry& e : entries_) {
    if (e.main == rel) return &e;
    for (const LockFile& f : e.files) {
      if (f.path == rel) return &e;
    }
  }
  return nullptr;
}

LockEntry& ProjectLock::upsert(const LockEntry& e) {
  if (LockEntry* cur = find(e.item)) {
    *cur = e;
    return *cur;
  }
  entries_.push_back(e);
  return entries_.back();
}

void ProjectLock::remove(const std::string& item) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const LockEntry& e) { return e.item == item; }),
                 entries_.end());
}

std::set<std::string> unused_pulled_files(const std::string& project_dir,
                                          const std::vector<std::string>& content) {
  std::set<std::string> out;
  ProjectLock lock;
  if (!lock.load(project_dir) || lock.entries().empty()) return out;
  const fs::path root = fs::u8path(project_dir);
  // Where each GUID lives now (assets may have moved since the pull).
  std::map<std::string, std::string> guid_path;
  for (const std::string& rel : content) {
    if (lower_ext(rel) != ".meta") continue;
    std::string text;
    if (!slurp(root / fs::u8path(rel), &text, 4u << 20)) continue;
    Value m;
    std::string err;
    if (json::parse(text, &m, &err) && !m.str("guid").empty()) {
      guid_path[m.str("guid")] = rel.substr(0, rel.size() - 5);
    }
  }
  const std::set<std::string> present(content.begin(), content.end());
  // Pulled items and their current files.
  struct Unit {
    const LockEntry* e;
    std::vector<std::string> files;
    bool used = false;
  };
  std::vector<Unit> units;
  std::map<std::string, std::size_t> unit_of_item;
  std::set<std::string> pulled_files;
  for (const LockEntry& e : lock.entries()) {
    if (e.origin != "pulled") continue;
    Unit u{&e, {}, false};
    // Files that sat next to the main asset move with it.
    std::string main_now = e.main;
    if (!e.guid.empty() && guid_path.count(e.guid)) main_now = guid_path[e.guid];
    const std::string old_dir = fs::path(e.main).parent_path().generic_string();
    const std::string new_dir = fs::path(main_now).parent_path().generic_string();
    for (const LockFile& f : e.files) {
      std::string p = f.path;
      if (!f.guid.empty() && guid_path.count(f.guid)) {
        p = guid_path[f.guid] + (f.path.size() > 5 && f.path.compare(f.path.size() - 5, 5, ".meta") == 0 ? ".meta" : "");
      } else if (old_dir != new_dir && p.rfind(old_dir + "/", 0) == 0) {
        p = new_dir + p.substr(old_dir.size());
      }
      if (present.count(p)) {
        u.files.push_back(p);
        pulled_files.insert(p);
      }
    }
    unit_of_item[e.item] = units.size();
    units.push_back(std::move(u));
  }
  if (units.empty()) return out;
  // Text of everything that is not a pulled file: the game's own content.
  std::string roots;
  for (const std::string& rel : content) {
    if (pulled_files.count(rel) || !referencing_file(rel)) continue;
    std::string text;
    if (slurp(root / fs::u8path(rel), &text, 16u << 20)) {
      roots += text;
      roots += '\n';
    }
  }
  auto named_in = [](const std::string& text, const Unit& u) {
    for (const std::string& f : u.files) {
      if (f.size() > 5 && f.compare(f.size() - 5, 5, ".meta") == 0) continue;
      if (text.find(f) != std::string::npos) return true;
    }
    return false;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (Unit& u : units) {
      if (!u.used && named_in(roots, u)) {
        u.used = true;
        changed = true;
      }
    }
    for (Unit& u : units) {
      if (!u.used) continue;
      for (const std::string& d : u.e->deps) {
        auto it = unit_of_item.find(d);
        if (it != unit_of_item.end() && !units[it->second].used) {
          units[it->second].used = true;
          changed = true;
        }
      }
    }
    if (changed) {
      // A used pulled prefab / glTF can name further pulled files.
      for (Unit& u : units) {
        if (!u.used) continue;
        for (const std::string& f : u.files) {
          if (!referencing_file(f)) continue;
          std::string text;
          if (slurp(root / fs::u8path(f), &text, 16u << 20) && roots.find(text) == std::string::npos) {
            roots += text;
            roots += '\n';
          }
        }
      }
    }
  }
  for (const Unit& u : units) {
    if (u.used) continue;
    for (const std::string& f : u.files) out.insert(f);
  }
  return out;
}

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
