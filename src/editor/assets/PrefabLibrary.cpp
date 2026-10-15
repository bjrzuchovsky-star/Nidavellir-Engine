#include "editor/assets/PrefabLibrary.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

void PrefabLibrary::set_project(const std::string& project_dir) {
  if (project_dir != dir_) clear();
  dir_ = project_dir;
}

std::string PrefabLibrary::abs_path(const std::string& rel) const {
  return (fs::u8path(dir_) / fs::u8path(rel)).string();
}

bool PrefabLibrary::load(const std::string& rel, Entry* e) {
  std::error_code ec;
  e->mtime = fs::last_write_time(fs::u8path(abs_path(rel)), ec);
  std::string err;
  e->ok = prefab::load_asset(abs_path(rel), &e->asset, &err);
  return e->ok;
}

const prefab::Asset* PrefabLibrary::get(const std::string& rel) {
  auto it = cache_.find(rel);
  if (it == cache_.end()) {
    Entry e;
    load(rel, &e);
    it = cache_.emplace(rel, std::move(e)).first;
  }
  return it->second.ok ? &it->second.asset : nullptr;
}

prefab::AssetLookup PrefabLibrary::lookup() {
  return [this](const std::string& rel) { return get(rel); };
}

bool PrefabLibrary::write_text(const std::string& rel, const std::string& text) {
  std::error_code ec;
  const fs::path p = fs::u8path(abs_path(rel));
  if (text.empty()) {
    fs::remove(p, ec);
  } else {
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
  }
  cache_.erase(rel);
  get(rel);
  return true;
}

bool PrefabLibrary::save(const std::string& rel, const prefab::Asset& asset,
                         std::string* error, std::size_t undo_depth,
                         const std::string& label) {
  std::string before;
  {
    std::ifstream in(fs::u8path(abs_path(rel)), std::ios::binary);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      before = ss.str();
    }
  }
  if (!prefab::save_asset(abs_path(rel), asset, error)) return false;
  Entry e;
  load(rel, &e);
  cache_[rel] = std::move(e);
  if (undo_depth > 0) {
    undo_.push_back({rel, before, prefab::write_asset(asset), undo_depth, label});
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
  }
  return true;
}

std::vector<std::string> PrefabLibrary::poll_changes() {
  std::vector<std::string> out;
  for (auto& [rel, e] : cache_) {
    std::error_code ec;
    const auto t = fs::last_write_time(fs::u8path(abs_path(rel)), ec);
    if (ec || t == e.mtime) continue;
    Entry fresh;
    if (load(rel, &fresh)) {
      e = std::move(fresh);
      out.push_back(rel);
    } else {
      e.mtime = t;  // broken mid-save; keep the last good asset
    }
  }
  return out;
}

std::vector<std::string> PrefabLibrary::list() const {
  return prefab::scan_assets(dir_);
}

bool PrefabLibrary::on_undo(std::size_t depth, const std::string& label) {
  bool any = false;
  while (!undo_.empty() && undo_.back().depth == depth && undo_.back().label == label) {
    Edit ed = std::move(undo_.back());
    undo_.pop_back();
    write_text(ed.rel, ed.before);
    redo_.push_back(std::move(ed));
    any = true;
  }
  return any;
}

bool PrefabLibrary::on_redo(std::size_t depth, const std::string& label) {
  bool any = false;
  while (!redo_.empty() && redo_.back().depth == depth && redo_.back().label == label) {
    Edit ed = std::move(redo_.back());
    redo_.pop_back();
    write_text(ed.rel, ed.after);
    undo_.push_back(std::move(ed));
    any = true;
  }
  return any;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
