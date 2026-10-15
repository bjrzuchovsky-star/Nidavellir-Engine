#include "GameManifest.h"

#include "core/JsonMini.h"
#include "vfs/Vfs.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>
#include <stb_image.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ts {
namespace tombstone {
namespace game {

namespace fs = std::filesystem;

namespace {
GameManifest g_manifest;

fs::path exe_dir(const char* argv0) {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n > 0) return fs::path(std::wstring(buf, n)).parent_path();
#else
  std::error_code ec;
  const fs::path self = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) return self.parent_path();
#endif
  std::error_code ec2;
  return fs::absolute(fs::path(argv0 ? argv0 : "."), ec2).parent_path();
}

void add_plugin_path(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::string v = dir.string();
  if (const char* old = std::getenv("TS_PLUGIN_PATH"); old && *old) v += sep + std::string(old);
#if defined(_WIN32)
  _putenv_s("TS_PLUGIN_PATH", v.c_str());
#else
  setenv("TS_PLUGIN_PATH", v.c_str(), 1);
#endif
}
}  // namespace

const GameManifest& manifest() { return g_manifest; }

bool load_manifest(const std::string& dir, GameManifest* out, std::string* error) {
  std::ifstream in(fs::path(dir) / "game.json", std::ios::binary);
  if (!in) {
    if (error) *error = "no game.json in " + dir;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const auto obj = json_mini::parse_object(ss.str());
  if (!obj) {
    if (error) *error = "game.json in " + dir + " is not valid JSON";
    return false;
  }
  GameManifest m;
  m.loaded = true;
  m.dir = dir;
  m.name = json_mini::get_string(*obj, "name", "");
  m.version = json_mini::get_string(*obj, "version", "");
  m.dimension = json_mini::get_string(*obj, "dimension", "2d");
  m.start_scene = json_mini::get_string(*obj, "start_scene", "scene.json");
  m.icon = json_mini::get_string(*obj, "icon", "");
  m.pak = json_mini::get_string(*obj, "pak", "game.tspak");
  m.profile = json_mini::get_string(*obj, "profile", "");
  m.window_width = std::atoi(json_mini::get_string(*obj, "window_width", "1280").c_str());
  m.window_height = std::atoi(json_mini::get_string(*obj, "window_height", "720").c_str());
  if (m.window_width < 160) m.window_width = 1280;
  if (m.window_height < 120) m.window_height = 720;
  *out = std::move(m);
  return true;
}

bool resolve_launch(const char* argv0, std::string* project, std::string* error) {
  std::error_code ec;
  fs::path root = project->empty() ? exe_dir(argv0) : fs::path(*project);
  if (!fs::exists(root / "game.json", ec)) {
    if (project->empty()) {
      if (error) *error = "ts_game needs a project (no --project and no game.json next to the executable).";
      return false;
    }
    return true;  // plain project folder: loose files, as before
  }
  GameManifest m;
  if (!load_manifest(root.string(), &m, error)) return false;
  g_manifest = m;
  add_plugin_path(root / "plugins");
  const fs::path pak = root / m.pak;
  if (fs::is_regular_file(pak, ec)) {
    vfs::MountResult r;
    if (!vfs::mount_project_pak(pak.string(), vfs::default_cache_root(), &r, error)) return false;
    std::cout << "[vfs] mounted " << m.pak << " (" << r.files << " files, hash "
              << vfs::hex64(r.hash) << (r.reused ? ", cached" : "") << ") at " << r.dir << '\n';
    *project = r.dir;
  } else if (fs::exists(root / "project.json", ec)) {
    std::cout << "[vfs] no " << m.pak << "; riding loose files in " << root.string() << '\n';
    *project = root.string();
  } else {
    if (error) *error = "game.json names " + m.pak + " but it is missing (and no loose project.json)";
    return false;
  }
  if (m.start_scene != "scene.json") {
    std::cout << "[vfs] start_scene " << m.start_scene
              << " noted; this runtime starts scene.json\n";
  }
  return true;
}

int window_width() { return g_manifest.window_width; }
int window_height() { return g_manifest.window_height; }

void apply_window_icon(GLFWwindow* win, const std::string& project_dir) {
  if (!win || g_manifest.icon.empty()) return;
  const fs::path p = fs::path(project_dir) / g_manifest.icon;
  std::ifstream in(p, std::ios::binary);
  if (!in) return;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string bytes = ss.str();
  int w = 0, h = 0, n = 0;
  unsigned char* px = stbi_load_from_memory(
      reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()),
      &w, &h, &n, 4);
  if (!px) return;
  GLFWimage img{w, h, px};
  glfwSetWindowIcon(win, 1, &img);
  stbi_image_free(px);
}

}  // namespace game
}  // namespace tombstone
}  // namespace ts
