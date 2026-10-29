#include "runtime/NativeScript.h"
#include "core/AppPaths.h"

#include "runtime/ScriptAbi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <sstream>

#if !defined(TS_CMAKE_COMMAND)
#define TS_CMAKE_COMMAND "cmake"
#endif
#if !defined(TS_SCRIPT_SDK_DIR)
#define TS_SCRIPT_SDK_DIR ""
#endif

namespace ts {
namespace tombstone {
namespace runtime {

namespace fs = std::filesystem;

namespace native_scripts {

namespace {
fs::path native_dir(const std::string& project) { return fs::path(project) / ".tombstone" / "native"; }

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool write_if_changed(const fs::path& p, const std::string& text) {
  std::error_code ec;
  if (fs::exists(p, ec) && read_all(p) == text) return true;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o << text;
  return static_cast<bool>(o);
}
std::string q(const std::string& s) { return "\"" + s + "\""; }
std::string cmake_path(const fs::path& p) { return p.lexically_normal().generic_string(); }

std::string cmake_exe() {
  if (const char* e = std::getenv("TS_CMAKE"); e && *e) return e;
  std::error_code ec;
  if (fs::exists(TS_CMAKE_COMMAND, ec)) return TS_CMAKE_COMMAND;
  return "cmake";
}

fs::path sdk_dir() {
  std::error_code ec;
  if (const char* e = std::getenv("TS_SCRIPT_SDK"); e && *e) return e;
  // Installed engine: <exe dir>/sdk/include; build tree: the source sdk.
  if (const std::string exe = core::executable_dir(); !exe.empty()) {
    const fs::path installed = fs::path(exe) / "sdk" / "include";
    if (fs::exists(installed / "tombstone" / "script_sdk.h", ec)) return installed;
  }
  if (fs::exists(fs::path(TS_SCRIPT_SDK_DIR) / "tombstone" / "script_sdk.h", ec)) return TS_SCRIPT_SDK_DIR;
  return {};
}
}  // namespace

std::string sdk_include_dir() { return sdk_dir().string(); }

void set_blocking_builds(bool on) { set_compiled_scripts_blocking(on); }
bool blocking_builds() { return compiled_scripts_blocking(); }

std::string module_file_name() { return std::string("ts_scripts") + SharedLib::extension(); }
std::string prebuilt_module(const std::string& p) { return (fs::path(p) / "native" / module_file_name()).string(); }
std::string dev_module(const std::string& p) { return (native_dir(p) / "bin" / module_file_name()).string(); }

bool has_sources(const std::string& project) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(project) / "scripts", ec)) {
    if (e.path().extension() == ".cpp") return true;
  }
  return false;
}

std::int64_t sources_stamp(const std::string& project) {
  std::error_code ec;
  std::uint64_t h = 1469598103934665603ULL;
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(fs::path(project) / "scripts", ec)) {
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
    // (file clocks may sit before their epoch: hash, do not compare.)
    names.push_back(e.path().filename().string() + "|" +
                    std::to_string(static_cast<long long>(e.last_write_time(ec).time_since_epoch().count())) +
                    "|" + std::to_string(e.file_size(ec)));
  }
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    for (unsigned char c : n) h = (h ^ c) * 1099511628211ULL;
  }
  return static_cast<std::int64_t>(h & 0x7FFFFFFFFFFFFFFFULL);
}

bool toolchain_available(std::string* why) {
  std::string out;
  if (run_command(q(cmake_exe()) + " --version", &out) != 0) {
    if (why) *why = "cmake not found (install CMake 3.20+ or set TS_CMAKE)";
    return false;
  }
  if (sdk_dir().empty()) {
    if (why) *why = "TombStone script SDK headers not found (set TS_SCRIPT_SDK)";
    return false;
  }
#if defined(_WIN32)
  // C++ scripts build with the user's MSVC toolchain (VS 2022). CMake on its
  // own is not enough: without the C++ workload the module configure fails.
  const char* pf86 = std::getenv("ProgramFiles(x86)");
  const fs::path vswhere = fs::path(pf86 && *pf86 ? pf86 : "C:/Program Files (x86)") /
                           "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
  std::error_code ec;
  if (!fs::exists(vswhere, ec) ||
      run_command(q(vswhere.string()) +
                      " -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath",
                  &out) != 0 ||
      out.find_first_not_of(" \t\r\n") == std::string::npos) {
    if (why) {
      *why = "the Visual Studio 2022 C++ toolchain (MSVC) was not found. Install the "
             "\"Desktop development with C++\" workload from https://visualstudio.microsoft.com/downloads/ "
             "so CMake can build scripts/*.cpp";
    }
    return false;
  }
#else
  if (run_command("c++ --version", &out) != 0 && run_command("g++ --version", &out) != 0 &&
      run_command("clang++ --version", &out) != 0) {
    if (why) *why = "no C++ compiler (c++, g++ or clang++) is on PATH. C++ scripts need one, plus CMake";
    return false;
  }
#endif
  return true;
}

