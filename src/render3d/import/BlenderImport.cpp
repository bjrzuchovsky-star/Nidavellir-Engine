// .blend import through Blender itself: Blender runs headless (-b) with a
// tiny export script writing a .glb into a cache folder (keyed by the
// file's path, size and time), then the glTF loader reads that. Assimp's
// own .blend reader is not used (it misses most modern files).
#include "render3d/ModelImport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;

namespace {

bool is_file(const fs::path& p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}

std::string env(const char* name) {
#ifdef _WIN32
  wchar_t buf[32768];
  std::wstring w(name, name + std::char_traits<char>::length(name));
  const DWORD n = GetEnvironmentVariableW(w.c_str(), buf, 32768);
  if (n == 0 || n >= 32768) return {};
  return fs::path(std::wstring(buf, n)).string();
#else
  const char* v = std::getenv(name);
  return v ? v : "";
#endif
}

#ifdef _WIN32
std::string reg_string(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[2048];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
    return {};
  }
  return fs::path(std::wstring(buf)).string();
}

// "C:\...\blender-launcher.exe" "%1" -> the blender.exe beside it.
std::string exe_from_command(const std::string& cmd) {
  std::string exe;
  if (!cmd.empty() && cmd[0] == '"') {
    const std::size_t q = cmd.find('"', 1);
    exe = cmd.substr(1, q == std::string::npos ? std::string::npos : q - 1);
  } else {
    exe = cmd.substr(0, cmd.find(' '));
  }
  fs::path p = fs::u8path(exe);
  if (is_file(p.parent_path() / "blender.exe")) return (p.parent_path() / "blender.exe").string();
  return is_file(p) ? p.string() : std::string();
}
#endif

std::uint64_t fnv(const std::string& s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

// Runs exe with args, waits (up to timeout). Returns the exit code, -1 when
// it could not start or timed out.
int run_process(const std::string& exe, const std::vector<std::string>& args, int timeout_s) {
#ifdef _WIN32
  auto quote = [](const std::wstring& a) {
    std::wstring q = L"\"";
    for (wchar_t c : a) {
      if (c == L'"') q += L'\\';
      q += c;
    }
    if (!q.empty() && q.back() == L'\\') q += L'\\';
    return q + L"\"";
  };
  std::wstring cmd = quote(fs::u8path(exe).wstring());
  for (const std::string& a : args) cmd += L" " + quote(fs::u8path(a).wstring());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> line(cmd.begin(), cmd.end());
  line.push_back(0);
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  const DWORD w = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_s) * 1000);
  DWORD code = static_cast<DWORD>(-1);
  if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return w == WAIT_OBJECT_0 ? static_cast<int>(code) : -1;
#else
  std::vector<std::string> all = {exe};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (std::string& a : all) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return -1;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  int status = 0;
  for (;;) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (r < 0) return -1;
    if (std::chrono::steady_clock::now() > end) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    usleep(50 * 1000);
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace

std::string find_blender(const std::string& override_path) {
  if (!override_path.empty()) {
    fs::path p = fs::u8path(override_path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
#ifdef _WIN32
      p /= "blender.exe";
#else
      p /= "blender";
#endif
    }
    return is_file(p) ? p.string() : std::string();
  }
  if (const std::string e = env("TS_BLENDER"); is_file(fs::u8path(e))) return e;
#ifdef _WIN32
  const char sep = ';';
  const char* names[] = {"blender.exe"};
#else
  const char sep = ':';
  const char* names[] = {"blender"};
#endif
  {
    const std::string path = env("PATH");
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = std::min(path.find(sep, start), path.size());
      const std::string dir = path.substr(start, end - start);
      for (const char* n : names) {
        if (!dir.empty() && is_file(fs::u8path(dir) / n)) return (fs::u8path(dir) / n).string();
      }
      start = end + 1;
    }
  }
#ifdef _WIN32
  for (const std::string& cmd : {reg_string(HKEY_CLASSES_ROOT, L"blendfile\\shell\\open\\command", nullptr),
                                 reg_string(HKEY_CURRENT_USER, L"Software\\Classes\\blendfile\\shell\\open\\command", nullptr)}) {
    if (const std::string exe = exe_from_command(cmd); !exe.empty()) return exe;
  }
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    const std::string dir = reg_string(root, L"SOFTWARE\\BlenderFoundation", L"Install_Dir");
    if (!dir.empty() && is_file(fs::u8path(dir) / "blender.exe")) return (fs::u8path(dir) / "blender.exe").string();
  }
  std::vector<fs::path> found;
  for (const char* pf : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
    const std::string base = env(pf);
    if (base.empty()) continue;
    const fs::path foundation = fs::u8path(base) / "Blender Foundation";
    std::error_code ec;
    if (fs::is_directory(foundation, ec)) {
      for (const auto& it : fs::directory_iterator(foundation, ec)) {
        if (is_file(it.path() / "blender.exe")) found.push_back(it.path() / "blender.exe");
      }
    }
    const fs::path steam = fs::u8path(base) / "Steam" / "steamapps" / "common" / "Blender" / "blender.exe";
    if (is_file(steam)) found.push_back(steam);
  }
  if (!found.empty()) {
    // "Blender 4.2" sorts after "Blender 3.6": take the newest.
    std::sort(found.begin(), found.end());
    return found.back().string();
  }
#elif defined(__APPLE__)
  for (const char* p : {"/Applications/Blender.app/Contents/MacOS/Blender"}) {
    if (is_file(p)) return p;
  }
#else
  for (const char* p : {"/usr/bin/blender", "/usr/local/bin/blender", "/snap/bin/blender"}) {
    if (is_file(p)) return p;
  }
#endif
  return {};
}

bool load_blend_model(const std::string& path, const ModelImportOptions& opt, Model* out,
                      std::string* error) {
  const std::string blender = find_blender(opt.blender_path);
  if (blender.empty()) {
    if (error) {
      *error = fs::u8path(path).filename().string() +
               ": Blender was not found. Install Blender (blender.org) or export the file to glTF / FBX"
               " (you can also set the Blender path in the import settings).";
    }
    return false;
  }
  std::error_code ec;
  const fs::path src = fs::u8path(path);
  const auto size = fs::file_size(src, ec);
  const auto time = fs::last_write_time(src, ec).time_since_epoch().count();
  const fs::path cache = fs::temp_directory_path(ec) / "tombstone_blend";
  fs::create_directories(cache, ec);
  char key[32];
  std::snprintf(key, sizeof(key), "%016llx",
                static_cast<unsigned long long>(fnv(fs::absolute(src, ec).string() + "|" + std::to_string(size) + "|" +
                                                    std::to_string(static_cast<long long>(time)))));
  const fs::path glb = cache / (std::string(key) + ".glb");
  if (!is_file(glb)) {
    const fs::path script = cache / "ts_export_gltf.py";
    {
      std::ofstream py(script, std::ios::binary | std::ios::trunc);
      py << "import bpy, sys\n"
            "out = sys.argv[sys.argv.index('--') + 1]\n"
            "bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_animations=True,"
            " export_apply=True, export_yup=True)\n";
    }
    const int rc = run_process(blender, {"-b", src.string(), "--python", script.string(), "--", glb.string()}, 300);
    if (rc != 0 || !is_file(glb)) {
      if (error) *error = fs::u8path(path).filename().string() + ": Blender (" + blender + ") could not export it to glTF (exit " + std::to_string(rc) + ")";
      fs::remove(glb, ec);
      return false;
    }
  }
  if (!load_gltf(glb.string(), out, error)) return false;
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
