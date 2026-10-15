#include "export/Exporter.h"

#include "export/Plugins.h"
#include "export/WinIcon.h"
#include "export/Zip.h"

#include "core/JsonMini.h"
#include "runtime/NativeScript.h"
#include "scene/SceneJson.h"
#include "vfs/Pak.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace exporter {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr const char* kExeExt = ".exe";
#else
constexpr const char* kExeExt = "";
#endif

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_all(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o << s;
  return static_cast<bool>(o);
}

bool copy_into(const fs::path& from, const fs::path& to, std::string* err) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    *err = "copy " + from.string() + " -> " + to.string() + ": " + ec.message();
    return false;
  }
  return true;
}

// Build root (folder with CMakeCache.txt) above the runtime, if any.
fs::path build_root_of(const fs::path& exe) {
  std::error_code ec;
  for (fs::path p = exe.parent_path(); !p.empty(); p = p.parent_path()) {
    if (fs::exists(p / "CMakeCache.txt", ec)) return p;
    if (p == p.root_path()) break;
  }
  return {};
}

std::string find_server(const fs::path& game) {
  std::error_code ec;
  const std::string name = std::string("ts_server") + kExeExt;
  const fs::path dir = game.parent_path();
  std::vector<fs::path> c = {dir / name, dir.parent_path() / "server" / name,
                             dir.parent_path().parent_path() / "server" / dir.filename() / name};
  for (const fs::path& p : c) {
    if (fs::is_regular_file(p, ec)) return p.string();
  }
  return {};
}

std::string json_num_object(const std::vector<std::pair<std::string, std::string>>& s,
                            const std::vector<std::pair<std::string, int>>& n) {
  std::string out = "{\n";
  bool first = true;
  for (const auto& [k, v] : s) {
    out += first ? "" : ",\n";
    first = false;
    out += "  \"" + json_mini::escape_string(k) + "\": \"" + json_mini::escape_string(v) + "\"";
  }
  for (const auto& [k, v] : n) {
    out += first ? "" : ",\n";
    first = false;
    out += "  \"" + json_mini::escape_string(k) + "\": " + std::to_string(v);
  }
  return out + "\n}\n";
}

}  // namespace

std::string ExportResult::text() const {
  std::ostringstream o;
  o << "TombStone Freight Office - export report\n";
  o << "result: " << (ok ? "SHIPPED" : "HELD AT THE DEPOT") << '\n';
  if (!game_dir.empty()) o << "game folder: " << game_dir << '\n';
  if (!exe_path.empty()) o << "executable: " << exe_path << '\n';
  if (!pak_path.empty()) {
    o << "pak: " << pak_path << " (" << pak_files << " files, " << pak_bytes
      << " bytes, hash " << vfs::hex64(pak_hash) << ")\n";
  }
  if (!zip_path.empty()) o << "zip: " << zip_path << '\n';
  for (const std::string& p : plugins_shipped) o << "plugin dll: " << p << '\n';
  for (const std::string& e : report.errors) o << "ERROR: " << e << '\n';
  for (const std::string& w : report.warnings) o << "warning: " << w << '\n';
  for (const std::string& n : report.notes) o << "note: " << n << '\n';
  return o.str();
}

