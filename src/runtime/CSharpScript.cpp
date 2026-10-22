#include "runtime/CSharpScript.h"
#include "core/AppPaths.h"

#include "runtime/ScriptAbi.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#if !defined(TS_DOTNET_SDK_DIR)
#define TS_DOTNET_SDK_DIR ""
#endif

namespace ts {
namespace tombstone {
namespace runtime {

namespace fs = std::filesystem;

namespace csharp_scripts {

const char* const kInstallHint =
    "install the .NET 8 SDK (https://dotnet.microsoft.com/download/dotnet/8.0) to enable C#";

namespace {

#if defined(_WIN32)
constexpr const char* kExe = "dotnet.exe";
constexpr const char* kFxr = "hostfxr.dll";
#elif defined(__APPLE__)
constexpr const char* kExe = "dotnet";
constexpr const char* kFxr = "libhostfxr.dylib";
#else
constexpr const char* kExe = "dotnet";
constexpr const char* kFxr = "libhostfxr.so";
#endif

std::string q(const std::string& s) { return "\"" + s + "\""; }

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

// "8.0.426" -> {8,0,426}
std::vector<int> version_of(const std::string& v) {
  std::vector<int> out;
  std::stringstream ss(v);
  std::string part;
  while (std::getline(ss, part, '.')) out.push_back(std::atoi(part.c_str()));
  return out;
}

// Newest "<ver> [path]" line whose major >= 8 (with an optional prefix).
std::string newest(const std::string& listing, const std::string& prefix, std::string* path) {
  std::string best;
  std::istringstream in(listing);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!prefix.empty()) {
      if (line.rfind(prefix, 0) != 0) continue;
      line = line.substr(prefix.size());
    }
    const std::size_t sp = line.find(' ');
    const std::string ver = line.substr(0, sp);
    const std::vector<int> v = version_of(ver);
    if (v.empty() || v[0] < 8 || ver.find('-') != std::string::npos) continue;
    if (best.empty() || version_of(best) < v) {
      best = ver;
      if (path) {
        const std::size_t a = line.find('['), b = line.rfind(']');
        *path = a != std::string::npos && b != std::string::npos && b > a ? line.substr(a + 1, b - a - 1) : "";
      }
    }
  }
  return best;
}

DotnetInfo discover() {
  DotnetInfo d;
  std::vector<fs::path> roots;
  std::error_code ec;
  if (const char* r = std::getenv("DOTNET_ROOT"); r && *r) roots.emplace_back(r);
#if defined(_WIN32)
  if (const char* pf = std::getenv("ProgramFiles"); pf && *pf) roots.push_back(fs::path(pf) / "dotnet");
  roots.emplace_back("C:/Program Files/dotnet");
  if (const char* h = std::getenv("USERPROFILE"); h && *h) roots.push_back(fs::path(h) / ".dotnet");
#else
  if (const char* h = std::getenv("HOME"); h && *h) roots.push_back(fs::path(h) / ".dotnet");
  for (const char* r : {"/usr/share/dotnet", "/usr/lib/dotnet", "/usr/local/share/dotnet", "/opt/dotnet",
                        "/usr/local/dotnet"}) {
    roots.emplace_back(r);
  }
#endif
  for (const fs::path& r : roots) {
    if (fs::exists(r / kExe, ec)) {
      d.dotnet = (r / kExe).string();
      d.root = r.string();
      break;
    }
  }
  if (d.dotnet.empty()) d.dotnet = "dotnet";  // PATH
  std::string out;
  if (run_command(q(d.dotnet) + " --list-runtimes", &out) == 0) {
    std::string path;
    d.runtime = newest(out, "Microsoft.NETCore.App ", &path);
    if (d.root.empty() && !path.empty()) d.root = fs::path(path).parent_path().parent_path().string();
  }
  if (run_command(q(d.dotnet) + " --list-sdks", &out) == 0) d.sdk = newest(out, "", nullptr);
  if (!d.root.empty()) {
    std::string best;
    for (const auto& e : fs::directory_iterator(fs::path(d.root) / "host" / "fxr", ec)) {
      if (!fs::exists(e.path() / kFxr, ec)) continue;
      const std::string v = e.path().filename().string();
      if (best.empty() || version_of(best) < version_of(v)) best = v;
    }
    if (!best.empty()) d.hostfxr = (fs::path(d.root) / "host" / "fxr" / best / kFxr).string();
  }
  return d;
}

fs::path dotnet_dir(const std::string& project) { return fs::path(project) / ".tombstone" / "dotnet"; }

fs::path binding_sources() {
  std::error_code ec;
  if (const char* e = std::getenv("TS_DOTNET_SDK"); e && *e) return fs::path(e) / "TombStone.Scripting";
  if (const std::string exe = core::executable_dir(); !exe.empty()) {
    const fs::path installed = fs::path(exe) / "sdk" / "dotnet" / "TombStone.Scripting";
    if (fs::exists(installed / "Host.cs", ec)) return installed;
  }
  const fs::path p = fs::path(TS_DOTNET_SDK_DIR) / "TombStone.Scripting";
  return fs::exists(p / "Host.cs", ec) ? p : fs::path();
}

std::string xml_path(const fs::path& p) { return p.lexically_normal().generic_string(); }

}  // namespace

const DotnetInfo& dotnet_info() {
  static const DotnetInfo info = discover();  // thread-safe static init
  return info;
}

std::string binding_sources_dir() { return binding_sources().string(); }

bool sdk_available(std::string* why) {
  const std::string r = disabled_reason(false);
  if (why) *why = r;
  return r.empty();
}

std::string disabled_reason(bool prebuilt) {
  const DotnetInfo& d = dotnet_info();
  if (!prebuilt && d.sdk.empty()) return std::string("C# scripts disabled: .NET SDK not found - ") + kInstallHint;
  if (d.runtime.empty() || d.hostfxr.empty()) {
    return std::string("C# scripts disabled: no .NET 8+ runtime found to host them - ") + kInstallHint;
  }
  if (!prebuilt && binding_sources().empty()) {
    return "C# scripts disabled: TombStone.Scripting sources not found (set TS_DOTNET_SDK to sdk/dotnet)";
  }
  return {};
}

bool has_sources(const std::string& project) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(project) / "scripts", ec)) {
    if (e.path().extension() == ".cs") return true;
  }
  return false;
}

