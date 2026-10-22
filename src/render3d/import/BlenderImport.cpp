// .blend import through Blender itself: Blender runs headless (-b) with a
// tiny export script writing a .glb into a cache folder (keyed by the
// file's path, size and time), then the glTF loader reads that. Assimp's
// own .blend reader is not used (it misses most modern files).
#include "render3d/ModelImport.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
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

// Paths travel as UTF-8 (fs::u8path in, u8 out). path::string() and
// wide -> narrow conversions use the ANSI code page on Windows and throw on
// characters it lacks (a "™" in an Uninstall entry's name was enough).
std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

#ifdef _WIN32
std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}
#endif

std::string env(const char* name) {
#ifdef _WIN32
  wchar_t buf[32768];
  std::wstring w(name, name + std::char_traits<char>::length(name));
  const DWORD n = GetEnvironmentVariableW(w.c_str(), buf, 32768);
  if (n == 0 || n >= 32768) return {};
  return utf8(std::wstring(buf, n));
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
  return utf8(std::wstring(buf));
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
  if (is_file(p.parent_path() / "blender.exe")) return u8(p.parent_path() / "blender.exe");
  return is_file(p) ? u8(p) : std::string();
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

std::vector<std::string> steam_library_folders_from_vdf(const std::string& text) {
  // Tokens: quoted strings (backslash escapes), { and }.
  struct Tok {
    int kind;  // 0 string, 1 open, 2 close
    std::string s;
  };
  std::vector<Tok> toks;
  for (std::size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '"') {
      std::string v;
      ++i;
      while (i < text.size() && text[i] != '"') {
        if (text[i] == '\\' && i + 1 < text.size()) {
          const char e = text[i + 1];
          v += e == 'n' ? '\n' : e == 't' ? '\t' : e;
          i += 2;
        } else {
          v += text[i++];
        }
      }
      ++i;
      toks.push_back({0, v});
    } else if (c == '{') {
      toks.push_back({1, {}});
      ++i;
    } else if (c == '}') {
      toks.push_back({2, {}});
      ++i;
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;  // comment
    } else {
      ++i;
    }
  }
  std::vector<std::string> out;
  auto add = [&](std::string p) {
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    if (!p.empty() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  };
  for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
    if (toks[i].kind != 0 || toks[i + 1].kind != 0) continue;
    const std::string& k = toks[i].s;
    const std::string& v = toks[i + 1].s;
    std::string kl = k;
    std::transform(kl.begin(), kl.end(), kl.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool digits = !k.empty() && std::all_of(k.begin(), k.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
    // New layout: "path" inside each numbered block. Old layout: the
    // numbered key's value is the folder itself.
    if (kl == "path" || (digits && (v.find(':') != std::string::npos || (!v.empty() && v[0] == '/')))) add(v);
    ++i;  // the value is consumed
  }
  return out;
}

namespace {
std::string& blender_setting_ref() {
  static std::string s;
  return s;
}
}  // namespace

void set_blender_setting(const std::string& path) { blender_setting_ref() = path; }
std::string blender_setting() { return blender_setting_ref(); }

namespace {

#ifdef _WIN32
std::vector<std::wstring> reg_subkeys(HKEY root, const wchar_t* key) {
  std::vector<std::wstring> out;
  HKEY h = nullptr;
  if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
  wchar_t name[512];
  for (DWORD i = 0;; ++i) {
    DWORD n = 512;
    if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    out.emplace_back(name, n);
  }
  RegCloseKey(h);
  return out;
}
#endif

// The executable for an override: a file, or a folder holding blender(.exe).
fs::path exe_in(const fs::path& p) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
#ifdef _WIN32
    return p / "blender.exe";
#else
    return p / "blender";
#endif
  }
  return p;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

static std::string find_blender_unguarded(const std::string& override_path, std::vector<std::string>* looked) {
  auto note = [&](const std::string& where) {
    if (looked) looked->push_back(where);
  };
  if (!override_path.empty()) {
    const fs::path p = exe_in(fs::u8path(override_path));
    note("asset Blender path (" + u8(p) + ")");
    return is_file(p) ? u8(p) : std::string();  // an explicit path is final
  }
  if (const std::string e = env("TS_BLENDER"); !e.empty()) {
    const fs::path p = exe_in(fs::u8path(e));
    note("TS_BLENDER (" + u8(p) + ")");
    if (is_file(p)) return u8(p);
  } else {
    note("TS_BLENDER (not set)");
  }
  if (const std::string st = blender_setting(); !st.empty()) {
    const fs::path p = exe_in(fs::u8path(st));
    note("Settings > Blender executable (" + u8(p) + ")");
    if (is_file(p)) return u8(p);
  } else {
    note("Settings > Blender executable (not set)");
  }
#ifdef _WIN32
  const char sep = ';';
  const char* names[] = {"blender.exe"};
#else
  const char sep = ':';
  const char* names[] = {"blender"};
#endif
  {
    note("PATH");
    const std::string path = env("PATH");
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = std::min(path.find(sep, start), path.size());
      const std::string dir = path.substr(start, end - start);
      for (const char* n : names) {
        if (!dir.empty() && is_file(fs::u8path(dir) / n)) return u8(fs::u8path(dir) / n);
      }
      start = end + 1;
    }
  }
  std::vector<fs::path> steam_roots;
  const std::string home = env("HOME");
#ifdef _WIN32
  note("registry (.blend open command, BlenderFoundation, Uninstall)");
  for (const std::string& cmd : {reg_string(HKEY_CLASSES_ROOT, L"blendfile\\shell\\open\\command", nullptr),
                                 reg_string(HKEY_CURRENT_USER, L"Software\\Classes\\blendfile\\shell\\open\\command", nullptr),
                                 reg_string(HKEY_CLASSES_ROOT, L"Applications\\blender.exe\\shell\\open\\command", nullptr)}) {
    if (const std::string exe = exe_from_command(cmd); !exe.empty()) return exe;
  }
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    for (const wchar_t* key : {L"SOFTWARE\\BlenderFoundation", L"SOFTWARE\\Blender Foundation",
                               L"SOFTWARE\\WOW6432Node\\BlenderFoundation"}) {
      const std::string dir = reg_string(root, key, L"Install_Dir");
      if (!dir.empty() && is_file(fs::u8path(dir) / "blender.exe")) return u8(fs::u8path(dir) / "blender.exe");
    }
    // Blender Foundation subkeys ("Blender Foundation\Blender 4.2" -> InstallDir / Install_Dir).
    for (const std::wstring& sub : reg_subkeys(root, L"SOFTWARE\\Blender Foundation")) {
      const std::wstring k = L"SOFTWARE\\Blender Foundation\\" + sub;
      for (const wchar_t* v : {L"Install_Dir", L"InstallDir", L"InstallLocation"}) {
        const std::string dir = reg_string(root, k.c_str(), v);
        if (!dir.empty() && is_file(fs::u8path(dir) / "blender.exe")) return u8(fs::u8path(dir) / "blender.exe");
      }
    }
    // Add / remove programs entries (MSI, winget, Steam's "Steam App 365670").
    for (const wchar_t* un : {L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                              L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"}) {
      for (const std::wstring& sub : reg_subkeys(root, un)) {
        const std::wstring k = std::wstring(un) + L"\\" + sub;
        std::string name = reg_string(root, k.c_str(), L"DisplayName");
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (name.find("blender") == std::string::npos) continue;
        const std::string dir = reg_string(root, k.c_str(), L"InstallLocation");
        if (!dir.empty() && is_file(fs::u8path(dir) / "blender.exe")) return u8(fs::u8path(dir) / "blender.exe");
        if (const std::string exe = exe_from_command(reg_string(root, k.c_str(), L"DisplayIcon")); !exe.empty()) return exe;
      }
    }
  }
  std::vector<fs::path> found;
  note("Program Files\\Blender Foundation\\*");
  for (const char* pf : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
    const std::string base = env(pf);
    if (base.empty()) continue;
    const fs::path foundation = fs::u8path(base) / "Blender Foundation";
    std::error_code ec;
    if (fs::is_directory(foundation, ec)) {
      for (fs::directory_iterator it(foundation, ec), end; !ec && it != end; it.increment(ec)) {
        if (is_file(it->path() / "blender.exe")) found.push_back(it->path() / "blender.exe");
      }
    }
    // Microsoft Store packages (when the folder is readable).
    const fs::path apps = fs::u8path(base) / "WindowsApps";
    if (fs::is_directory(apps, ec)) {
      for (fs::directory_iterator dit(apps, ec), end; !ec && dit != end; dit.increment(ec)) {
        const fs::directory_entry& it = *dit;
        const std::string n = u8(it.path().filename());
        if (n.rfind("BlenderFoundation.Blender", 0) == 0 && is_file(it.path() / "Blender" / "blender.exe")) {
          found.push_back(it.path() / "Blender" / "blender.exe");
        } else if (n.rfind("BlenderFoundation.Blender", 0) == 0 && is_file(it.path() / "blender.exe")) {
          found.push_back(it.path() / "blender.exe");
        }
      }
    }
    steam_roots.push_back(fs::u8path(base) / "Steam");
  }
  if (!found.empty()) {
    // "Blender 4.2" sorts after "Blender 3.6": take the newest.
    std::sort(found.begin(), found.end());
    return u8(found.back());
  }
  note("Microsoft Store / winget (%LOCALAPPDATA%\\Microsoft\\WindowsApps, WinGet\\Links, WinGet\\Packages)");
  if (const std::string la = env("LOCALAPPDATA"); !la.empty()) {
    const fs::path ms = fs::u8path(la) / "Microsoft";
    std::error_code ec;
    for (const fs::path& p : {ms / "WindowsApps" / "blender.exe", ms / "WinGet" / "Links" / "blender.exe"}) {
      // App execution aliases are reparse points: exists() rather than is_regular_file().
      if (fs::exists(p, ec)) return u8(p);
    }
    const fs::path pk = ms / "WinGet" / "Packages";
    if (fs::is_directory(pk, ec)) {
      for (fs::directory_iterator dit(pk, ec), end; !ec && dit != end; dit.increment(ec)) {
        const fs::directory_entry& it = *dit;
        if (u8(it.path().filename()).rfind("BlenderFoundation.Blender", 0) != 0) continue;
        for (auto r = fs::recursive_directory_iterator(it.path(), ec); r != fs::recursive_directory_iterator(); r.increment(ec)) {
          if (r.depth() > 3) r.disable_recursion_pending();
          if (r->path().filename() == "blender.exe" && is_file(r->path())) found.push_back(r->path());
        }
      }
    }
    if (!found.empty()) {
      std::sort(found.begin(), found.end());
      return u8(found.back());
    }
  }
  for (const std::string& sp : {reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                                reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
                                reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath")}) {
    if (!sp.empty()) steam_roots.insert(steam_roots.begin(), fs::u8path(sp));
  }
  const char* steam_exe = "blender.exe";
#elif defined(__APPLE__)
  note("/Applications/Blender.app");
  for (const char* p : {"/Applications/Blender.app/Contents/MacOS/Blender"}) {
    if (is_file(p)) return p;
  }
  if (!home.empty()) {
    steam_roots.push_back(fs::u8path(home) / "Library" / "Application Support" / "Steam");
  }
  const char* steam_exe = "Blender.app/Contents/MacOS/Blender";
#else
  note("/usr/bin, /usr/local/bin, /snap/bin, flatpak");
  for (const char* p : {"/usr/bin/blender", "/usr/local/bin/blender", "/snap/bin/blender",
                        "/var/lib/flatpak/exports/bin/org.blender.Blender"}) {
    if (is_file(p)) return p;
  }
  if (!home.empty()) {
    steam_roots.push_back(fs::u8path(home) / ".steam" / "steam");
    steam_roots.push_back(fs::u8path(home) / ".local" / "share" / "Steam");
  }
  const char* steam_exe = "blender";
#endif
  // Steam: every library folder listed by each Steam install.
  std::vector<fs::path> libraries;
  for (const fs::path& root : steam_roots) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) continue;
    libraries.push_back(root);
    for (const fs::path& vdf : {root / "steamapps" / "libraryfolders.vdf", root / "config" / "libraryfolders.vdf"}) {
      for (const std::string& lib : steam_library_folders_from_vdf(read_text(vdf))) libraries.push_back(fs::u8path(lib));
    }
  }
  std::string libs;
  for (const fs::path& lib : libraries) {
    const fs::path exe = lib / "steamapps" / "common" / "Blender" / fs::u8path(steam_exe);
    libs += (libs.empty() ? "" : ", ") + u8(lib);
    if (is_file(exe)) return u8(exe);
  }
  note("Steam libraries" + (libs.empty() ? std::string(" (no Steam found)") : " (" + libs + ")"));
  return {};
}

