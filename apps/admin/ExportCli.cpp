#include "ExportCli.h"

#include "editor/launch/GameLauncher.h"
#include "editor/projects/ProjectStore.h"
#include "editor/templates/TemplateStore.h"
#include "editor/workspace/SceneIO.h"
#include "editor/workspace/Workspace2D.h"
#include "scene/SampleRider.h"
#include "scene/SampleScripts.h"
#include "scene/SceneJson.h"
#include "export/Exporter.h"
#include "export/Installer.h"
#include "vfs/Pak.h"
#include "vfs/Vfs.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace ts {
namespace tombstone {

namespace fs = std::filesystem;

namespace {

constexpr const char* kExportUsage =
    "Usage: ts_admin --export <project dir | sample:<id>> --out <dir>\n"
    "                [--profile shipping|dev] [--server] [--zip]\n"
    "                [--runtime <path to ts_game>] [--name N] [--version V]\n"
    "                [--installer] [--publisher P] [--license <file>]\n"
    "                [--install-mode ask|user|all] [--icon <png>] [--no-desktop-icon]\n"
    "  The project's export_settings.json (Freight Office) is applied first;\n"
    "  flags override it.\n"
    "  samples: sample-2d-platformer ghost-town-3d haunted-saloon-vr\n"
    "           frontier-survival sky-pirates\n";

const char* kSamples[] = {"sample-2d-platformer", "ghost-town-3d", "haunted-saloon-vr",
                          "frontier-survival", "sky-pirates"};

// What the editor does the first time a 2D claim opens: rider sheet,
// sample scripts and the starter scene.json (the 2D sample ships as just
// project.json until then).
void prime_2d(const std::string& dir) {
  std::error_code ec;
  if (fs::exists(fs::path(dir) / "scene.json", ec)) return;
  sample_rider::write_into_project(dir);
  sample_scripts::write_into_project(dir);
  editor::Workspace2D ws;
  ws.reset_defaults();
  editor::scene_io::save(ws, (fs::path(dir) / "scene.json").string());
}

// Seeds every sample into <temp>/ts_export_samples once and returns the dir.
std::string sample_dir(const std::string& id, std::string* err) {
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "ts_export_samples";
  if (!fs::exists(root / id / "project.json", ec)) {
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    // The samples are template generators now (nothing seeds a root).
    for (const std::string& gen : editor::TemplateStore::generator_ids()) {
      std::string gen_err;
      const std::string name = gen == "sample-2d-platformer" ? "Sample 2D Platformer"
                               : gen == "ghost-town-3d"      ? "Ghost Town 3D"
                               : gen == "haunted-saloon-vr"  ? "Haunted Saloon VR"
                               : gen == "frontier-survival"  ? "Frontier Survival"
                                                             : "Sky Pirates";
      if (!editor::TemplateStore::run_generator(gen, (root / gen).string(), name, &gen_err)) {
        *err = "could not write sample " + gen + ": " + gen_err;
        return {};
      }
    }
  }
  if (!fs::exists(root / id / "project.json", ec)) {
    *err = "no sample " + id;
    return {};
  }
  if (id == "sample-2d-platformer") prime_2d((root / id).string());
  return (root / id).string();
}

void set_env(const char* k, const std::string& v) {
#if defined(_WIN32)
  _putenv_s(k, v.c_str());
#else
  setenv(k, v.c_str(), 1);
#endif
}

}  // namespace

std::string export_sample_dir(const std::string& id, std::string* err) { return sample_dir(id, err); }

int run_export_cli(int argc, char** argv) {
  exporter::ExportOptions opt;
  std::string project;
  // Pass 1: the project, so its saved settings sit under the flags.
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--export") project = argv[i + 1];
  }
  {
    std::string err;
    if (project.rfind("sample:", 0) == 0) project = sample_dir(project.substr(7), &err);
    std::error_code ec;
    if (!project.empty() && fs::exists(fs::path(project) / exporter::kSettingsFile, ec)) {
      opt.project_dir = project;
      exporter::apply_project_settings(&opt);
    }
  }
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string* v) {
      if (i + 1 >= argc) return false;
      *v = argv[++i];
      return true;
    };
    bool ok = true;
    std::string ignored;
    if (a == "--export") ok = next(&ignored);
    else if (a == "--out") ok = next(&opt.out_dir);
    else if (a == "--profile") ok = next(&opt.profile);
    else if (a == "--runtime") ok = next(&opt.runtime_exe);
    else if (a == "--name") ok = next(&opt.name);
    else if (a == "--version") ok = next(&opt.version);
    else if (a == "--server") opt.server = true;
    else if (a == "--zip") opt.zip = true;
    else if (a == "--installer") opt.installer = true;
    else if (a == "--publisher") ok = next(&opt.publisher);
    else if (a == "--license") ok = next(&opt.license);
    else if (a == "--install-mode") ok = next(&opt.install_mode);
    else if (a == "--icon") ok = next(&opt.icon);
    else if (a == "--no-desktop-icon") opt.desktop_icon = false;
    else if (a == "--headless") {}
    else ok = false;
    if (!ok) {
      std::cerr << "Unknown or incomplete argument: " << a << '\n' << kExportUsage;
      return 2;
    }
  }
  if (project.empty() || opt.out_dir.empty() ||
      (opt.profile != "shipping" && opt.profile != "dev")) {
    std::cerr << kExportUsage;
    return 2;
  }
  if (opt.install_mode != "ask" && opt.install_mode != "user" && opt.install_mode != "all") {
    std::cerr << kExportUsage;
    return 2;
  }
  if (project.empty()) {
    std::cerr << "no such sample / project\n";
    return 1;
  }
  if (!opt.license.empty() && fs::path(opt.license).is_relative() &&
      !fs::exists(fs::path(project) / opt.license)) {
    opt.license = fs::absolute(opt.license).string();  // relative to the cwd
  }
  opt.project_dir = project;
  if (opt.runtime_exe.empty()) opt.runtime_exe = editor::launcher::find_game_executable();
  exporter::ExportResult res;
  exporter::export_project(opt, &res, [](const std::string& s) {
    std::cout << "[freight] " << s << '\n';
  });
  std::cout << res.text();
  return res.ok ? 0 : 1;
}

