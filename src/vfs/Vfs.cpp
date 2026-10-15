#include "vfs/Vfs.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace vfs {

namespace fs = std::filesystem;

bool Vfs::mount_pak(const std::string& pak_path, std::string* error) {
  auto r = std::make_unique<PakReader>();
  if (!r->open(pak_path, error)) return false;
  paks_.push_back(std::move(r));
  return true;
}

void Vfs::add_loose_root(const std::string& dir) { loose_.push_back(dir); }

bool Vfs::exists(const std::string& rel) const {
  const std::string key = normalize_rel(rel);
  if (key.empty()) return false;
  for (auto it = paks_.rbegin(); it != paks_.rend(); ++it) {
    if ((*it)->find(key)) return true;
  }
  std::error_code ec;
  for (const std::string& root : loose_) {
    if (fs::is_regular_file(fs::path(root) / fs::path(key), ec)) return true;
  }
  return false;
}

bool Vfs::read(const std::string& rel, std::string* out, std::string* error) const {
  const std::string key = normalize_rel(rel);
  for (auto it = paks_.rbegin(); it != paks_.rend(); ++it) {
    if ((*it)->find(key)) return (*it)->read(key, out, error);
  }
  for (const std::string& root : loose_) {
    std::ifstream in(fs::path(root) / fs::path(key), std::ios::binary);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      *out = ss.str();
      return true;
    }
  }
  if (error) *error = rel + ": not found (no pak entry, no loose file)";
  return false;
}

std::vector<std::string> Vfs::list(const std::string& prefix) const {
  std::set<std::string> all;
  const std::string p = normalize_rel(prefix);
  auto take = [&](const std::string& s) {
    if (p.empty() || s.rfind(p + "/", 0) == 0 || s == p) all.insert(s);
  };
  for (const auto& pak : paks_) {
    for (const PakEntry& e : pak->entries()) take(e.path);
  }
  std::error_code ec;
  for (const std::string& root : loose_) {
    if (!fs::is_directory(root, ec)) continue;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec)) {
        take(fs::relative(it->path(), root, ec).generic_string());
      }
    }
  }
  return {all.begin(), all.end()};
}

std::string default_cache_root() {
  if (const char* e = std::getenv("TS_PAK_CACHE"); e && *e) return e;
#if defined(_WIN32)
  if (const char* e = std::getenv("LOCALAPPDATA"); e && *e) {
    return (fs::path(e) / "TombStone" / "paks").string();
  }
#else
  if (const char* e = std::getenv("XDG_CACHE_HOME"); e && *e) {
    return (fs::path(e) / "tombstone" / "paks").string();
  }
  if (const char* e = std::getenv("HOME"); e && *e) {
    return (fs::path(e) / ".cache" / "tombstone" / "paks").string();
  }
#endif
  std::error_code ec;
  return (fs::temp_directory_path(ec) / "tombstone-paks").string();
}

bool mount_project_pak(const std::string& pak_path, const std::string& cache_root,
                       MountResult* out, std::string* error) {
  PakReader pak;
  if (!pak.open(pak_path, error)) return false;
  const std::string hex = hex64(pak.content_hash());
  const fs::path dir = fs::path(cache_root) / hex;
  const fs::path marker = dir / ".tspak-mounted";
  out->hash = pak.content_hash();
  out->files = pak.entries().size();
  out->dir = dir.string();
  std::error_code ec;
  {
    std::ifstream m(marker);
    std::string got;
    if (m && std::getline(m, got) && got == hex) {
      out->reused = true;
      return true;
    }
  }
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) {
    if (error) *error = "cannot create pak cache " + dir.string() + ": " + ec.message();
    return false;
  }
  for (const PakEntry& e : pak.entries()) {
    std::string bytes;
    if (!pak.read(e.path, &bytes, error)) return false;
    const fs::path dst = dir / fs::path(e.path);
    fs::create_directories(dst.parent_path(), ec);
    std::ofstream o(dst, std::ios::binary | std::ios::trunc);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!o) {
      if (error) *error = "cannot write " + dst.string();
      return false;
    }
  }
  std::ofstream(marker) << hex << '\n';
  return true;
}

}  // namespace vfs
}  // namespace tombstone
}  // namespace ts
