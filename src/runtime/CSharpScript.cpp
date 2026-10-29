#include "runtime/CSharpScript.h"
#include "core/AppPaths.h"

#include "runtime/ScriptAbi.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

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

std::string xml_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

// Canonical path used to drop the same file listed twice (a relative path
// and an absolute one, a symlink, or Scripts vs scripts on Windows).
std::string path_key(const fs::path& p) {
  std::error_code ec;
  fs::path c = fs::weakly_canonical(p, ec);
  if (ec) c = fs::absolute(p, ec).lexically_normal();
  std::string s = c.generic_string();
#if defined(_WIN32)
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
#endif
  return s;
}

bool is_cs(const fs::path& p) {
  std::string ext = p.extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".cs";
}

struct TypeDecl {
  std::string qname;
  std::string file;
  int line = 0;
  bool partial = false;
};

void skip_trivia(const std::string& s, std::size_t& i, int& line) {
  while (i < s.size()) {
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    if (c == '\n') {
      ++line;
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] == '\n') ++line;
        ++i;
      }
      if (i + 1 < s.size()) i += 2;
      continue;
    }
    break;
  }
}

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '@'; }
bool ident_cont(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Reads an identifier at i, or returns false and leaves i unchanged.
bool read_ident(const std::string& s, std::size_t& i, int& line, std::string* out) {
  skip_trivia(s, i, line);
  if (i >= s.size() || !ident_start(s[i])) return false;
  std::size_t j = i;
  if (s[j] == '@') ++j;
  if (j >= s.size() || !(std::isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_')) return false;
  ++j;
  while (j < s.size() && ident_cont(s[j])) ++j;
  *out = s.substr(i + (s[i] == '@' ? 1 : 0), j - (i + (s[i] == '@' ? 1 : 0)));
  i = j;
  return true;
}

void skip_string(const std::string& s, std::size_t& i, int& line) {
  const bool verbatim = i > 0 && s[i - 1] == '@' && s[i] == '"';
  if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return;
  const char q = s[i];
  ++i;
  if (verbatim && q == '"') {
    while (i < s.size()) {
      if (s[i] == '\n') ++line;
      if (s[i] == '"') {
        if (i + 1 < s.size() && s[i + 1] == '"') {
          i += 2;
          continue;
        }
        ++i;
        return;
      }
      ++i;
    }
    return;
  }
  while (i < s.size()) {
    if (s[i] == '\n') {
      ++line;
      ++i;
      if (q == '"') return;  // a regular string cannot span a line
      continue;
    }
    if (s[i] == '\\' && i + 1 < s.size()) {
      i += 2;
      continue;
    }
    if (s[i] == q) {
      ++i;
      return;
    }
    ++i;
  }
}

// [Script("stem")] and other attributes. Returns the Script stem, if any.
std::string scan_attribute(const std::string& s, std::size_t& i, int& line) {
  std::string stem;
  if (i >= s.size() || s[i] != '[') return stem;
  ++i;
  int depth = 1;
  while (i < s.size() && depth > 0) {
    skip_trivia(s, i, line);
    if (i >= s.size()) break;
    if (s[i] == '[') {
      ++depth;
      ++i;
      continue;
    }
    if (s[i] == ']') {
      --depth;
      ++i;
      continue;
    }
    if (s[i] == '"' || s[i] == '\'') {
      skip_string(s, i, line);
      continue;
    }
    std::string id;
    if (read_ident(s, i, line, &id)) {
      if (depth == 1 && id == "Script") {
        skip_trivia(s, i, line);
        if (i < s.size() && s[i] == '(') {
          ++i;
          skip_trivia(s, i, line);
          if (i < s.size() && s[i] == '"') {
            const std::size_t start = i + 1;
            skip_string(s, i, line);
            if (i > start) stem = s.substr(start, i - start - 1);
          }
        }
      }
      continue;
    }
    ++i;
  }
  return stem;
}

std::string qualify(const std::vector<std::string>& ns, const std::string& name) {
  std::string q;
  for (const std::string& p : ns) {
    if (!q.empty()) q += '.';
    q += p;
  }
  if (!q.empty()) q += '.';
  q += name;
  return q;
}

bool is_modifier(const std::string& id) {
  return id == "public" || id == "private" || id == "protected" || id == "internal" || id == "static" ||
         id == "abstract" || id == "sealed" || id == "unsafe" || id == "new" || id == "readonly" || id == "ref" ||
         id == "file" || id == "async" || id == "virtual" || id == "override" || id == "extern" || id == "volatile";
}

std::vector<TypeDecl> scan_types(const std::string& src, const std::string& file) {
  std::vector<TypeDecl> out;
  std::vector<std::string> ns;
  struct Frame {
    int brace;
    int count;
  };
  std::vector<Frame> frames;
  int brace = 0;
  bool pending_partial = false;
  std::string pending_script;
  std::vector<std::string> pending_ns;
  bool open_ns = false;
  std::size_t i = 0;
  int line = 1;
  auto ns_brace = [&]() { return frames.empty() ? 0 : frames.back().brace; };
  auto clear_flags = [&]() {
    pending_partial = false;
    pending_script.clear();
  };
  while (i < src.size()) {
    skip_trivia(src, i, line);
    if (i >= src.size()) break;
    if (src[i] == '"' || src[i] == '\'') {
      skip_string(src, i, line);
      continue;
    }
    // @"..." or $@"..." / @$"..." — the @ is an ident start, handled below
    // when it is not a keyword. A verbatim string starts with @".
    if (src[i] == '@' && i + 1 < src.size() && src[i + 1] == '"') {
      ++i;
      skip_string(src, i, line);
      continue;
    }
    if ((src[i] == '$') && i + 1 < src.size() && (src[i + 1] == '"' || (src[i + 1] == '@' && i + 2 < src.size() && src[i + 2] == '"'))) {
      // Interpolated strings can contain braces. Skip the holes coarsely:
      // walk until the closing quote, counting { } that are not doubled.
      const bool verb = src[i + 1] == '@';
      i += verb ? 3 : 2;
      int hole = 0;
      while (i < src.size()) {
        if (src[i] == '\n' && !verb) break;
        if (src[i] == '\n') ++line;
        if (!verb && src[i] == '\\' && i + 1 < src.size()) {
          i += 2;
          continue;
        }
        if (src[i] == '{') {
          if (i + 1 < src.size() && src[i + 1] == '{') {
            i += 2;
            continue;
          }
          ++hole;
          ++i;
          continue;
        }
        if (src[i] == '}' && hole > 0) {
          if (i + 1 < src.size() && src[i + 1] == '}') {
            i += 2;
            continue;
          }
          --hole;
          ++i;
          continue;
        }
        if (hole == 0 && src[i] == '"') {
          if (verb && i + 1 < src.size() && src[i + 1] == '"') {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (src[i] == '[') {
      const std::string stem = scan_attribute(src, i, line);
      if (!stem.empty()) pending_script = stem;
      continue;
    }
    if (src[i] == '{') {
      ++brace;
      if (open_ns) {
        frames.push_back(Frame{brace, static_cast<int>(pending_ns.size())});
        for (const std::string& p : pending_ns) ns.push_back(p);
        pending_ns.clear();
        open_ns = false;
      }
      ++i;
      continue;
    }
    if (src[i] == '}') {
      if (!frames.empty() && frames.back().brace == brace) {
        for (int n = 0; n < frames.back().count && !ns.empty(); ++n) ns.pop_back();
        frames.pop_back();
      }
      if (brace > 0) --brace;
      ++i;
      continue;
    }
    if (src[i] == ';') {
      pending_partial = false;
      ++i;
      continue;
    }
    std::string id;
    const int id_line = line;
    if (!read_ident(src, i, line, &id)) {
      ++i;
      continue;
    }
    if (id == "namespace" && brace == ns_brace()) {
      std::vector<std::string> parts;
      std::string part;
      if (read_ident(src, i, line, &part)) {
        parts.push_back(part);
        for (;;) {
          skip_trivia(src, i, line);
          if (i < src.size() && src[i] == '.') {
            ++i;
            if (!read_ident(src, i, line, &part)) break;
            parts.push_back(part);
            continue;
          }
          break;
        }
        skip_trivia(src, i, line);
        if (i < src.size() && src[i] == ';') {
          ns = parts;
          frames.clear();
          ++i;
        } else if (i < src.size() && src[i] == '{') {
          pending_ns = parts;
          open_ns = true;
        }
      }
      continue;
    }
    if (id == "partial") {
      pending_partial = true;
      continue;
    }
    if (id == "struct" || id == "enum" || id == "interface" || id == "delegate") {
      clear_flags();
      continue;
    }
    if (id == "class" || id == "record") {
      std::string name;
      std::size_t save_i = i;
      int save_line = line;
      std::string next;
      if (!read_ident(src, i, line, &next)) {
        i = save_i;
        line = save_line;
        clear_flags();
        continue;
      }
      if (id == "record" && next == "struct") {
        clear_flags();
        continue;
      }
      if (id == "record" && next == "class") {
        if (!read_ident(src, i, line, &name)) {
          clear_flags();
          continue;
        }
      } else if (next == "struct" || next == "class" || next == "interface" || next == "enum" || next == "where") {
        clear_flags();
        continue;
      } else {
        name = next;
      }
      if (brace == ns_brace() && !name.empty()) {
        TypeDecl d;
        d.qname = qualify(ns, name);
        d.file = file;
        d.line = id_line;
        d.partial = pending_partial;
        (void)pending_script;
        out.push_back(std::move(d));
      }
      clear_flags();
      continue;
    }
    if (!is_modifier(id) && brace == ns_brace()) pending_partial = false;
  }
  return out;
}

std::string join_and(const std::vector<std::string>& xs) {
  if (xs.empty()) return {};
  if (xs.size() == 1) return xs[0];
  if (xs.size() == 2) return xs[0] + " and " + xs[1];
  std::string s;
  for (std::size_t n = 0; n < xs.size(); ++n) {
    if (n) s += (n + 1 == xs.size()) ? " and " : ", ";
    s += xs[n];
  }
  return s;
}

// Two different files declaring the same class. CSC reports CS0101 / CS0111
// only on the later file, so the earlier copy is invisible in the console.
std::vector<LogEntry> duplicate_classes(const std::string& project, const std::vector<std::string>& files) {
  std::vector<TypeDecl> decls;
  for (const std::string& rel : files) {
    const std::string src = read_all(fs::path(project) / rel);
    for (TypeDecl& d : scan_types(src, rel)) decls.push_back(std::move(d));
  }
  std::vector<LogEntry> problems;
  std::vector<std::string> names;
  for (const TypeDecl& d : decls) names.push_back(d.qname);
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  for (const std::string& qname : names) {
    std::vector<const TypeDecl*> group;
    for (const TypeDecl& d : decls)
      if (d.qname == qname) group.push_back(&d);
    std::vector<std::string> where;
    bool all_partial = true;
    for (const TypeDecl* d : group) {
      all_partial = all_partial && d->partial;
      if (std::find(where.begin(), where.end(), d->file) == where.end()) where.push_back(d->file);
    }
    if (where.size() < 2 || all_partial) continue;
    LogEntry e;
    e.level = LogLevel::Error;
    e.channel = "script";
    e.file = where.back();
    e.line = group.back()->line;
    e.text = "C# script class " + qname + " is defined in " + join_and(where) +
             ". Rename one so each script class has its own name.";
    problems.push_back(std::move(e));
  }
  return problems;
}

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

std::vector<std::string> compile_files(const std::string& project) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path root = fs::path(project) / "scripts";
  if (!fs::is_directory(root, ec)) return out;
  const std::string root_key = path_key(root);
  std::unordered_set<std::string> seen;
  std::vector<std::pair<std::string, std::string>> items;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code fec;
    if (it->is_directory(fec)) {
      if (generated_script_directory(it->path().filename().string())) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(fec) || !is_cs(it->path())) continue;
    const std::string key = path_key(it->path());
    if (key != root_key && key.rfind(root_key + "/", 0) != 0) continue;
    if (!seen.insert(key).second) continue;
    const fs::path rel = fs::relative(it->path(), fs::path(project), fec);
    if (fec) continue;
    items.emplace_back(rel.generic_string(), key);
  }
  std::sort(items.begin(), items.end(),
            [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
              return a.first < b.first;
            });
  out.reserve(items.size());
  for (auto& item : items) out.push_back(std::move(item.first));
  return out;
}

bool has_sources(const std::string& project) { return !compile_files(project).empty(); }

std::int64_t sources_stamp(const std::string& project) {
  std::error_code ec;
  std::vector<std::string> names;
  for (const std::string& rel : compile_files(project)) {
    const fs::path p = fs::path(project) / rel;
    names.push_back(rel + "|" +
                    std::to_string(static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count())) +
                    "|" + std::to_string(fs::file_size(p, ec)));
  }
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
  // One Compile item per file. A recursive scripts/**/*.cs glob also picks up
  // copies under obj/bin/cache/build, and on Windows an absolute forward-slash
  // wildcard can hand the compiler the same path twice. CSC then reports
  // CS0101 / CS0111 only against the later file.
  const std::vector<std::string> sources = compile_files(project);
  if (const std::vector<LogEntry> dups = duplicate_classes(project, sources); !dups.empty()) {
    r.problems = dups;
    for (const LogEntry& p : dups) {
      r.output += p.file + ":" + std::to_string(p.line) + ": error: " + p.text + "\n";
    }
    r.error = "C# scripts did not compile";
    return r;
  }
  std::string includes;
  for (const std::string& rel : sources) {
    const fs::path abs = fs::absolute(fs::path(project) / rel, ec);
    includes += "    <Compile Include=\"" + xml_escape(xml_path(abs)) + "\" />\n";
  }
  const std::string props =
      "<Project>\n"
      "  <!-- Generated by TombStone. Keeps a claim Directory.Build.props from\n"
      "       adding a second Compile glob on top of the file list. -->\n"
      "  <PropertyGroup>\n"
      "    <EnableDefaultItems>false</EnableDefaultItems>\n"
      "    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n"
      "    <EnableDefaultNoneItems>false</EnableDefaultNoneItems>\n"
      "    <ImportDirectoryBuildTargets>false</ImportDirectoryBuildTargets>\n"
      "  </PropertyGroup>\n"
      "</Project>\n";
  write_if_changed(dir / "scripts" / "Directory.Build.props", props);
  const std::string csproj =
      "<!-- Generated by TombStone for this project's C# scripts. Do not edit:\n"
      "     the editor rewrites it. Every scripts/*.cs joins one assembly.\n"
      "     Each file is listed once; obj, bin, cache and build are left out. -->\n"
      "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
      "  <PropertyGroup>\n"
      "    <TargetFramework>net8.0</TargetFramework>\n"
      "    <AssemblyName>ProjectScripts</AssemblyName>\n"
      "    <EnableDefaultItems>false</EnableDefaultItems>\n"
      "    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n"
      "    <EnableDefaultNoneItems>false</EnableDefaultNoneItems>\n"
      "    <ImportDirectoryBuildTargets>false</ImportDirectoryBuildTargets>\n"
      "    <ImplicitUsings>disable</ImplicitUsings>\n"
      "    <Nullable>disable</Nullable>\n"
      "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n"
      "    <EnableDynamicLoading>true</EnableDynamicLoading>\n"
      "    <OutputPath>../bin/</OutputPath>\n"
      "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n"
      "    <DebugType>portable</DebugType>\n"
      "    <GenerateRuntimeConfigurationFiles>false</GenerateRuntimeConfigurationFiles>\n"
      "  </PropertyGroup>\n"
      "  <ItemGroup>\n" +
      includes +
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