int run_export_smoke() {
  int failures = 0;
  auto fail = [&](const std::string& what) {
    std::cerr << "[smoke-export] FAIL: " << what << '\n';
    ++failures;
  };
  std::error_code ec;
  const std::string runtime = editor::launcher::find_game_executable();
  if (runtime.empty()) {
    std::cerr << "[smoke-export] FAIL: ts_game not built\n";
    return 1;
  }
  const fs::path out = fs::temp_directory_path(ec) / "ts_export_smoke";
  fs::remove_all(out, ec);
  set_env("TS_PAK_CACHE", (out / "pak-cache").string());

  // Pak round trip + vfs fallback.
  {
    vfs::PakWriter w;
    w.add("a/b.txt", "hello");
    w.add("c.bin", std::string("\0\1\2", 3));
    std::string err;
    std::uint64_t h = 0;
    const std::string p = (out / "t.tspak").string();
    fs::create_directories(out / "loose", ec);
    std::ofstream(out / "loose" / "only_loose.txt") << "loose";
    vfs::Vfs v;
    std::string got;
    if (!w.write(p, &h, &err) || !v.mount_pak(p, &err)) fail("pak write/mount: " + err);
    v.add_loose_root((out / "loose").string());
    if (!v.read("a\\b.txt", &got) || got != "hello") fail("pak read");
    if (!v.read("only_loose.txt", &got) || got != "loose") fail("loose fallback");
    if (v.exists("../c.bin") || !v.exists("c.bin")) fail("path normalisation");
    if (v.list().size() != 3) fail("vfs list");
  }

  for (const char* id : kSamples) {
    std::string err;
    const std::string dir = sample_dir(id, &err);
    if (dir.empty()) {
      fail(err);
      continue;
    }
    exporter::ExportOptions opt;
    opt.project_dir = dir;
    opt.out_dir = out.string();
    opt.runtime_exe = runtime;
      opt.zip = std::string(id) == "sample-2d-platformer";
    opt.server = std::string(id) == "frontier-survival";
    exporter::ExportResult res;
    if (!exporter::export_project(opt, &res)) {
      fail(std::string(id) + " export: " + res.text());
      continue;
    }
    // Pak only: no loose content may sit in the export folder.
    for (const char* loose : {"project.json", "scene.json", "scripts", "assets", "ui"}) {
      if (fs::exists(fs::path(res.game_dir) / loose, ec)) {
        fail(std::string(id) + ": loose " + loose + " in the export");
      }
    }
    if (opt.zip && (res.zip_path.empty() || !fs::exists(res.zip_path, ec))) fail("zip missing");
    if (opt.server && !fs::exists(fs::path(res.game_dir) / "server", ec)) fail("server package missing");
    // Run it from another folder so nothing resolves relative to the cwd.
    std::string cmd = "\"" + res.exe_path + "\" --smoke";
#if defined(_WIN32)
    cmd = "\"" + cmd + " > \"" + (fs::path(res.game_dir) / "smoke.log").string() + "\" 2>&1\"";
#else
    cmd = "cd / && LD_LIBRARY_PATH=\"" + res.game_dir + "/lib\" " + cmd + " > \"" +
          res.game_dir + "/smoke.log\" 2>&1";
#endif
    const int rc = std::system(cmd.c_str());
    std::cout << "[smoke-export] " << id << ": " << res.pak_files << " files in pak (hash "
              << vfs::hex64(res.pak_hash) << "), " << res.report.warnings.size()
              << " warning(s); exported game --smoke rc=" << rc << '\n';
    if (rc != 0) {
      fail(std::string(id) + ": exported ts_game --smoke failed (see " + res.game_dir + "/smoke.log)");
    } else {
      fs::remove(fs::path(res.game_dir) / "smoke.log", ec);
    }
  }
  std::cout << (failures ? "[smoke-export] FAILED\n" : "[smoke-export] OK (5 samples shipped + rode from pak)\n");
  return failures ? 1 : 0;
}

namespace {

// Runs a command with stdout/stderr to log; on Windows the PATH is cut to
// the Windows folders so nothing resolves from the dev machine.
int run_clean(const std::string& cmdline, const std::string& cwd, const std::string& log) {
#if defined(_WIN32)
  std::string sysroot = "C:\\Windows";
  if (const char* r = std::getenv("SystemRoot")) sysroot = r;
  const std::string cmd = "\"cd /d \"" + cwd + "\" && set \"PATH=" + sysroot + "\\System32;" + sysroot +
                          "\" && " + cmdline + " > \"" + log + "\" 2>&1\"";
#else
  const std::string cmd = "cd \"" + cwd + "\" && " + cmdline + " > \"" + log + "\" 2>&1";
#endif
  return std::system(cmd.c_str());
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

#if defined(_WIN32)
// The shell's real folder (OneDrive-redirected Desktop included).
fs::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = fs::path(p);
  CoTaskMemFree(p);
  return out;
}
#endif

int run_installer_smoke() {
  int failures = 0;
  auto fail = [&](const std::string& what) {
    std::cerr << "[smoke-installer] FAIL: " << what << '\n';
    ++failures;
  };
  std::error_code ec;
  const std::string runtime = editor::launcher::find_game_executable();
  if (runtime.empty()) {
    std::cerr << "[smoke-installer] FAIL: ts_game not built\n";
    return 1;
  }
  const fs::path out = fs::temp_directory_path(ec) / "ts_installer_smoke";
  fs::remove_all(out, ec);
  fs::create_directories(out, ec);
  set_env("TS_PAK_CACHE", (out / "pak-cache").string());
  set_env("TS_NO_DIALOGS", "1");
  std::string err;
  const std::string dir = sample_dir("ghost-town-3d", &err);
  if (dir.empty()) {
    std::cerr << "[smoke-installer] FAIL: " << err << '\n';
    return 1;
  }

  // Settings round trip (what the Freight Office saves in the project).
  {
    const fs::path tmp = out / "settings-project";
    fs::create_directories(tmp, ec);
    exporter::ExportSettings s;
    s.version = "2.4.1";
    s.publisher = "Dusty \"Quotes\" & Co";
    s.license = "EULA.txt";
    s.install_mode = "all";
    s.installer = true;
    s.desktop_icon = false;
    if (!exporter::save_settings(tmp.string(), s, &err)) fail(err);
    const exporter::ExportSettings b = exporter::load_settings(tmp.string());
    if (b.version != s.version || b.publisher != s.publisher || b.license != s.license ||
        b.install_mode != "all" || !b.installer || b.desktop_icon || !b.portable_zip) {
      fail("export_settings.json round trip");
    }
    if (!exporter::is_editor_only(exporter::kSettingsFile)) fail("export_settings.json would ship in the pak");
  }

  // Script text (checked everywhere, ISCC or not).
  {
    exporter::InstallerSpec sp;
    sp.game_dir = "C:/x/ghost-town-3d";
    sp.out_dir = "C:/x";
    sp.name = "Ghost {Town}";
    sp.version = "1.2.3-beta";
    sp.publisher = "Brenden";
    sp.exe_name = "ghost.exe";
    sp.slug = "ghost";
    sp.license_file = "C:/x/EULA.txt";
    const std::string t = exporter::inno_script(sp);
    for (const char* need : {"AppName=Ghost {{Town}", "AppPublisher=Brenden", "LicenseFile=",
                             "PrivilegesRequiredOverridesAllowed=dialog commandline",
                             "{autoprograms}", "{autodesktop}", "VersionInfoVersion=1.2.3",
                             "--check-gpu --quiet", "ArchitecturesInstallIn64BitMode=x64compatible"}) {
      if (t.find(need) == std::string::npos) fail(std::string("installer script lacks ") + need);
    }
  }

  // Export: portable zip + installer, EULA from outside the project.
  const fs::path eula = out / "EULA.txt";
  std::ofstream(eula) << "TombStone smoke EULA\r\nRide at your own risk.\r\n";
  exporter::ExportOptions opt;
  opt.project_dir = dir;
  opt.out_dir = (out / "depot").string();
  opt.runtime_exe = runtime;
  opt.zip = true;
  opt.installer = true;
  opt.publisher = "TombStone Smoke";
  opt.license = eula.string();
  opt.install_mode = "ask";
  exporter::ExportResult res;
  if (!exporter::export_project(opt, &res)) {
    std::cerr << "[smoke-installer] FAIL: export: " << res.text();
    return 1;
  }
  if (res.zip_path.empty() || !fs::exists(res.zip_path, ec)) fail("portable zip missing");
  if (!fs::exists(fs::path(res.game_dir) / "LICENSE.txt", ec)) fail("LICENSE.txt not shipped");
  if (res.iss_path.empty() || !fs::exists(res.iss_path, ec)) fail("installer script not written");
  if (!res.missing_dlls.empty()) fail("missing DLLs: " + res.missing_dlls.front());
#if defined(_WIN32)
  {
    bool crt = false;
    for (const auto& e : fs::directory_iterator(res.game_dir, ec)) {
      if (e.path().filename().string().rfind("vcruntime140", 0) == 0) crt = true;
    }
    if (!crt) fail("app-local VC++ runtime (vcruntime140.dll) not shipped");
  }
#endif

  // Clean folder: a copy far from the build tree, system-only PATH.
  {
    const fs::path clean = out / "clean-pc" / "game";
    fs::create_directories(clean.parent_path(), ec);
    fs::copy(res.game_dir, clean, fs::copy_options::recursive, ec);
    if (ec) fail("copy to clean folder: " + ec.message());
    const fs::path exe = clean / fs::path(res.exe_path).filename();
    const int rc = run_clean("\"" + exe.string() + "\" --smoke", out.string(), (out / "clean.log").string());
    std::cout << "[smoke-installer] clean-folder --smoke rc=" << rc << '\n';
    if (rc != 0) fail("clean-folder run failed: " + slurp(out / "clean.log"));
  }

  if (res.installer_skipped || res.installer_path.empty()) {
    std::cout << "[smoke-installer] SKIP installer build: Inno Setup (ISCC) not found\n";
    std::cout << (failures ? "[smoke-installer] FAILED\n"
                           : "[smoke-installer] OK (settings, script, zip, deps, clean run; installer SKIPPED)\n");
    return failures ? 1 : 0;
  }
  std::cout << "[smoke-installer] installer: " << res.installer_path << " ("
            << fs::file_size(res.installer_path, ec) << " bytes)\n";
#if defined(_WIN32)
  const fs::path inst = out / "installed";
  const std::string exe_name = fs::path(res.exe_path).filename().string();
  const int irc = run_clean("\"" + res.installer_path + "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /DIR=\"" +
                                inst.string() + "\" /MERGETASKS=\"desktopicon\" /LOG=\"" + (out / "setup.log").string() + "\"",
                            out.string(), (out / "setup.out").string());
  std::cout << "[smoke-installer] silent install rc=" << irc << '\n';
  if (irc != 0 || !fs::exists(inst / exe_name, ec)) fail("silent install (see " + (out / "setup.log").string() + ")");
  const std::string game_name = "Ghost Town 3D";
  fs::path start_lnk, desk_lnk;
  if (const fs::path p = known_folder(FOLDERID_Programs); !p.empty()) start_lnk = p / (game_name + ".lnk");
  if (const fs::path d = known_folder(FOLDERID_Desktop); !d.empty()) desk_lnk = d / (game_name + ".lnk");
  if (start_lnk.empty() || !fs::exists(start_lnk, ec)) fail("Start menu shortcut missing: " + start_lnk.string());
  if (desk_lnk.empty() || !fs::exists(desk_lnk, ec)) fail("desktop shortcut missing: " + desk_lnk.string());
  std::cout << "[smoke-installer] shortcuts: " << start_lnk.string() << " + " << desk_lnk.string() << '\n';
  const int grc = run_clean("\"" + (inst / exe_name).string() + "\" --smoke", out.string(), (out / "installed.log").string());
  std::cout << "[smoke-installer] installed game --smoke rc=" << grc << '\n';
  if (grc != 0) fail("installed game --smoke: " + slurp(out / "installed.log"));
  const int gpu = run_clean("\"" + (inst / exe_name).string() + "\" --check-gpu --quiet", out.string(), (out / "gpu.log").string());
  std::cout << "[smoke-installer] --check-gpu rc=" << gpu << " " << slurp(out / "gpu.log");
  const fs::path unins = inst / "unins000.exe";
  if (!fs::exists(unins, ec)) fail("uninstaller missing");
  const int urc = run_clean("\"" + unins.string() + "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART", out.string(),
                            (out / "uninstall.out").string());
  // The uninstaller re-launches itself from %TEMP%; wait for it to finish.
  bool gone = false;
  for (int i = 0; i < 120 && !gone; ++i) {
    gone = !fs::exists(inst / exe_name, ec) && !fs::exists(unins, ec);
    if (!gone) std::system("ping -n 2 127.0.0.1 > nul");
  }
  std::cout << "[smoke-installer] uninstall rc=" << urc << (gone ? ", files removed" : ", files LEFT") << '\n';
  if (!gone) fail("uninstall left files in " + inst.string());
  if (!start_lnk.empty() && fs::exists(start_lnk, ec)) fail("uninstall left the Start menu shortcut");
  if (!desk_lnk.empty() && fs::exists(desk_lnk, ec)) fail("uninstall left the desktop shortcut");
#endif
  std::cout << (failures ? "[smoke-installer] FAILED\n"
                         : "[smoke-installer] OK (settings, script, zip, deps, clean run, install / --smoke / uninstall)\n");
  return failures ? 1 : 0;
}

}  // namespace tombstone
}  // namespace ts
