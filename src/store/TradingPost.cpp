#include "store/TradingPost.h"

#include "plugin/DynamicLibrary.h"
#include "store/Crypto.h"
#include "store/Fetch.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <chrono>
#include <set>
#include <thread>

namespace ts {
namespace tombstone {
namespace store {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::uint32_t types_from(const Json& j) {
  std::uint32_t t = 0;
  for (const std::string& s : j.strings_at("types")) {
    const std::string l = lower(s);
    if (l == "2d") t |= plugin::kType2D;
    if (l == "3d") t |= plugin::kType3D;
    if (l == "vr") t |= plugin::kTypeVR;
    if (l == "all") t |= plugin::kTypeAll;
  }
  return t ? t : plugin::kTypeAll;
}

Json types_json(std::uint32_t t) {
  Json a = Json::array();
  if (t & plugin::kType2D) a.push("2d");
  if (t & plugin::kType3D) a.push("3d");
  if (t & plugin::kTypeVR) a.push("vr");
  return a;
}

StoreEntry from_info(const plugin::PluginInfo& info) {
  StoreEntry e;
  e.id = info.id;
  e.name = info.name;
  e.description = info.description;
  e.author = info.author;
  e.category = plugin::to_string(info.category);
  e.version = info.version.str();
  e.types = info.project_types;
  e.default_enabled = info.default_enabled;
  for (const plugin::Dependency& d : info.deps) {
    e.deps.push_back({d.id, d.min.str(), d.optional});
  }
  return e;
}

Json entry_json(const StoreEntry& e) {
  Json j = Json::object();
  j.set("id", e.id);
  j.set("name", e.name);
  j.set("version", e.version);
  j.set("description", e.description);
  j.set("author", e.author);
  j.set("category", e.category);
  j.set("types", types_json(e.types));
  Json deps = Json::array();
  for (const StoreDep& d : e.deps) {
    Json dj = Json::object();
    dj.set("id", d.id);
    if (!d.min.empty()) dj.set("min", d.min);
    if (d.optional) dj.set("optional", true);
    deps.push(dj);
  }
  j.set("deps", deps);
  Json tags = Json::array();
  for (const std::string& t : e.tags) tags.push(t);
  j.set("tags", tags);
  return j;
}

void entry_meta_from(const Json& p, StoreEntry* e) {
  e->id = p.string_at("id");
  e->name = p.string_at("name", e->id);
  e->version = p.string_at("version", "0.0.0");
  e->description = p.string_at("description");
  e->author = p.string_at("author");
  e->category = p.string_at("category", "Gameplay");
  e->types = types_from(p);
  e->tags = p.strings_at("tags");
  e->deps.clear();
  for (const Json& d : p.get("deps").items()) {
    if (d.is_string()) {
      e->deps.push_back({d.as_string(), {}, false});
    } else if (d.is_object() && !d.string_at("id").empty()) {
      e->deps.push_back({d.string_at("id"), d.string_at("min"), d.get("optional").as_bool()});
    }
  }
}

int compare_versions(const std::string& a, const std::string& b) {
  return plugin::Version::from(a).compare(plugin::Version::from(b));
}

// The last path segment of a URL / path, reduced to safe characters.
std::string package_file_name(const std::string& url, const std::string& id) {
  std::string name = url.substr(0, url.find_first_of("?#"));
  const std::size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  std::string safe;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') safe += c;
  }
  const std::string ext = plugin::DynamicLibrary::native_extension();
  if (safe.empty() || safe[0] == '.' || safe.size() < ext.size() ||
      safe.compare(safe.size() - ext.size(), ext.size(), ext) != 0) {
    std::string base;
    for (char c : id) base += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    safe = base + ext;
  }
  return safe;
}

}  // namespace

const char* to_string(Source s) {
  switch (s) {
    case Source::BuiltIn: return "Built-in";
    case Source::Installed: return "Installed";
    case Source::Local: return "Local";
    case Source::Remote: return "Remote";
    case Source::ComingSoon: return "Coming soon";
  }
  return "?";
}

TradingPost::TradingPost(std::string cache_root)
    : cache_root_(cache_root.empty() ? default_cache_root() : std::move(cache_root)) {}

std::string TradingPost::default_cache_root() {
  if (const char* env = std::getenv("TS_PLUGIN_CACHE")) {
    if (*env) return env;
  }
  std::error_code ec;
  return fs::absolute("TombStonePlugins", ec).string();
}

std::string TradingPost::platform_key() {
#if defined(_WIN32)
  return "windows-x64";
#elif defined(__APPLE__)
#if defined(__aarch64__)
  return "macos-arm64";
#else
  return "macos-x64";
#endif
#elif defined(__aarch64__)
  return "linux-arm64";
#else
  return "linux-x64";
#endif
}

bool TradingPost::load_sources(std::string* error) {
  sources_ = StoreSources{};
  const fs::path file = fs::path(cache_root_) / "trading_post.json";
  std::error_code ec;
  if (!fs::exists(file, ec)) return true;
  Json j;
  if (!read_json_file(file.string(), &j, error)) return false;
  sources_.local_folders = j.strings_at("local_folders");
  sources_.index_urls = j.strings_at("index_urls");
  sources_.trusted_keys = j.strings_at("trusted_keys");
  sources_.allow_unsigned = j.get("allow_unsigned").as_bool(false);
  return true;
}

bool TradingPost::save_sources(std::string* error) const {
  Json j = Json::object();
  auto arr = [](const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const std::string& s : v) a.push(s);
    return a;
  };
  j.set("local_folders", arr(sources_.local_folders));
  j.set("index_urls", arr(sources_.index_urls));
  j.set("trusted_keys", arr(sources_.trusted_keys));
  j.set("allow_unsigned", sources_.allow_unsigned);
  return write_text_file((fs::path(cache_root_) / "trading_post.json").string(), j.dump(), error);
}