BuildResult build(const std::string& project, bool force) {
  BuildResult r;
  r.module = dev_module(project);
  if (!has_sources(project)) {
    r.skipped = r.ok = true;
    return r;
  }
  const fs::path dir = native_dir(project);
  const fs::path stamp_file = dir / "bin" / "sources.stamp";
  const std::string stamp = std::to_string(sources_stamp(project));
  std::error_code ec;
  if (!force && fs::exists(r.module, ec) && read_all(stamp_file) == stamp) {
    r.skipped = r.ok = true;
    return r;
  }
  std::string why;
  if (!toolchain_available(&why)) {
    r.error = "C++ scripts need a compiler toolchain: " + why;
    return r;
  }
  // SDK copy (the module builds without the engine source tree).
  const fs::path sdk = sdk_dir();
  for (const char* f : {"script_abi.h", "script_sdk.h", "ai.h", "physics.h"}) {
    write_if_changed(dir / "sdk" / "tombstone" / f, read_all(sdk / "tombstone" / f));
  }
  write_if_changed(dir / "module_main.cpp",
                   "// Generated by TombStone: the module's C ABI entry points.\n"
                   "#define TS_SCRIPT_MODULE_MAIN\n#include <tombstone/script_sdk.h>\n");
  const std::string scripts = cmake_path(fs::absolute(fs::path(project) / "scripts", ec));
  const std::string bin = cmake_path(fs::absolute(dir / "bin", ec));
  std::string cm =
      "# Generated by TombStone for this project's C++ scripts. Do not edit:\n"
      "# the editor rewrites it. Every scripts/*.cpp joins one module.\n"
      "cmake_minimum_required(VERSION 3.20)\n"
      "project(ts_project_scripts LANGUAGES CXX)\n"
      "set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
      "if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n  set(CMAKE_BUILD_TYPE Release)\nendif()\n"
      "file(GLOB TS_SCRIPT_SOURCES CONFIGURE_DEPENDS \"" + scripts + "/*.cpp\")\n"
      "add_library(ts_scripts MODULE module_main.cpp ${TS_SCRIPT_SOURCES})\n"
      "target_include_directories(ts_scripts PRIVATE sdk \"" + scripts + "\")\n"
      "set_target_properties(ts_scripts PROPERTIES PREFIX \"\" CXX_VISIBILITY_PRESET hidden\n"
      "  POSITION_INDEPENDENT_CODE ON)\n"
      "foreach(cfg \"\" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)\n"
      "  set_target_properties(ts_scripts PROPERTIES LIBRARY_OUTPUT_DIRECTORY${cfg} \"" + bin + "\"\n"
      "    RUNTIME_OUTPUT_DIRECTORY${cfg} \"" + bin + "\")\n"
      "endforeach()\n"
      "if(MSVC)\n  target_compile_options(ts_scripts PRIVATE /utf-8 /EHsc /W3)\n"
      "  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)\nendif()\n";
  write_if_changed(dir / "CMakeLists.txt", cm);
  const fs::path bdir = dir / "build";
  std::string out1, out2;
  if (!fs::exists(bdir / "CMakeCache.txt", ec)) {
    const int rc = run_command(q(cmake_exe()) + " -S " + q(dir.string()) + " -B " + q(bdir.string()) +
#if defined(_WIN32)
                                   " -A x64"
#else
                                   " -DCMAKE_BUILD_TYPE=Release"
#endif
                               , &out1);
    if (rc != 0) {
      r.output = out1;
      r.error = "cmake could not configure the script module (" + std::to_string(rc) + ")";
      r.problems = parse_build_output(out1, project);
      fs::remove_all(bdir, ec);
      return r;
    }
  }
  const int rc = run_command(q(cmake_exe()) + " --build " + q(bdir.string()) + " --config Release", &out2);
  r.output = out1 + out2;
  r.problems = parse_build_output(r.output, project);
  if (rc != 0 || !fs::exists(r.module, ec)) {
    r.error = "C++ scripts did not compile";
    return r;
  }
  write_if_changed(stamp_file, stamp);
  r.ok = true;
  return r;
}

}  // namespace native_scripts