std::int64_t sources_stamp(const std::string& project) {
  std::error_code ec;
  std::vector<std::string> names;
  for (const auto& e : fs::recursive_directory_iterator(fs::path(project) / "scripts", ec)) {
    if (e.path().extension() != ".cs") continue;
    names.push_back(e.path().generic_string() + "|" +
                    std::to_string(static_cast<long long>(e.last_write_time(ec).time_since_epoch().count())) +
                    "|" + std::to_string(e.file_size(ec)));
  }
  std::sort(names.begin(), names.end());
  std::uint64_t h = 1469598103934665603ULL;
  for (const std::string& n : names) {
    for (unsigned char c : n) h = (h ^ c) * 1099511628211ULL;
  }
  return static_cast<std::int64_t>(h & 0x7FFFFFFFFFFFFFFFULL);
}

std::string prebuilt_assembly(const std::string& p) { return (fs::path(p) / "dotnet" / "ProjectScripts.dll").string(); }
std::string dev_assembly(const std::string& p) { return (dotnet_dir(p) / "bin" / "ProjectScripts.dll").string(); }

std::vector<std::string> shipped_files(const std::string&) {
  return {"TombStone.Scripting.dll", "TombStone.Scripting.runtimeconfig.json", "TombStone.Scripting.deps.json"};
}