bool export_project(const ExportOptions& opt, ExportResult* out, const Progress& progress) {
  ExportResult& r = *out;
  r = ExportResult{};
  auto say = [&](const std::string& s) {
    if (progress) progress(s);
  };
  auto fail = [&](const std::string& e) {
    r.report.errors.push_back(e);
    r.ok = false;
    say("ERROR: " + e);
    return false;
  };
  const bool dev = opt.profile == "dev";
  const std::string profile = dev ? "dev" : "shipping";
  std::error_code ec;
  const fs::path proj = fs::absolute(fs::path(opt.project_dir), ec).lexically_normal();

  say("Inspecting " + proj.string() + " (" + profile + ")");
  r.report = validate_project(proj.string(), profile);
  if (!r.report.ok()) {
    if (!dev) return fail("validation failed; fix the errors above (or export the dev profile)");
    r.report.warnings.insert(r.report.warnings.end(), r.report.errors.begin(), r.report.errors.end());
    r.report.errors.clear();
  }

  const auto pj = json_mini::parse_object(read_all(proj / "project.json"));
  if (!pj) return fail("project.json unreadable");
  const std::string name = !opt.name.empty() ? opt.name
                                             : json_mini::get_string(*pj, "name", proj.filename().string());
  std::string version = !opt.version.empty() ? opt.version : json_mini::get_string(*pj, "version", "");
  if (version.empty()) version = "1.0.0";
  std::string dimension = json_mini::get_string(*pj, "dimension", "2d");
  for (char& c : dimension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // VR is a 3D switch: old "vr" dimensions ship as 3d + vr.
  const std::string vr_flag = json_mini::get_string(*pj, "vr", json_mini::get_string(*pj, "xr", ""));
  const bool vr = dimension == "vr" || (dimension == "3d" && vr_flag == "true");
  if (dimension == "vr") dimension = "3d";
  const std::string slug = slugify(name);
  const std::string exe_name = (opt.exe_name.empty() ? slug : opt.exe_name) + kExeExt;
  auto pick_int = [&](int given, const char* key, int fallback) {
    if (given > 0) return given;
    const int v = std::atoi(json_mini::get_string(*pj, key, "0").c_str());
    return v > 0 ? v : fallback;
  };
  const int ww = pick_int(opt.window_width, "window_width", 1280);
  const int wh = pick_int(opt.window_height, "window_height", 720);
  std::string icon = opt.icon;
  if (icon.empty()) {
    for (const char* c : {"icon.png", "assets/icon.png"}) {
      if (fs::is_regular_file(proj / c, ec)) {
        icon = c;
        break;
      }
    }
  }

  // Runtime.
  const fs::path game_exe(opt.runtime_exe);
  if (opt.runtime_exe.empty() || !fs::is_regular_file(game_exe, ec)) {
    return fail("ts_game runtime not found (" + opt.runtime_exe + "); build ts_game (Release) first");
  }
  const std::string gpath = game_exe.generic_string();
  if (gpath.find("/Debug/") != std::string::npos) {
    r.report.warnings.push_back("runtime comes from a Debug build: " + gpath);
  } else if (gpath.find("/Release/") == std::string::npos) {
    // Single-config build tree: ask its CMakeCache.
    std::string type;
    const fs::path root = build_root_of(game_exe);
    std::istringstream cache(root.empty() ? std::string() : read_all(root / "CMakeCache.txt"));
    for (std::string line; std::getline(cache, line);) {
      if (line.rfind("CMAKE_BUILD_TYPE:", 0) == 0) type = line.substr(line.find('=') + 1);
    }
    if (type == "Release" || type == "RelWithDebInfo" || type == "MinSizeRel") {
      r.report.notes.push_back("runtime: " + gpath + " (" + type + ")");
    } else {
      r.report.warnings.push_back("runtime " + gpath + " is not a Release build (" +
                                  (type.empty() ? "unknown config" : type) + ")");
    }
  }

  const fs::path game_dir = fs::absolute(fs::path(opt.out_dir), ec) / slug;
  if (game_dir.lexically_normal() == proj || proj.lexically_normal().string().rfind(game_dir.lexically_normal().string(), 0) == 0) {
    return fail("output folder must not contain the project");
  }
  fs::remove_all(game_dir, ec);
  fs::create_directories(game_dir, ec);
  if (ec) return fail("cannot create " + game_dir.string() + ": " + ec.message());
  r.game_dir = game_dir.string();

  // Cook: content -> game.tspak.
  say("Cooking content into game.tspak");
  vfs::PakWriter pak;
  std::string err;
  // C++ scripts: build the project module and ship it (sources stay home
  // in shipping exports).
  if (runtime::native_scripts::has_sources(proj.string())) {
    say("Compiling C++ scripts");
    const runtime::native_scripts::BuildResult b = runtime::native_scripts::build(proj.string());
    for (const runtime::LogEntry& p : b.problems) {
      r.report.errors.push_back("C++ " + p.file + ":" + std::to_string(p.line) + ": " + p.text);
    }
    if (!b.ok) return fail(b.error.empty() ? "C++ scripts did not build" : b.error);
    if (!pak.add_file("native/" + runtime::native_scripts::module_file_name(), b.module, &err)) return fail(err);
    r.report.notes.push_back("C++ scripts: shipped native/" + runtime::native_scripts::module_file_name());
  }
  for (const std::string& rel : content_files(proj.string())) {
    if (!dev && (rel.size() > 4 && (rel.compare(rel.size() - 4, 4, ".cpp") == 0 ||
                                    rel.compare(rel.size() - 2, 2, ".h") == 0) &&
                 rel.rfind("scripts/", 0) == 0)) {
      continue;  // compiled into native/
    }
    if (rel == "project.json" && !dev) {
      // Strip editor-only bookkeeping (created / last_opened / editor_*).
      std::vector<std::pair<std::string, std::string>> keep;
      std::vector<std::pair<std::string, bool>> bools;
      for (const auto& [k, v] : *pj) {
        if (k == "created" || k == "last_opened" || k.rfind("editor", 0) == 0) continue;
        if (v == "true" || v == "false") bools.emplace_back(k, v == "true");
        else keep.emplace_back(k, v);
      }
      std::sort(keep.begin(), keep.end());
      std::sort(bools.begin(), bools.end());
      pak.add(rel, json_mini::write_object(keep, bools));
      continue;
    }
    if (rel == "scene.json" && !dev) {
      // Strip the editor's tool state (selection, grid, snap) from 2D scenes.
      scene_json::SceneDoc doc;
      std::string perr;
      if (dimension == "2d" && scene_json::load_file((proj / rel).string(), &doc, &perr)) {
        doc.selected_id.reset();
        doc.selection.clear();
        doc.grid_size.reset();
        doc.snap.reset();
        pak.add(rel, scene_json::write(doc));
        continue;
      }
    }
    if (!pak.add_file(rel, (proj / rel).string(), &err)) return fail(err);
  }

  r.pak_path = (game_dir / "game.tspak").string();
  r.pak_files = pak.count();
  r.pak_bytes = pak.total_bytes();
  if (!pak.write(r.pak_path, &r.pak_hash, &err)) return fail(err);
  say("game.tspak: " + std::to_string(r.pak_files) + " files, hash " + vfs::hex64(r.pak_hash));

  // Runtime executable (+ the DLLs that sit beside it on Windows).
  say("Loading the runtime: " + game_exe.string());
  r.exe_path = (game_dir / exe_name).string();
  if (!copy_into(game_exe, r.exe_path, &err)) return fail(err);
  fs::permissions(r.exe_path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                  fs::perm_options::add, ec);
  for (const auto& e : fs::directory_iterator(game_exe.parent_path(), ec)) {
    const std::string ext = e.path().extension().string();
    if (ext == ".dll" || (dev && ext == ".pdb" && e.path().stem() == "ts_game")) {
      if (!copy_into(e.path(), game_dir / e.path().filename(), &err)) return fail(err);
    }
  }
  const fs::path broot = build_root_of(game_exe);
  if (!broot.empty()) {
    // OpenXR loader: static on MSVC; a shared lib on Linux (ships in lib/).
    const fs::path xr = broot / "_deps" / "ts_openxr-build" / "src" / "loader";
    if (fs::is_directory(xr, ec)) {
      for (const auto& e : fs::directory_iterator(xr, ec)) {
        const std::string f = e.path().filename().string();
        if (f == "libopenxr_loader.so.1") {  // the soname ts_game links
          if (!copy_into(fs::canonical(e.path(), ec), game_dir / "lib" / f, &err)) return fail(err);
        } else if (f == "openxr_loader.dll") {
          if (!copy_into(e.path(), game_dir / f, &err)) return fail(err);
        }
      }
    }
  }

  // Icon: inside the pak for the window; stamped into the exe on Windows.
  if (!icon.empty()) {
#if defined(_WIN32)
    if (!stamp_exe_icon(r.exe_path, (proj / icon).string(), &err)) {
      r.report.warnings.push_back("icon: " + err);
    } else {
      r.report.notes.push_back("icon stamped into " + exe_name);
    }
#else
    r.report.notes.push_back("icon " + icon + " used as the window icon");
#endif
  }

  // Plugins the project enables that live in DLLs.
  std::vector<std::string> extra;
  if (!broot.empty()) extra.push_back((broot / "plugins").string());
  for (const PluginNeed& p : project_plugins(proj.string(), extra)) {
    if (!p.dynamic || p.library.empty() || p.editor_only) continue;
    const fs::path lib(p.library);
    if (!copy_into(lib, game_dir / "plugins" / lib.filename(), &err)) return fail(err);
    r.plugins_shipped.push_back(p.id + " -> plugins/" + lib.filename().string());
  }

  // Manifest.
  std::vector<std::pair<std::string, std::string>> gs = {
      {"name", name},           {"version", version},  {"dimension", dimension},
      {"start_scene", "scene.json"}, {"icon", icon},   {"pak", "game.tspak"},
      {"profile", profile},     {"engine", "TombStone Engine"},
      {"vr", vr ? "true" : "false"}};
  if (!write_all(game_dir / "game.json",
                 json_num_object(gs, {{"window_width", ww}, {"window_height", wh}}))) {
    return fail("cannot write game.json");
  }

  // Launchers.
#if !defined(_WIN32)
  {
    const std::string sh =
        "#!/bin/sh\n# " + name + " " + version + " (TombStone Engine export)\n"
        "HERE=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
        "export LD_LIBRARY_PATH=\"$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n"
        "exec \"$HERE/" + exe_name + "\"" + (dev ? " --log \"$HERE/telegraph.log\"" : "") + " \"$@\"\n";
    const fs::path p = game_dir / (slug + ".sh");
    write_all(p, sh);
    fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                           fs::perms::others_read | fs::perms::others_exec, ec);
  }
#else
  if (dev) {
    write_all(game_dir / (slug + "-dev.bat"),
              "@echo off\r\n\"%~dp0" + exe_name + "\" --log \"%~dp0telegraph.log\" %*\r\n");
  }
#endif

  // Dedicated server.
  if (opt.server) {
    const std::string sexe = opt.server_exe.empty() ? find_server(game_exe) : opt.server_exe;
    if (sexe.empty() || !fs::is_regular_file(sexe, ec)) {
      r.report.warnings.push_back("ts_server not found next to the runtime; server package skipped");
    } else {
      const fs::path sdir = game_dir / "server";
      const fs::path sbin = sdir / (std::string("ts_server") + kExeExt);
      if (!copy_into(sexe, sbin, &err)) return fail(err);
      fs::permissions(sbin, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                      fs::perm_options::add, ec);
      for (const std::string& rel : content_files(proj.string())) {
        if (!copy_into(proj / rel, sdir / "content" / rel, &err)) return fail(err);
      }
#if defined(_WIN32)
      write_all(sdir / "run_server.bat",
                "@echo off\r\n\"%~dp0ts_server.exe\" --project \"%~dp0content\" %*\r\n");
#else
      const fs::path rs = sdir / "run_server.sh";
      write_all(rs, "#!/bin/sh\nHERE=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
                    "exec \"$HERE/ts_server\" --project \"$HERE/content\" \"$@\"\n");
      fs::permissions(rs, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec, ec);
#endif
      r.report.notes.push_back("dedicated server packaged in server/ (loose content; ts_server has no pak mount)");
    }
  }

  // README + report.
  std::ostringstream readme;
  readme << name << " " << version << "\nBuilt with TombStone Engine (" << profile << " export).\n\n"
         << "Run: " << exe_name
#if !defined(_WIN32)
         << " (or ./" << slug << ".sh, which sets LD_LIBRARY_PATH for lib/)"
#endif
         << "\nSelf-check: " << exe_name << " --smoke\n"
         << "Content: game.tspak (mounted through ts_vfs into a per-hash cache;\n"
         << "set TS_PAK_CACHE to choose the folder).\n"
         << "\nWindows: the runtime links the MSVC runtime dynamically (/MD). On a PC\n"
         << "without Visual Studio install the Microsoft Visual C++ 2015-2022\n"
         << "Redistributable (x64): https://aka.ms/vs/17/release/vc_redist.x64.exe\n";
  if (vr) {
    readme << "\nVR: rides in a headset through an OpenXR runtime (SteamVR / Oculus /\n"
           << "WMR). Without a headset it starts on the desktop (flat 3D); --sim\n"
           << "gives the VR desktop simulator, --desktop forces the flat ride.\n";
  }
  write_all(game_dir / "README.txt", readme.str());
  r.ok = true;

  if (opt.zip) {
    r.zip_path = (fs::absolute(fs::path(opt.out_dir), ec) / (slug + "-" + version + ".zip")).string();
    write_all(game_dir / "export_report.txt", r.text());
    say("Zipping " + r.zip_path);
    if (!zip_folder(game_dir.string(), slug, r.zip_path, &err)) {
      r.report.warnings.push_back(err);
      r.zip_path.clear();
    }
  }
  write_all(game_dir / "export_report.txt", r.text());
  say("Shipped: " + game_dir.string());
  return true;
}

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