const StoreEntry* TradingPost::find(const std::string& id) const {
  for (const StoreEntry& e : entries_) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

void TradingPost::add_or_merge(StoreEntry e) {
  for (StoreEntry& have : entries_) {
    if (have.id != e.id) continue;
    if (have.source == Source::BuiltIn || e.source == Source::ComingSoon) return;
    if (have.source == Source::Installed || have.in_search_path) {
      // Installed: remember where an update would come from.
      if (e.source == Source::Remote || e.source == Source::Local) {
        if (compare_versions(e.version, have.installed_version) > 0 && !have.in_search_path) {
          have.update_available = true;
          have.version = e.version;
          have.package_url = e.package_url;
          have.package_sha256 = e.package_sha256;
          have.package_size = e.package_size;
          have.local_path = e.local_path;
          have.origin = e.origin;
        } else if (have.package_url.empty() && have.local_path.empty()) {
          have.package_url = e.package_url;
          have.package_sha256 = e.package_sha256;
          have.package_size = e.package_size;
          have.local_path = e.local_path;
        }
        if (have.description.empty()) have.description = e.description;
        if (have.tags.empty()) have.tags = e.tags;
      }
      return;
    }
    if (have.source == Source::ComingSoon || compare_versions(e.version, have.version) > 0) have = e;
    return;
  }
  entries_.push_back(std::move(e));
}

void TradingPost::scan_cache() {
  std::error_code ec;
  if (!fs::is_directory(cache_root_, ec)) return;
  for (const auto& d : fs::directory_iterator(cache_root_, ec)) {
    if (!d.is_directory(ec)) continue;
    if (fs::exists(d.path() / ".remove", ec)) {
      std::error_code rec;
      fs::remove_all(d.path(), rec);  // finish an uninstall when it is free
      continue;
    }
    if (d.path().filename().string().find(".tmp") != std::string::npos) {
      fs::remove_all(d.path(), ec);  // an interrupted install
      continue;
    }
    Json j;
    if (!read_json_file((d.path() / "installed.json").string(), &j, nullptr)) continue;
    StoreEntry e;
    entry_meta_from(j, &e);
    if (e.id.empty()) continue;
    e.source = Source::Installed;
    e.origin = j.string_at("origin");
    e.installed = true;
    e.installed_version = e.version;
    e.installed_dir = d.path().string();
    add_or_merge(std::move(e));
  }
}

void TradingPost::scan_local_folder(const std::string& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    notes_.push_back("Local folder not found: " + dir);
    return;
  }
  plugin::Catalog c;
  std::vector<std::string> errs;
  c.scan_folder(dir, &errs);
  for (const std::string& e : errs) notes_.push_back(dir + ": " + e);
  for (const plugin::CatalogEntry& ce : c.entries()) {
    StoreEntry e = from_info(ce.info);
    e.source = Source::Local;
    e.origin = dir;
    e.local_path = ce.source;
    add_or_merge(std::move(e));
  }
}