ScriptBuildResult build(const std::string& project, bool force) {
  ScriptBuildResult r;
  r.module = dev_assembly(project);
  if (!has_sources(project)) {
    r.ok = r.skipped = true;
    return r;
  }
  std::error_code ec;
  const fs::path dir = dotnet_dir(project);
  const fs::path stamp_file = dir / "bin" / "sources.stamp";
  // The binding (TombStone.Scripting) is part of the stamp too, so an SDK /
  // script ABI change rebuilds old project assemblies.
  std::uint64_t binding_hash = 1469598103934665603ULL;
  if (const fs::path bsrc = binding_sources(); !bsrc.empty()) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(bsrc, ec)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
      const std::string ext = f.extension().string();
      if (ext != ".cs" && ext != ".csproj") continue;
      for (unsigned char c : f.filename().string() + read_all(f)) binding_hash = (binding_hash ^ c) * 1099511628211ULL;
    }
  }
  const std::string stamp = std::to_string(sources_stamp(project)) + "/" + std::to_string(binding_hash);
  if (!force && fs::exists(r.module, ec) && read_all(stamp_file) == stamp) {
    r.ok = r.skipped = true;
    return r;
  }
  if (const std::string why = disabled_reason(false); !why.empty()) {
    r.error = why;
    return r;
  }
  // Binding copy (the project builds without the engine tree).
  const fs::path src = binding_sources();
  for (const auto& e : fs::directory_iterator(src, ec)) {
    const std::string ext = e.path().extension().string();
    if (ext == ".cs" || ext == ".csproj") write_if_changed(dir / "binding" / e.path().filename(), read_all(e.path()));
  }
  const std::string scripts = xml_path(fs::absolute(fs::path(project) / "scripts", ec));
  const std::string csproj =
      "<!-- Generated by TombStone for this project's C# scripts. Do not edit:\n"
      "     the editor rewrites it. Every scripts/*.cs joins one assembly. -->\n"
      "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
      "  <PropertyGroup>\n"
      "    <TargetFramework>net8.0</TargetFramework>\n"
      "    <AssemblyName>ProjectScripts</AssemblyName>\n"
      "    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n"
      "    <ImplicitUsings>disable</ImplicitUsings>\n"
      "    <Nullable>disable</Nullable>\n"
      "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n"
      "    <EnableDynamicLoading>true</EnableDynamicLoading>\n"
      "    <OutputPath>../bin/</OutputPath>\n"
      "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n"
      "    <DebugType>portable</DebugType>\n"
      "    <GenerateRuntimeConfigurationFiles>false</GenerateRuntimeConfigurationFiles>\n"
      "  </PropertyGroup>\n"
      "  <ItemGroup>\n"
      "    <Compile Include=\"" + scripts + "/**/*.cs\" />\n"
      "  </ItemGroup>\n"
      "  <ItemGroup>\n"
      "    <ProjectReference Include=\"../binding/TombStone.Scripting.csproj\">\n"
      "      <Private>false</Private>\n"
      "    </ProjectReference>\n"
      "  </ItemGroup>\n"
      "</Project>\n";
  write_if_changed(dir / "scripts" / "ProjectScripts.csproj", csproj);
  const DotnetInfo& d = dotnet_info();
  std::string out;
  const int rc = run_command(q(d.dotnet) + " build " + q((dir / "scripts" / "ProjectScripts.csproj").string()) +
                                 " -c Release --nologo -v q -clp:NoSummary -p:TreatWarningsAsErrors=false",
                             &out);
  r.output = out;
  r.problems = parse_build_output(out, project);
  // dotnet repeats each diagnostic; keep one of each.
  std::vector<LogEntry> uniq;
  for (const LogEntry& p : r.problems) {
    bool dup = false;
    for (const LogEntry& u : uniq) dup = dup || (u.file == p.file && u.line == p.line && u.text == p.text);
    if (!dup) uniq.push_back(p);
  }
  r.problems = std::move(uniq);
  if (rc != 0 || !fs::exists(r.module, ec)) {
    r.error = "C# scripts did not compile";
    return r;
  }
  write_if_changed(stamp_file, stamp);
  r.ok = true;
  return r;
}

// ---------------------------------------------------------------------------
// hostfxr: one .NET runtime per process.

