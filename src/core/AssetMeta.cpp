#include "core/AssetMeta.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace ts {
namespace tombstone {
namespace core {
namespace asset_meta {

namespace fs = std::filesystem;

namespace {

std::string clean(std::string rel) {
  std::replace(rel.begin(), rel.end(), '\\', '/');
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  while (rel.rfind("./", 0) == 0) rel.erase(0, 2);
  return rel;
}

bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

}  // namespace

std::string store_dir(const std::string& claim_dir) {
  return (fs::u8path(claim_dir) / fs::u8path(kMetaDir)).string();
}

std::string meta_path(const std::string& claim_dir, const std::string& asset_rel) {
  return (fs::u8path(claim_dir) / fs::u8path(kMetaDir) / fs::u8path(clean(asset_rel) + kMetaExt)).string();
}

std::string sidecar_path(const std::string& claim_dir, const std::string& asset_rel) {
  return (fs::u8path(claim_dir) / fs::u8path(clean(asset_rel) + kMetaExt)).string();
}

bool is_meta_rel(const std::string& rel) {
  if (rel.size() <= 5) return false;
  std::string tail = rel.substr(rel.size() - 5);
  std::transform(tail.begin(), tail.end(), tail.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return tail == kMetaExt;
}

std::string asset_of(const std::string& meta_rel) {
  std::string r = clean(meta_rel);
  const std::string store = std::string(kMetaDir) + "/";
  if (r.rfind(store, 0) == 0) r = r.substr(store.size());
  return is_meta_rel(r) ? r.substr(0, r.size() - 5) : r;
}

std::string find_for_asset(const std::string& claim_dir, const std::string& asset_rel) {
  const std::string m = meta_path(claim_dir, asset_rel);
  if (is_file(fs::u8path(m))) return m;
  const std::string s = sidecar_path(claim_dir, asset_rel);
  if (is_file(fs::u8path(s))) return s;
  return {};
}

std::string resolve(const std::string& claim_dir, const std::string& logical_meta_rel) {
  return find_for_asset(claim_dir, asset_of(logical_meta_rel));
}

std::string find_for_file(const std::string& abs_asset_path) {
  std::error_code ec;
  const fs::path file = fs::u8path(abs_asset_path);
  fs::path abs = fs::absolute(file, ec);
  if (ec) abs = file;
  abs = abs.lexically_normal();
  fs::path dir = abs.parent_path();
  for (int depth = 0; depth < 16 && !dir.empty(); ++depth) {
    const fs::path store = dir / fs::u8path(kMetaDir);
    if (fs::is_directory(store, ec)) {
      const fs::path rel = abs.lexically_relative(dir);
      const fs::path m = store / fs::u8path(rel.generic_string() + kMetaExt);
      if (is_file(m)) return m.string();
      break;  // the claim root: its mirror does not hold this file
    }
    const fs::path up = dir.parent_path();
    if (up == dir) break;
    dir = up;
  }
  const fs::path side = fs::u8path(abs_asset_path + kMetaExt);
  if (is_file(side)) return side.string();
  return {};
}

}  // namespace asset_meta
}  // namespace core
}  // namespace tombstone
}  // namespace ts
