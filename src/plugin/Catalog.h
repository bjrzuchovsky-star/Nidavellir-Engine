#pragma once

// Every plugin the host knows about (the "wagon"): statically linked
// built-ins plus dynamic libraries loaded from a folder. Enabling and
// load order are the PluginManager's business; the catalog only lists.
//
// Static plugins register through an explicit link call instead of a
// global constructor, so a static library's linker never drops them and
// there is no static-init order to worry about (works the same on MSVC,
// GCC and Clang):
//
//   // tumbleweed_plugin.cpp (global namespace)
//   TS_PLUGIN_STATIC(tumbleweed, TumbleweedPlugin)
//
//   // wherever the host fills its catalog (global namespace declarations)
//   TS_PLUGIN_DECLARE(tumbleweed);
//   void fill(Catalog& c) { TS_PLUGIN_LINK(tumbleweed, c); }
//
// A catalog must outlive every PluginManager using it (it owns the
// dynamic libraries their code lives in).

#include "plugin/Plugin.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugin {

class DynamicLibrary;

struct CatalogEntry {
  PluginInfo info;
  std::string source;  // "built-in" or the library path
  bool dynamic = false;
  std::function<Plugin*()> create;
  std::function<void(Plugin*)> destroy;
};

class Catalog {
 public:
  Catalog();
  ~Catalog();
  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;

  using StaticFactory = std::function<std::unique_ptr<Plugin>()>;
  // Built-in. False (with a reason) for a bad id or a duplicate.
  bool add_static(StaticFactory make, std::string* error = nullptr);
  // One .dll / .so with a TS_DYNAMIC_PLUGIN entry point.
  bool load_library(const std::string& path, std::string* error = nullptr);
  // Every native library in `dir` (not recursive), sorted by name. Returns
  // how many loaded; problems are appended to *errors. A missing folder is
  // not an error.
  int scan_folder(const std::string& dir, std::vector<std::string>* errors);

  const std::vector<CatalogEntry>& entries() const { return entries_; }
  const CatalogEntry* find(const std::string& id) const;
  std::size_t index_of(const std::string& id) const;  // npos when absent

  // Lowercase letters, digits, '.', '_', '-'; 1..64 chars; starts with a
  // letter.
  static bool valid_id(const std::string& id);

 private:
  bool add_entry(CatalogEntry entry, std::string* error);

  std::vector<CatalogEntry> entries_;
  std::vector<std::unique_ptr<DynamicLibrary>> libs_;
};

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts

// Static registration (use at global namespace scope).
#define TS_PLUGIN_STATIC(SYMBOL, CLASS)                                       \
  bool ts_plugin_link_##SYMBOL(::ts::tombstone::plugin::Catalog& catalog,     \
                               std::string* error) {                          \
    return catalog.add_static(                                                \
        []() -> std::unique_ptr<::ts::tombstone::plugin::Plugin> {            \
          return std::make_unique<CLASS>();                                   \
        },                                                                    \
        error);                                                               \
  }
#define TS_PLUGIN_DECLARE(SYMBOL)                                         \
  bool ts_plugin_link_##SYMBOL(::ts::tombstone::plugin::Catalog& catalog, \
                               std::string* error)
#define TS_PLUGIN_LINK(SYMBOL, CATALOG, ERROR) \
  ::ts_plugin_link_##SYMBOL((CATALOG), (ERROR))
