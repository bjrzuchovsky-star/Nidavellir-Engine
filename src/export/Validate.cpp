#include "export/Exporter.h"
#include "export/Plugins.h"

#include "core/JsonMini.h"
#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "plugins/RuntimePlugins.h"
#include "runtime/GraphScript.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace exporter {

namespace fs = std::filesystem;

namespace {
std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

std::string slugify(const std::string& name) {
  std::string out;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out += static_cast<char>(std::tolower(u));
    } else if (!out.empty() && out.back() != '-') {
      out += '-';
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? std::string("game") : out;
}

bool is_editor_only(const std::string& rel) {
  const std::string r = lower(rel);
  std::size_t start = 0;
  // Any dot folder/file segment (.git, .ts_editor, .thumbs, seed markers).
  while (start <= r.size()) {
    const std::size_t slash = r.find('/', start);
    const std::string seg = r.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!seg.empty() && seg[0] == '.') return true;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  static const char* kDirs[] = {"export/", "exports/", "build/", "editor/", "thumbnails/",
                                "plugins/", "server/", "autosave/", "backup/"};
  for (const char* d : kDirs) {
    if (r.rfind(d, 0) == 0) return true;
  }
  static const char* kFiles[] = {"imgui.ini", "export_report.txt", "game.json", "game.tspak",
                                 "export_settings.json"};
  for (const char* f : kFiles) {
    if (r == f) return true;
  }
  static const char* kExt[] = {".bak", ".tmp", ".autosave", ".log", ".dll", ".so", ".dylib",
                               ".pdb", ".exe", ".zip", ".swp"};
  const std::string ext = fs::path(r).extension().string();
  for (const char* e : kExt) {
    if (ext == e) return true;
  }
  return !r.empty() && r.back() == '~';
}

std::vector<std::string> content_files(const std::string& project_dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(project_dir, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const std::string rel = fs::relative(it->path(), project_dir, ec).generic_string();
    if (it->is_directory(ec)) {
      if (is_editor_only(rel + "/x")) it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec) && !is_editor_only(rel)) out.push_back(rel);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<PluginNeed> project_plugins(const std::string& project_dir,
                                        const std::vector<std::string>& extra_dirs) {
  plugin::Catalog catalog;
  std::vector<std::string> errs;
  plugins::fill_catalog(catalog, project_dir, &errs);
  for (const std::string& d : extra_dirs) catalog.scan_folder(d, &errs);
  const plugins::ProjectPluginList list = plugins::read_project_plugins(project_dir);
  const std::vector<std::string> ids =
      list.listed ? list.ids : plugin::PluginManager::defaults(catalog, list.type);
  std::vector<PluginNeed> out;
  for (const std::string& id : ids) {
    PluginNeed n;
    n.id = id;
    n.editor_only = id == "ts.posse";
    if (const plugin::CatalogEntry* e = catalog.find(id)) {
      n.found = true;
      n.dynamic = e->dynamic;
      if (e->dynamic) n.library = e->source;
    }
    out.push_back(std::move(n));
  }
  return out;
}

ValidationReport validate_project(const std::string& project_dir, const std::string& profile) {
  ValidationReport rep;
  const bool strict = profile != "dev";
  std::error_code ec;
  const fs::path root(project_dir);
  if (!fs::is_regular_file(root / "project.json", ec)) {
    rep.errors.push_back("project.json missing in " + project_dir);
    return rep;
  }
  const auto pj = json_mini::parse_object(read_all(root / "project.json"));
  if (!pj) {
    rep.errors.push_back("project.json is not valid JSON");
    return rep;
  }
  const std::string dim = lower(json_mini::get_string(*pj, "dimension", "2d"));
  rep.notes.push_back("dimension: " + dim);
  if (!fs::is_regular_file(root / "scene.json", ec)) {
    // The editor writes the starter scene the first time a claim is opened.
    (strict ? rep.errors : rep.warnings)
        .push_back("scene.json missing (start scene); open and save the project in the editor first");
  }

  const std::vector<std::string> files = content_files(project_dir);
  std::set<std::string> have(files.begin(), files.end());
  std::set<std::string> have_names;
  for (const std::string& f : files) have_names.insert(lower(fs::path(f).filename().string()));

  // Scripts: every .lua must compile.
  int scripts = 0;
  for (const std::string& f : files) {
    if (lower(fs::path(f).extension().string()) != ".lua") continue;
    ++scripts;
    const std::string src = read_all(root / f);
    lua_State* L = luaL_newstate();
    if (luaL_loadbufferx(L, src.data(), src.size(), ("@" + f).c_str(), "t") != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      rep.errors.push_back("script error: " + std::string(msg ? msg : f));
    }
    lua_close(L);
  }
  // Visual scripts: parse + node / link check.
  for (const std::string& f : files) {
    if (f.size() < 11 || f.compare(f.size() - 11, 11, ".graph.json") != 0) continue;
    ++scripts;
    runtime::Graph g;
    std::string err;
    std::vector<std::string> problems;
    if (!runtime::parse_graph(read_all(root / f), &g, &err)) {
      rep.errors.push_back("graph error: " + f + ": " + err);
    } else if (!runtime::check_graph(g, &problems)) {
      for (const std::string& p : problems) rep.errors.push_back("graph error: " + f + ": " + p);
    }
  }
  rep.notes.push_back(std::to_string(scripts) + " script(s) compiled");

  // Assets: every "....ext" string in the JSON data must be a file we ship.
  static const std::regex kRef(
      R"re("([^"\\:]{1,240}\.(png|jpg|jpeg|bmp|tga|wav|ogg|mp3|flac|gltf|glb|lua|ttf|otf))")re",
      std::regex::icase);
  int refs = 0;
  std::set<std::string> missing;
  for (const std::string& f : files) {
    if (lower(fs::path(f).extension().string()) != ".json") continue;
    const std::string text = read_all(root / f);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), kRef);
         it != std::sregex_iterator(); ++it) {
      std::string ref = (*it)[1].str();
      for (char& c : ref) if (c == '\\') c = '/';
      ++refs;
      const std::string dir = fs::path(f).parent_path().generic_string();
      const bool ok = have.count(ref) || have.count("assets/" + ref) ||
                      (!dir.empty() && have.count(dir + "/" + ref)) ||
                      have_names.count(lower(fs::path(ref).filename().string()));
      if (!ok) missing.insert(ref + " (from " + f + ")");
    }
  }
  for (const std::string& m : missing) rep.warnings.push_back("missing asset: " + m);
  rep.notes.push_back(std::to_string(refs) + " asset reference(s) checked, " +
                      std::to_string(files.size()) + " content file(s)");

  for (const PluginNeed& p : project_plugins(project_dir, {})) {
    if (p.editor_only) {
      rep.notes.push_back("plugin " + p.id + " is editor-only (not shipped)");
    } else if (!p.found) {
      (strict ? rep.errors : rep.warnings).push_back("plugin " + p.id + " not found (no built-in, no DLL)");
    } else {
      rep.notes.push_back("plugin " + p.id + (p.dynamic ? " (dynamic: " + p.library + ")" : " (built-in)"));
    }
  }
  return rep;
}

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