bool TradingPost::parse_index(const std::string& text, const std::string& base_url,
                              std::vector<StoreEntry>* out, std::string* error) {
  Json j;
  std::string err;
  if (!Json::parse(text, &j, &err)) {
    if (error) *error = "index is not JSON: " + err;
    return false;
  }
  const Json& list = j.get("plugins");
  if (!list.is_array()) {
    if (error) *error = "index has no \"plugins\" array";
    return false;
  }
  const std::string key = platform_key();
  for (const Json& p : list.items()) {
    StoreEntry e;
    entry_meta_from(p, &e);
    if (!plugin::Catalog::valid_id(e.id)) continue;
    e.source = p.get("coming_soon").as_bool() ? Source::ComingSoon : Source::Remote;
    e.origin = base_url;
    const Json& dl = p.get("downloads").get(key);
    if (dl.is_object()) {
      e.package_url = resolve_url(base_url, dl.string_at("url"));
      e.package_sha256 = lower(dl.string_at("sha256"));
      e.package_size = static_cast<std::uint64_t>(dl.get("size").as_number(0));
    }
    out->push_back(std::move(e));
  }
  return true;
}

void TradingPost::fetch_index(const std::string& url) {
  std::string text, err;
  FetchOptions small;
  small.max_bytes = 4u * 1024u * 1024u;
  if (!fetch_url(url, &text, &err, small)) {
    notes_.push_back("Index " + url + ": " + err);
    return;
  }
  std::string sig, sig_err;
  const bool have_sig = fetch_url(url + ".sig", &sig, &sig_err, small);
  bool trusted = false;
  if (have_sig) {
    // The signature file may carry a trailing newline.
    while (!sig.empty() && std::isspace(static_cast<unsigned char>(sig.back()))) sig.pop_back();
    for (const std::string& key : sources_.trusted_keys) {
      if (ed25519_verify(key, sig, text)) {
        trusted = true;
        break;
      }
    }
  }
  if (!trusted) {
    const std::string why = have_sig ? "signature does not match a trusted key"
                                     : "unsigned (no " + url + ".sig)";
    if (!sources_.allow_unsigned) {
      notes_.push_back("Index " + url + " refused: " + why);
      return;
    }
    notes_.push_back("Index " + url + ": " + why + " -- used because allow_unsigned is on");
  }
  std::vector<StoreEntry> list;
  if (!parse_index(text, url, &list, &err)) {
    notes_.push_back("Index " + url + ": " + err);
    return;
  }
  for (StoreEntry& e : list) {
    if (e.source == Source::Remote && e.package_url.empty()) {
      notes_.push_back(e.id + ": no download for " + platform_key());
    }
    add_or_merge(std::move(e));
  }
}

std::vector<StoreEntry> TradingPost::coming_soon() {
  auto make = [](const char* id, const char* name, const char* cat, const char* desc,
                 std::vector<std::string> tags) {
    StoreEntry e;
    e.id = id;
    e.name = name;
    e.category = cat;
    e.description = desc;
    e.author = "TombStone";
    e.version = "0.1.0";
    e.types = plugin::kTypeAll;
    e.source = Source::ComingSoon;
    e.origin = "TombStone roadmap";
    e.tags = std::move(tags);
    return e;
  };
  return {
      make("ts.steam", "Steam Hitch", "Platform",
           "Steamworks: achievements, stats, cloud saves, rich presence and lobbies for the posse.",
           {"steam", "platform", "multiplayer"}),
  };
}

void TradingPost::refresh(const plugin::Catalog* known) {
  entries_.clear();
  notes_.clear();
  std::error_code ec;
  const fs::path cache = fs::weakly_canonical(fs::path(cache_root_), ec);
  if (known) {
    for (const plugin::CatalogEntry& ce : known->entries()) {
      StoreEntry e = from_info(ce.info);
      e.installed = true;
      e.installed_version = e.version;
      if (!ce.dynamic) {
        e.source = Source::BuiltIn;
        e.origin = "built-in";
        e.optional = !ce.info.default_enabled;
        entries_.push_back(std::move(e));
        continue;
      }
      // A library from the cache is picked up by scan_cache below; one from
      // another search dir (project plugins/, $TS_PLUGIN_PATH, the build
      // tree) is in the wagon already and not ours to install.
      const fs::path lib = fs::weakly_canonical(fs::path(ce.source), ec);
      const std::string rel = lib.parent_path().parent_path().string();
      if (!cache.empty() && rel == cache.string()) continue;
      e.source = Source::Local;
      e.in_search_path = true;
      e.origin = lib.parent_path().string();
      e.local_path = ce.source;
      entries_.push_back(std::move(e));
    }
  }
  scan_cache();
  for (const std::string& dir : sources_.local_folders) scan_local_folder(dir);
  for (const std::string& url : sources_.index_urls) fetch_index(url);
  for (StoreEntry& e : coming_soon()) add_or_merge(std::move(e));
  std::stable_sort(entries_.begin(), entries_.end(), [](const StoreEntry& a, const StoreEntry& b) {
    const bool sa = a.source == Source::ComingSoon, sb = b.source == Source::ComingSoon;
    if (sa != sb) return !sa;
    return lower(a.name) < lower(b.name);
  });
}

