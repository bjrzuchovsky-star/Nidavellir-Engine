#include "plugin/Catalog.h"

#include "plugin/DynamicLibrary.h"
#include "plugin/PluginAbi.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {
namespace plugin {

Catalog::Catalog() = default;

Catalog::~Catalog() {
  // Entries hold function objects whose code may live in a library.
  entries_.clear();
  libs_.clear();
}

bool Catalog::valid_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  if (!(id[0] >= 'a' && id[0] <= 'z')) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

const CatalogEntry* Catalog::find(const std::string& id) const {
  for (const CatalogEntry& e : entries_) {
    if (e.info.id == id) return &e;
  }
  return nullptr;
}

std::size_t Catalog::index_of(const std::string& id) const {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].info.id == id) return i;
  }
  return std::numeric_limits<std::size_t>::max();
}

bool Catalog::add_entry(CatalogEntry entry, std::string* error) {
  if (!valid_id(entry.info.id)) {
    if (error) {
      *error = "plugin id '" + entry.info.id +
               "' is not valid (lowercase letters, digits, . _ -)";
    }
    return false;
  }
  if (const CatalogEntry* dup = find(entry.info.id)) {
    if (error) {
      *error = "two plugins claim the id '" + entry.info.id + "' (" +
               dup->source + " and " + entry.source + ")";
    }
    return false;
  }
  for (const Dependency& d : entry.info.deps) {
    if (d.id == entry.info.id) {
      if (error) *error = "plugin '" + entry.info.id + "' depends on itself";
      return false;
    }
  }
  if (entry.info.name.empty()) entry.info.name = entry.info.id;
  entries_.push_back(std::move(entry));
  return true;
}

bool Catalog::add_static(StaticFactory make, std::string* error) {
  if (!make) {
    if (error) *error = "empty plugin factory";
    return false;
  }
  std::unique_ptr<Plugin> probe;
  try {
    probe = make();
  } catch (const std::exception& e) {
    if (error) *error = std::string("plugin constructor threw: ") + e.what();
    return false;
  }
  if (!probe) {
    if (error) *error = "plugin factory returned nothing";
    return false;
  }
  CatalogEntry entry;
  entry.info = probe->info();
  entry.source = "built-in";
  entry.create = [make]() -> Plugin* { return make().release(); };
  entry.destroy = [](Plugin* p) { delete p; };
  return add_entry(std::move(entry), error);
}

bool Catalog::load_library(const std::string& path, std::string* error) {
  auto lib = std::make_unique<DynamicLibrary>();
  std::string why;
  if (!lib->open(path, &why)) {
    if (error) *error = "could not open " + path + ": " + why;
    return false;
  }
  void* sym = lib->symbol(TS_PLUGIN_ENTRY_NAME, &why);
  if (!sym) {
    if (error) {
      *error = path + " is not a TombStone plugin (no " TS_PLUGIN_ENTRY_NAME
               " entry point)";
    }
    return false;
  }
  TsPluginEntryFn fn = nullptr;
  static_assert(sizeof(fn) == sizeof(sym), "function pointer size");
  std::memcpy(&fn, &sym, sizeof(fn));
  const TsPluginEntry* entry = fn ? fn() : nullptr;
  if (!entry) {
    if (error) *error = path + ": entry point returned nothing";
    return false;
  }
  if (entry->abi_version != TS_PLUGIN_ABI_VERSION ||
      entry->struct_size < sizeof(TsPluginEntry)) {
    if (error) {
      *error = path + ": plugin ABI " + std::to_string(entry->abi_version) +
               ", engine speaks " + std::to_string(TS_PLUGIN_ABI_VERSION);
    }
    return false;
  }
  if (!entry->api_tag || std::strcmp(entry->api_tag, TS_PLUGIN_API_TAG) != 0) {
    if (error) {
      *error = path + ": built for " +
               std::string(entry->api_tag ? entry->api_tag : "?") +
               ", engine is " TS_PLUGIN_API_TAG " (rebuild the plugin)";
    }
    return false;
  }
  if (!entry->cxx_tag || std::strcmp(entry->cxx_tag, TS_PLUGIN_CXX_TAG) != 0) {
    if (error) {
      *error = path + ": built with C++ runtime " +
               std::string(entry->cxx_tag ? entry->cxx_tag : "?") +
               ", engine uses " TS_PLUGIN_CXX_TAG;
    }
    return false;
  }
  if (!entry->create || !entry->destroy) {
    if (error) *error = path + ": entry point has no create/destroy";
    return false;
  }
  auto* create = entry->create;
  auto* destroy = entry->destroy;
  auto* probe = static_cast<Plugin*>(create());
  if (!probe) {
    if (error) *error = path + ": create() failed";
    return false;
  }
  CatalogEntry ce;
  ce.info = probe->info();
  destroy(probe);
  ce.source = path;
  ce.dynamic = true;
  ce.create = [create]() -> Plugin* { return static_cast<Plugin*>(create()); };
  ce.destroy = [destroy](Plugin* p) { destroy(p); };
  if (!add_entry(std::move(ce), error)) {
    return false;
  }
  libs_.push_back(std::move(lib));
  return true;
}

int Catalog::scan_folder(const std::string& dir,
                         std::vector<std::string>* errors) {
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return 0;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    if (e.path().extension() == DynamicLibrary::native_extension()) {
      files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());
  int loaded = 0;
  for (const fs::path& p : files) {
    std::string err;
    if (load_library(p.string(), &err)) {
      ++loaded;
    } else if (errors) {
      errors->push_back(err);
    }
  }
  return loaded;
}

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts
