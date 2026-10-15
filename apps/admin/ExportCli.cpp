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
#include "vfs/Pak.h"
#include "vfs/Vfs.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {

namespace fs = std::filesystem;

namespace {

constexpr const char* kExportUsage =
    "Usage: ts_admin --export <project dir | sample:<id>> --out <dir>\n"
    "                [--profile shipping|dev] [--server] [--zip]\n"
    "                [--runtime <path to ts_game>] [--name N] [--version V]\n"
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
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string* v) {
      if (i + 1 >= argc) return false;
      *v = argv[++i];
      return true;
    };
    bool ok = true;
    if (a == "--export") ok = next(&project);
    else if (a == "--out") ok = next(&opt.out_dir);
    else if (a == "--profile") ok = next(&opt.profile);
    else if (a == "--runtime") ok = next(&opt.runtime_exe);
    else if (a == "--name") ok = next(&opt.name);
    else if (a == "--version") ok = next(&opt.version);
    else if (a == "--server") opt.server = true;
    else if (a == "--zip") opt.zip = true;
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
  std::string err;
  if (project.rfind("sample:", 0) == 0) {
    project = sample_dir(project.substr(7), &err);
    if (project.empty()) {
      std::cerr << err << '\n';
      return 1;
    }
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

}  // namespace tombstone
}  // namespace ts