std::vector<std::string> TradingPost::categories() const {
  std::vector<std::string> out;
  for (const char* c : {"Gameplay", "Engine", "Editor", "Platform", "Tools", "Sample"}) {
    out.push_back(c);
  }
  for (const StoreEntry& e : entries_) {
    if (!e.category.empty() && std::find(out.begin(), out.end(), e.category) == out.end()) {
      out.push_back(e.category);
    }
  }
  return out;
}

std::vector<const StoreEntry*> TradingPost::search(const std::string& query,
                                                   const std::string& category) const {
  std::vector<const StoreEntry*> out;
  const std::string q = lower(query);
  for (const StoreEntry& e : entries_) {
    if (category == "Installed") {
      if (!e.installed || e.source == Source::ComingSoon) continue;
    } else if (category == "Coming Soon") {
      if (e.source != Source::ComingSoon) continue;
    } else if (category == "Updates") {
      if (!e.update_available) continue;
    } else if (!category.empty() && lower(e.category) != lower(category)) {
      continue;
    }
    if (!q.empty()) {
      bool hit = lower(e.name).find(q) != std::string::npos ||
                 lower(e.id).find(q) != std::string::npos ||
                 lower(e.description).find(q) != std::string::npos ||
                 lower(e.author).find(q) != std::string::npos;
      for (const std::string& t : e.tags) hit = hit || lower(t).find(q) != std::string::npos;
      if (!hit) continue;
    }
    out.push_back(&e);
  }
  return out;
}