// ---------------------------------------------------------------------------

namespace {

using FnAbi = int (*)();
using FnCreate = void* (*)(const char*, const TsScriptApi*, uint64_t, const char*);
using FnDestroy = void (*)(void*);
using FnCall = void (*)(void*, int, uint64_t, double, const char*);
using FnSave = int (*)(void*, char*, int);
using FnLoad = void (*)(void*, const char*, int);

struct NativeModule final : CompiledModule {
  SharedLib lib;
  FnCreate fc = nullptr;
  FnDestroy fd = nullptr;
  FnCall fcall = nullptr;
  FnSave fsave = nullptr;
  FnLoad fload = nullptr;
  void* create(const char* c, const TsScriptApi* a, std::uint64_t s, const char* p) override { return fc(c, a, s, p); }
  void destroy(void* o) override { fd(o); }
  void call(void* o, int h, std::uint64_t e, double n, const char* t) override { fcall(o, h, e, n, t); }
  int save(void* o, char* b, int c) override { return fsave ? fsave(o, b, c) : 0; }
  void load(void* o, const char* d, int l) override {
    if (fload) fload(o, d, l);
  }
};

// Loads a copy of `file` (so a rebuild can overwrite the original).
std::unique_ptr<CompiledModule> open_native(const std::string& file, bool prebuilt, int generation,
                                            std::string* err) {
  std::error_code ec;
  fs::path live = file;
  if (!prebuilt) {
    const fs::path dir = fs::path(file).parent_path().parent_path() / "live";
    fs::create_directories(dir, ec);
    static std::atomic<int> serial{0};
    live = dir / ("ts_scripts-" + std::to_string(generation) + "-" + std::to_string(++serial) +
                  SharedLib::extension());
    fs::copy_file(file, live, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *err = "could not stage " + file + ": " + ec.message();
      return nullptr;
    }
  }
  auto m = std::make_unique<NativeModule>();
  if (!m->lib.open(live.string(), err)) return nullptr;
  m->path = live.string();
  auto abi = reinterpret_cast<FnAbi>(m->lib.symbol("ts_script_abi"));
  m->fc = reinterpret_cast<FnCreate>(m->lib.symbol("ts_script_create"));
  m->fd = reinterpret_cast<FnDestroy>(m->lib.symbol("ts_script_destroy"));
  m->fcall = reinterpret_cast<FnCall>(m->lib.symbol("ts_script_call"));
  m->fsave = reinterpret_cast<FnSave>(m->lib.symbol("ts_script_save"));
  m->fload = reinterpret_cast<FnLoad>(m->lib.symbol("ts_script_load"));
  if (!abi || !m->fc || !m->fd || !m->fcall) {
    *err = file + " is not a TombStone script module (missing ts_script_* exports)";
    return nullptr;
  }
  if (abi() != TS_SCRIPT_ABI) {
    *err = file + ": script ABI " + std::to_string(abi()) + ", engine wants " + std::to_string(TS_SCRIPT_ABI) +
           " (rebuild the module)";
    return nullptr;
  }
  return m;
}

CompiledBackend native_backend() {
  CompiledBackend b;
  b.lang = "cpp";
  b.label = "C++";
  b.stamp = [](const std::string& p) { return native_scripts::sources_stamp(p); };
  b.build = [](const std::string& p, bool force) { return native_scripts::build(p, force); };
  b.prebuilt = [](const std::string& p) { return native_scripts::prebuilt_module(p); };
  b.open = &open_native;
  b.class_hint = [](const std::string& stem) { return "TS_SCRIPT(Class, \"" + stem + "\")"; };
  return b;
}

}  // namespace

NativeScriptHost::NativeScriptHost(ScriptWorld& world, ScriptLibrary&, std::string project_dir)
    : CompiledScriptHost(world, std::move(project_dir), native_backend()) {}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