std::string find_blender(const std::string& override_path, std::vector<std::string>* looked) {
  // Registry, environment and folder contents are outside our control: a
  // probe that throws must not take the editor down.
  try {
    return find_blender_unguarded(override_path, looked);
  } catch (const std::exception& e) {
    if (looked) looked->push_back(std::string("(search stopped: ") + e.what() + ")");
  }
  return {};
}

bool load_blend_model(const std::string& path, const ModelImportOptions& opt, Model* out,
                      std::string* error) {
  std::vector<std::string> looked;
  const std::string blender = find_blender(opt.blender_path, &looked);
  if (blender.empty()) {
    if (error) {
      std::string where;
      for (const std::string& l : looked) where += (where.empty() ? "" : "; ") + l;
      *error = u8(fs::u8path(path).filename()) +
               ": Blender was not found. Set Settings > Tools > Blender executable (or the TS_BLENDER"
               " environment variable, or this asset's Blender path) to your blender.exe, install Blender"
               " (blender.org, Steam, Microsoft Store), or export the file to glTF / FBX. Looked in: " + where;
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
                static_cast<unsigned long long>(fnv(u8(fs::absolute(src, ec)) + "|" + std::to_string(size) + "|" +
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
    const int rc = run_process(blender, {"-b", u8(src), "--python", u8(script), "--", u8(glb)}, 300);
    if (rc != 0 || !is_file(glb)) {
      if (error) *error = u8(fs::u8path(path).filename()) + ": Blender (" + blender + ") could not export it to glTF (exit " + std::to_string(rc) + ")";
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
