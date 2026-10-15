#pragma once

// The Trading Post: TombStone's plugin store. One catalog over four kinds
// of goods:
//
//   built-in     plugins compiled into the engine (always "installed";
//                a project still has to enable them to ship them)
//   installed    packages in the engine plugin cache (see below)
//   local        plugin libraries in folders the user pointed at
//   remote       entries from a remote index (JSON over file:// / http(s)://)
//   coming soon  announced plugins with nothing to install yet
//
// The engine plugin cache (default ./TombStonePlugins, $TS_PLUGIN_CACHE to
// move it) holds one folder per installed plugin:
//
//   <cache>/trading_post.json      sources: local folders, index URLs,
//                                  trusted publisher keys, allow_unsigned
//   <cache>/<id>/installed.json    what was installed, from where, its hash
//   <cache>/<id>/<library>         the plugin's .dll / .so
//
// plugins::plugin_search_dirs() scans those folders, so an installed plugin
// shows up in every project's Outfitter's Wagon and is enabled per project
// (project.json "plugins"); export ships only the enabled ones.
//
// Remote index (index.json) + detached signature (index.json.sig, the hex
// Ed25519 signature of the exact index bytes):
//
//   {"name": "Frontier Exchange", "plugins": [{
//      "id": "acme.lasso", "name": "Lasso", "version": "1.2.0",
//      "description": "...", "author": "...", "category": "Gameplay",
//      "types": ["2d", "3d"], "tags": ["rope"],
//      "deps": [{"id": "ts.tumbleweed", "min": "1.0.0"}],
//      "downloads": {"windows-x64": {"url": "lasso.dll", "sha256": "...",
//                                    "size": 12345},
//                    "linux-x64": {...}}}]}
//
// An index is only used when its signature checks against a trusted key
// (or allow_unsigned is on, for local development). Every download is
// checked against the index's size and SHA-256 before it is written to the
// cache, and the library must load and report the advertised id before the
// install counts; anything else is rolled back.

#include "plugin/Catalog.h"
#include "store/Json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace store {

enum class Source { BuiltIn, Installed, Local, Remote, ComingSoon };
const char* to_string(Source s);

struct StoreDep {
  std::string id;
  std::string min;  // "" = any
  bool optional = false;
};

struct StoreEntry {
  std::string id;
  std::string name;
  std::string description;
  std::string author;
  std::string category;  // Gameplay / Engine / Editor / Sample / Platform / Tools
  std::string version;
  std::vector<StoreDep> deps;
  std::vector<std::string> tags;
  std::uint32_t types = 0;  // plugin::kType2D | kType3D | kTypeVR
  Source source = Source::Remote;
  std::string origin;       // "built-in", folder, index URL
  bool default_enabled = false;
  bool optional = true;     // ships only with projects that enable it

  // Install state.
  bool installed = false;           // in the cache (or built in)
  std::string installed_version;
  std::string installed_dir;        // <cache>/<id>
  bool update_available = false;
  bool in_search_path = false;      // already loaded from another plugin dir

  // Where an install comes from.
  std::string package_url;   // remote: resolved download URL
  std::string package_sha256;
  std::uint64_t package_size = 0;
  std::string local_path;    // local: the library file

  bool installable() const {
    return (source == Source::Remote || source == Source::Local) && !in_search_path;
  }
};

struct StoreSources {
  std::vector<std::string> local_folders;
  std::vector<std::string> index_urls;
  std::vector<std::string> trusted_keys;  // Ed25519 public keys (hex)
  bool allow_unsigned = false;
};

class TradingPost {
 public:
  // cache_root "" = default_cache_root().
  explicit TradingPost(std::string cache_root = {});

  // $TS_PLUGIN_CACHE, else <cwd>/TombStonePlugins (absolute).
  static std::string default_cache_root();
  // "windows-x64", "linux-x64", "macos-arm64"...: the downloads key used.
  static std::string platform_key();

  const std::string& cache_root() const { return cache_root_; }
  StoreSources& sources() { return sources_; }
  const StoreSources& sources() const { return sources_; }
  bool load_sources(std::string* error = nullptr);
  bool save_sources(std::string* error = nullptr) const;

  // Rebuild the catalog. `known` (optional) is a catalog the host already
  // filled (built-ins + search dirs): its static entries are listed as
  // built-in, its dynamic ones outside the cache as in_search_path. Remote
  // indexes are fetched here (blocking; the UI calls it on demand).
  void refresh(const plugin::Catalog* known = nullptr);

  const std::vector<StoreEntry>& entries() const { return entries_; }
  const StoreEntry* find(const std::string& id) const;
  // Notes and problems from the last refresh / install (index fetch
  // failures, bad signatures, ...).
  const std::vector<std::string>& notes() const { return notes_; }
  std::vector<std::string> categories() const;
  // Search over name / id / description / tags / author; category "" = all,
  // "Installed" and "Coming Soon" are special.
  std::vector<const StoreEntry*> search(const std::string& query,
                                        const std::string& category) const;

  // Download (remote) or copy (local) into <cache>/<id>, verify, record.
  bool install(const std::string& id, std::string* error);
  // Removes <cache>/<id>. A library still loaded by this process can not
  // be deleted on Windows: it is then marked (.remove) and skipped by the
  // plugin search, and cleaned on the next refresh.
  bool uninstall(const std::string& id, std::string* error);

  // The announced-but-not-built plugins (quests, dialogue, Steam, AI
  // toolkit, profiler).
  static std::vector<StoreEntry> coming_soon();

  // Index helpers (tools + smokes): parse an index's entries (no fetch).
  static bool parse_index(const std::string& text, const std::string& base_url,
                          std::vector<StoreEntry>* out, std::string* error);

 private:
  void add_or_merge(StoreEntry e);
  void scan_cache();
  void scan_local_folder(const std::string& dir);
  void fetch_index(const std::string& url);

  std::string cache_root_;
  StoreSources sources_;
  std::vector<StoreEntry> entries_;
  std::vector<std::string> notes_;
};

}  // namespace store
}  // namespace tombstone
}  // namespace ts