namespace {

#if defined(_WIN32)
using char_t = wchar_t;
#define TS_FXR_CALL __cdecl
#define TS_CLR_CALL __stdcall
#else
using char_t = char;
#define TS_FXR_CALL
#define TS_CLR_CALL
#endif

using fxr_init_fn = int32_t(TS_FXR_CALL*)(const char_t*, const void*, void**);
using fxr_delegate_fn = int32_t(TS_FXR_CALL*)(void*, int, void**);
using fxr_close_fn = int32_t(TS_FXR_CALL*)(void*);
using fxr_error_writer_fn = void(TS_FXR_CALL*)(const char_t*);
using fxr_set_error_writer_fn = fxr_error_writer_fn(TS_FXR_CALL*)(fxr_error_writer_fn);
using load_fn = int(TS_CLR_CALL*)(const char_t*, const char_t*, const char_t*, const char_t*, void*, void**);
constexpr int kLoadAssemblyAndGetFunctionPointer = 5;
const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

std::basic_string<char_t> native(const fs::path& p) { return p.native(); }
std::basic_string<char_t> native(const char* s) { return fs::path(s).native(); }

std::string g_fxr_error;
void TS_FXR_CALL fxr_error(const char_t* msg) {
  if (g_fxr_error.size() < 2000) g_fxr_error += fs::path(msg).string() + "\n";
}

struct Runtime {
  std::mutex mu;
  bool tried = false;
  bool ok = false;
  std::string error;
  std::string host_dir;
  SharedLib fxr;
  int (*abi)() = nullptr;
  int (*load_scripts)(const char*, char*, int) = nullptr;
  void (*unload_scripts)(int) = nullptr;
  void* (*create)(int, const char*, const TsScriptApi*, std::uint64_t, const char*) = nullptr;
  void (*destroy)(void*) = nullptr;
  void (*call)(void*, int, std::uint64_t, double, const char*) = nullptr;
  int (*save)(void*, char*, int) = nullptr;
  void (*load)(void*, const char*, int) = nullptr;
  int (*live)() = nullptr;

  // Starts .NET from host_dir/TombStone.Scripting.runtimeconfig.json.
  bool start(const fs::path& dir) {
    std::lock_guard<std::mutex> lock(mu);
    if (tried) return ok;
    tried = true;
    const DotnetInfo& d = dotnet_info();
    if (d.hostfxr.empty()) {
      error = std::string("no hostfxr found - ") + kInstallHint;
      return false;
    }
    if (!fxr.open(d.hostfxr, &error)) return false;
    auto init = reinterpret_cast<fxr_init_fn>(fxr.symbol("hostfxr_initialize_for_runtime_config"));
    auto get = reinterpret_cast<fxr_delegate_fn>(fxr.symbol("hostfxr_get_runtime_delegate"));
    auto close = reinterpret_cast<fxr_close_fn>(fxr.symbol("hostfxr_close"));
    auto set_writer = reinterpret_cast<fxr_set_error_writer_fn>(fxr.symbol("hostfxr_set_error_writer"));
    if (!init || !get || !close) {
      error = d.hostfxr + " is not a usable hostfxr";
      return false;
    }
    if (set_writer) set_writer(&fxr_error);
    const fs::path config = dir / "TombStone.Scripting.runtimeconfig.json";
    const fs::path assembly = dir / "TombStone.Scripting.dll";
    void* h = nullptr;
    const int32_t rc = init(native(config).c_str(), nullptr, &h);
    if ((rc != 0 && rc != 1 && rc != 2) || !h) {
      error = "hostfxr could not start .NET (" + std::to_string(rc) + "): " + g_fxr_error;
      if (h) close(h);
      return false;
    }
    void* lf = nullptr;
    const int32_t rc2 = get(h, kLoadAssemblyAndGetFunctionPointer, &lf);
    close(h);
    if (rc2 != 0 || !lf) {
      error = "hostfxr gave no assembly loader (" + std::to_string(rc2) + ")";
      return false;
    }
    auto loader = reinterpret_cast<load_fn>(lf);
    const auto type = native("TombStone.Host, TombStone.Scripting");
    const auto path = native(assembly);
    auto get_fn = [&](const char* name, void** out) {
      const int32_t r = loader(path.c_str(), type.c_str(), native(name).c_str(), kUnmanagedCallersOnly, nullptr, out);
      if (r != 0 || !*out) {
        error = std::string("TombStone.Scripting has no ") + name + " (" + std::to_string(r) + ") " + g_fxr_error;
        return false;
      }
      return true;
    };
    void* p[10] = {};
    const char* names[] = {"Abi", "LoadScripts", "UnloadScripts", "Create", "Destroy",
                           "Call", "Save", "Load", "LiveContexts"};
    for (int i = 0; i < 9; ++i) {
      if (!get_fn(names[i], &p[i])) return false;
    }
    abi = reinterpret_cast<int (*)()>(p[0]);
    load_scripts = reinterpret_cast<int (*)(const char*, char*, int)>(p[1]);
    unload_scripts = reinterpret_cast<void (*)(int)>(p[2]);
    create = reinterpret_cast<void* (*)(int, const char*, const TsScriptApi*, std::uint64_t, const char*)>(p[3]);
    destroy = reinterpret_cast<void (*)(void*)>(p[4]);
    call = reinterpret_cast<void (*)(void*, int, std::uint64_t, double, const char*)>(p[5]);
    save = reinterpret_cast<int (*)(void*, char*, int)>(p[6]);
    load = reinterpret_cast<void (*)(void*, const char*, int)>(p[7]);
    live = reinterpret_cast<int (*)()>(p[8]);
    if (abi() != TS_SCRIPT_ABI) {
      error = "TombStone.Scripting speaks script ABI " + std::to_string(abi()) + ", engine wants " +
              std::to_string(TS_SCRIPT_ABI);
      return false;
    }
    host_dir = dir.string();
    ok = true;
    return true;
  }
};

Runtime& runtime() {
  static Runtime* r = new Runtime();  // never torn down: .NET cannot be restarted in a process
  return *r;
}

}  // namespace