bool TradingPost::install(const std::string& id, std::string* error) {
  std::set<std::string> visiting;
  std::function<bool(const std::string&)> go = [&](const std::string& want) -> bool {
    const StoreEntry* e = find(want);
    if (!e) {
      if (error) *error = "No plugin \"" + want + "\" in the Trading Post";
      return false;
    }
    if (e->source == Source::ComingSoon) {
      if (error) *error = e->name + " is coming soon -- nothing to install yet";
      return false;
    }
    if (e->source == Source::BuiltIn) return true;  // nothing to fetch
    if (e->installed && !e->update_available) return true;
    if (e->in_search_path) return true;
    if (!visiting.insert(want).second) return true;  // a loop; the manager reports it
    // Required dependencies first (optional ones are the user's call).
    for (const StoreDep& d : e->deps) {
      if (d.optional) continue;
      if (!find(d.id)) {
        if (error) *error = e->name + " needs \"" + d.id + "\", which no source offers";
        return false;
      }
      if (!go(d.id)) return false;
    }
    const StoreEntry entry = *find(want);  // copy: refresh below reshuffles
    const std::string src = !entry.local_path.empty() ? entry.local_path : entry.package_url;
    if (src.empty()) {
      if (error) *error = entry.name + ": no package for " + platform_key();
      return false;
    }
    std::string bytes, err;
    if (!entry.local_path.empty()) {
      if (!read_text_file(entry.local_path, &bytes, &err)) {
        if (error) *error = err;
        return false;
      }
    } else {
      if (entry.package_sha256.size() != 64) {
        if (error) *error = entry.name + ": the index pins no SHA-256 for this download; refused";
        return false;
      }
      FetchOptions opt;
      if (entry.package_size > 0) opt.max_bytes = static_cast<std::size_t>(entry.package_size);
      if (!fetch_url(entry.package_url, &bytes, &err, opt)) {
        if (error) *error = entry.name + ": download failed: " + err;
        return false;
      }
      if (entry.package_size > 0 && bytes.size() != entry.package_size) {
        if (error) {
          *error = entry.name + ": size mismatch (" + std::to_string(bytes.size()) + " bytes, index says " +
                   std::to_string(entry.package_size) + "); refused";
        }
        return false;
      }
      const std::string got = sha256_hex(bytes);
      if (got != entry.package_sha256) {
        if (error) *error = entry.name + ": SHA-256 mismatch (got " + got + "); refused";
        return false;
      }
    }
    // Stage, check that it loads as the plugin it claims to be, then swap in.
    std::error_code ec;
    const fs::path final_dir = fs::path(cache_root_) / entry.id;
    const fs::path stage = fs::path(cache_root_) / (entry.id + ".tmp");
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    const fs::path lib = stage / package_file_name(src, entry.id);
    if (!write_text_file(lib.string(), bytes, &err)) {
      fs::remove_all(stage, ec);
      if (error) *error = err;
      return false;
    }
    std::string loaded_version;
    {
      plugin::Catalog probe;
      std::string lerr;
      if (!probe.load_library(lib.string(), &lerr)) {
        fs::remove_all(stage, ec);
        if (error) *error = entry.name + ": the package does not load as a plugin: " + lerr;
        return false;
      }
      const plugin::CatalogEntry* ce = probe.find(entry.id);
      if (!ce) {
        fs::remove_all(stage, ec);
        if (error) *error = entry.name + ": the library is not \"" + entry.id + "\"; refused";
        return false;
      }
      loaded_version = ce->info.version.str();
    }
    Json rec = entry_json(entry);
    rec.set("version", loaded_version);
    rec.set("origin", entry.origin);
    rec.set("source", to_string(entry.source));
    rec.set("package", entry.local_path.empty() ? entry.package_url : entry.local_path);
    rec.set("sha256", sha256_hex(bytes));
    rec.set("file", lib.filename().string());
    if (!write_text_file((stage / "installed.json").string(), rec.dump(), &err)) {
      fs::remove_all(stage, ec);
      if (error) *error = err;
      return false;
    }
    if (fs::exists(final_dir, ec)) {
      fs::remove_all(final_dir, ec);
      if (ec) {
        fs::remove_all(stage, ec);
        if (error) {
          *error = entry.name + " is in use (close projects using it, then update): " + ec.message();
        }
        return false;
      }
    }
    // Windows: a scanner or indexer can hold a freshly written file for a
    // moment, so the rename fails transiently (sometimes with a bogus
    // "completed successfully" message). Retry with backoff, then fall
    // back to copy + remove.
    std::error_code mv;
    for (int attempt = 0; attempt < 8; ++attempt) {
      mv.clear();
      fs::rename(stage, final_dir, mv);
      if (!mv && fs::exists(final_dir / "installed.json", ec)) break;
      if (!mv) mv = std::make_error_code(std::errc::io_error);
      std::this_thread::sleep_for(std::chrono::milliseconds(25 << std::min(attempt, 4)));
    }
    if (mv) {
      std::error_code cp;
      fs::remove_all(final_dir, ec);
      fs::copy(stage, final_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, cp);
      if (!cp && fs::exists(final_dir / "installed.json", ec)) {
        fs::remove_all(stage, ec);
        mv.clear();
      } else {
        fs::remove_all(final_dir, ec);
      }
    }
    if (mv) {
      fs::remove_all(stage, ec);
      if (error) {
        *error = "could not move the package into the cache (after retries): " + mv.message() + " [" +
                 std::to_string(mv.value()) + "]";
      }
      return false;
    }
    notes_.push_back("Installed " + entry.name + " " + loaded_version);
    // Reflect the install without another network round trip.
    for (StoreEntry& have : entries_) {
      if (have.id != entry.id) continue;
      have.installed = true;
      have.installed_version = loaded_version;
      have.version = loaded_version;
      have.installed_dir = final_dir.string();
      have.update_available = false;
      if (have.source != Source::BuiltIn) have.source = Source::Installed;
    }
    return true;
  };
  return go(id);
}

bool TradingPost::uninstall(const std::string& id, std::string* error) {
  StoreEntry* e = nullptr;
  for (StoreEntry& have : entries_) {
    if (have.id == id) e = &have;
  }
  if (!e || e->source == Source::BuiltIn) {
    if (error) *error = e ? e->name + " is built into the engine; disable it per project instead"
                          : "No plugin \"" + id + "\"";
    return false;
  }
  if (e->installed_dir.empty()) {
    if (error) *error = e->name + " is not installed from the Trading Post";
    return false;
  }
  std::error_code ec;
  const fs::path dir = e->installed_dir;
  fs::remove_all(dir, ec);
  if (ec || fs::exists(dir)) {
    // Still loaded (Windows keeps the DLL open): mark it; the plugin search
    // skips it from now on and the next refresh deletes it.
    std::error_code wec;
    write_text_file((dir / ".remove").string(), "uninstalled\n", nullptr);
    notes_.push_back(e->name + " is still loaded; it goes when the editor lets go of it");
  }
  e->installed = false;
  e->installed_dir.clear();
  e->installed_version.clear();
  if (e->source == Source::Installed) {
    e->source = !e->local_path.empty() ? Source::Local : Source::Remote;
  }
  return true;
}

}  // namespace store
}  // namespace tombstone
}  // namespace ts