int live_contexts() {
  Runtime& r = runtime();
  return r.ok ? r.live() : -1;
}

}  // namespace csharp_scripts

// ---------------------------------------------------------------------------

namespace {

using csharp_scripts::Runtime;

struct ManagedModule final : CompiledModule {
  Runtime* rt = nullptr;
  int gen = 0;
  ~ManagedModule() override {
    if (rt && gen) rt->unload_scripts(gen);
  }
  void* create(const char* c, const TsScriptApi* a, std::uint64_t s, const char* p) override {
    return rt->create(gen, c, a, s, p);
  }
  void destroy(void* o) override { rt->destroy(o); }
  void call(void* o, int h, std::uint64_t e, double n, const char* t) override { rt->call(o, h, e, n, t); }
  int save(void* o, char* b, int c) override { return rt->save(o, b, c); }
  void load(void* o, const char* d, int l) override { rt->load(o, d, l); }
};

std::unique_ptr<CompiledModule> open_managed(const std::string& file, bool prebuilt, int, std::string* err) {
  Runtime& rt = csharp_scripts::runtime();
  std::error_code ec;
  const fs::path bin = fs::path(file).parent_path();
  fs::path host = bin;
  if (!prebuilt) {
    // Stage the binding (the runtime keeps its files open).
    host = bin.parent_path() / "host";
    const fs::path built = bin.parent_path() / "binding" / "bin" / "Release" / "net8.0";
    if (!rt.tried) {
      fs::create_directories(host, ec);
      for (const std::string& f : csharp_scripts::shipped_files("")) {
        fs::copy_file(built / f, host / f, fs::copy_options::overwrite_existing, ec);
      }
    }
  }
  if (!rt.start(host)) {
    *err = rt.error;
    return nullptr;
  }
  char buf[1024] = {};
  const int gen = rt.load_scripts(file.c_str(), buf, sizeof(buf));
  if (!gen) {
    *err = "could not load " + fs::path(file).filename().string() + ": " + buf;
    return nullptr;
  }
  auto m = std::make_unique<ManagedModule>();
  m->rt = &rt;
  m->gen = gen;
  m->path = file;
  return m;
}

CompiledBackend csharp_backend() {
  CompiledBackend b;
  b.lang = "cs";
  b.label = "C#";
  b.stamp = [](const std::string& p) { return csharp_scripts::sources_stamp(p); };
  b.build = [](const std::string& p, bool force) { return csharp_scripts::build(p, force); };
  b.prebuilt = [](const std::string& p) { return csharp_scripts::prebuilt_assembly(p); };
  b.open = &open_managed;
  b.usable = [](bool prebuilt, std::string* why) {
    *why = csharp_scripts::disabled_reason(prebuilt);
    return why->empty();
  };
  b.class_hint = [](const std::string& stem) {
    return "[Script(\"" + stem + "\")] class : TombStone.Script";
  };
  return b;
}

}  // namespace

CSharpScriptHost::CSharpScriptHost(ScriptWorld& world, ScriptLibrary&, std::string project_dir)
    : CompiledScriptHost(world, std::move(project_dir), csharp_backend()) {}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
