#include "editor/scripting/ScriptComponents.h"
#include "editor/ux/EmptyState.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/Actions.h"

#include "core/AppPaths.h"
#include "editor/launch/OpenWithOs.h"
#include "editor/scripting/BrandBook.h"
#include "editor/ui/Theme.h"
#include "runtime/CSharpScript.h"
#include "runtime/GraphScript.h"
#include "runtime/NativeScript.h"
#include "runtime/Script.h"
#include "runtime/ScriptHost.h"
#include "scene/SampleScripts.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {
#include "editor/scripting/ScriptTemplates.inc"

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  std::size_t at = 0;
  while ((at = s.find(from, at)) != std::string::npos) {
    s.replace(at, from.size(), to);
    at += to.size();
  }
  return s;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Writes when the content differs; true = written.
bool write_if_changed(const fs::path& p, const std::string& text, std::string* error) {
  std::error_code ec;
  if (fs::exists(p, ec) && read_file(p) == text) return false;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out && error) *error = "could not write " + p.string();
  return static_cast<bool>(out);
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (const char c : s) {
    if (c == '"' || c == '\\') o.push_back('\\');
    if (c == '\n') {
      o += "\\n";
      continue;
    }
    o.push_back(c);
  }
  return o;
}

std::string generic(const fs::path& p) { return p.lexically_normal().generic_string(); }

// --- Graph templates ---------------------------------------------------------------

using runtime::Graph;
using runtime::GraphNode;

ScriptValue N(double v) { return ScriptValue::of_number(v); }
ScriptValue T(const std::string& v) { return ScriptValue::of_text(v); }
ScriptValue B(bool v) { return ScriptValue::of_bool(v); }

struct GraphBuilder {
  Graph g;
  int add(const std::string& type, float x, float y, std::map<std::string, ScriptValue> in = {}) {
    GraphNode n;
    n.id = g.next_id();
    n.type = type;
    n.x = x;
    n.y = y;
    n.inputs = std::move(in);
    g.nodes.push_back(n);
    return n.id;
  }
  void link(int a, const std::string& pa, int b, const std::string& pb) { g.links.push_back({a, pa, b, pb}); }
  void prop(const std::string& name, ScriptValue v) { g.props.push_back({name, std::move(v)}); }
};

bool graph_template(const std::string& id, const std::string& stem, Graph* out) {
  GraphBuilder b;
  if (id == "blank") {
    b.prop("speed", N(60));
    const int start = b.add("event.start", 0, 0);
    const int log = b.add("log", 240, 0, {{"text", T(stem + " saddled up")}});
    b.add("event.tick", 0, 140);
    b.add("event.trigger_enter", 0, 260);
    b.add("event.interact", 0, 380);
    b.link(start, "out", log, "in");
  } else if (id == "gate") {
    b.prop("open_line", T("The gate creaks open."));
    b.prop("locked", B(false));
    const int interact = b.add("event.interact", 0, 0);
    const int locked = b.add("prop.get", 0, 160, {{"name", T("locked")}});
    const int branch = b.add("flow.branch", 240, 0);
    const int nope = b.add("toast", 480, -120, {{"text", T("Locked tight. Somebody's got the key.")}});
    const int self = b.add("entity.self", 240, 260);
    const int solid = b.add("entity.set_collider", 480, 40, {{"on", B(false)}});
    const int hide = b.add("entity.set_visible", 720, 40, {{"visible", B(false)}});
    const int line = b.add("prop.get", 720, 220, {{"name", T("open_line")}});
    const int say = b.add("toast", 960, 40);
    b.link(interact, "out", branch, "in");
    b.link(locked, "value", branch, "condition");
    b.link(branch, "true", nope, "in");
    b.link(branch, "false", solid, "in");
    b.link(self, "id", solid, "id");
    b.link(solid, "out", hide, "in");
    b.link(self, "id", hide, "id");
    b.link(hide, "out", say, "in");
    b.link(line, "value", say, "text");
  } else if (id == "pickup") {
    b.prop("amount", N(10));
    const int enter = b.add("event.trigger_enter", 0, 0);
    const int purse = b.add("state.get", 0, 160, {{"key", T("gold")}});
    const int amount = b.add("prop.get", 0, 300, {{"name", T("amount")}});
    const int sum = b.add("math.add", 240, 200);
    const int keep = b.add("state.set", 240, 0, {{"key", T("gold")}});
    const int say = b.add("toast", 480, 0, {{"text", T("Picked up some gold")}, {"seconds", N(2.5)}});
    const int self = b.add("entity.self", 480, 200);
    const int gone = b.add("entity.destroy", 720, 0);
    b.link(enter, "out", keep, "in");
    b.link(enter, "other", purse, "id");
    b.link(enter, "other", keep, "id");
    b.link(purse, "value", sum, "a");
    b.link(amount, "value", sum, "b");
    b.link(sum, "value", keep, "value");
    b.link(keep, "out", say, "in");
    b.link(say, "out", gone, "in");
    b.link(self, "id", gone, "id");
  } else if (id == "sign") {
    b.prop("line", T("Tombstone, 3 miles. Mind the rattlers."));
    b.prop("seconds", N(3));
    const int interact = b.add("event.interact", 0, 0);
    const int line = b.add("prop.get", 0, 160, {{"name", T("line")}});
    const int secs = b.add("prop.get", 0, 280, {{"name", T("seconds")}});
    const int say = b.add("toast", 260, 0);
    b.link(interact, "out", say, "in");
    b.link(line, "value", say, "text");
    b.link(secs, "value", say, "seconds");
  } else if (id == "ticker") {
    b.prop("every", N(2));
    b.prop("hop", N(8));
    const int start = b.add("event.start", 0, 0);
    const int every = b.add("prop.get", 0, 160, {{"name", T("every")}});
    const int hop = b.add("prop.get", 0, 280, {{"name", T("hop")}});
    const int down = b.add("math.neg", 240, 320);
    const int self = b.add("entity.self", 240, 200);
    const int wait_up = b.add("flow.delay", 240, 0);
    const int up = b.add("entity.move_by", 480, 0);
    const int wait_down = b.add("flow.delay", 720, 0);
    const int back = b.add("entity.move_by", 960, 0);
    b.link(start, "out", wait_up, "in");
    b.link(every, "value", wait_up, "seconds");
    b.link(wait_up, "out", up, "in");
    b.link(self, "id", up, "id");
    b.link(hop, "value", down, "a");
    b.link(down, "value", up, "dy");
    b.link(up, "out", wait_down, "in");
    b.link(every, "value", wait_down, "seconds");
    b.link(wait_down, "out", back, "in");
    b.link(self, "id", back, "id");
    b.link(hop, "value", back, "dy");
    b.link(back, "out", wait_up, "in");
  } else {
    return false;
  }
  std::sort(b.g.props.begin(), b.g.props.end(),
            [](const ScriptProp& x, const ScriptProp& y) { return x.name < y.name; });
  *out = std::move(b.g);
  return true;
}

// --- External editor state -------------------------------------------------------

std::mutex g_editor_mu;
ExternalEditor g_pref = ExternalEditor::Auto;
bool g_dry_run = false;
LaunchPlan g_last_launch;
bool g_vscode_probed = false, g_vs_probed = false;
EditorInstall g_vscode, g_vs;
bool g_vscode_override = false, g_vs_override = false;

std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

std::string find_on_path(const std::vector<std::string>& names) {
  const std::string path = env("PATH");
#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find(sep, start);
    const std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!dir.empty()) {
      for (const std::string& n : names) {
        std::error_code ec;
        const fs::path p = fs::path(dir) / n;
        if (fs::is_regular_file(p, ec)) return p.string();
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

#if defined(_WIN32)
std::string reg_string(HKEY root, const char* key, const char* value) {
  char buf[1024];
  DWORD size = sizeof(buf);
  if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
    return std::string(buf);
  }
  return {};
}

// Runs a command line, returns its stdout (vswhere).
std::string capture(const std::string& cmd) {
  std::string out;
  FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
  if (!p) return out;
  char buf[512];
  while (std::fgets(buf, sizeof(buf), p)) out += buf;
  _pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  return out;
}
#endif

EditorInstall probe_vscode() {
  EditorInstall r;
  std::error_code ec;
  if (const std::string e = env("TS_VSCODE"); !e.empty()) {
    r.found = fs::exists(e, ec);
    r.path = e;
    r.detail = r.found ? "TS_VSCODE" : "TS_VSCODE points at a missing file";
    return r;
  }
#if defined(_WIN32)
  // The CLI (bin\code.cmd) understands -g file; Code.exe is the fallback.
  std::vector<fs::path> dirs;
  for (const char* k : {"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{771FD6B0-FA20-440A-A002-3B3BAC16DC50}_is1",
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{EA457B21-F73E-494C-ACAB-524FDE069978}_is1",
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{D628A17A-9713-46BF-8D57-E671B46A741E}_is1",
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{1287CAD5-7C8D-410D-88B9-0D1EE4A83FF2}_is1"}) {
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
      const std::string loc = reg_string(root, k, "InstallLocation");
      if (!loc.empty()) dirs.emplace_back(loc);
    }
  }
  if (const std::string la = env("LOCALAPPDATA"); !la.empty()) dirs.push_back(fs::path(la) / "Programs" / "Microsoft VS Code");
  if (const std::string pf = env("ProgramFiles"); !pf.empty()) dirs.push_back(fs::path(pf) / "Microsoft VS Code");
  if (const std::string pf = env("ProgramFiles(x86)"); !pf.empty()) dirs.push_back(fs::path(pf) / "Microsoft VS Code");
  if (const std::string on_path = find_on_path({"code.cmd"}); !on_path.empty()) {
    r.found = true;
    r.path = on_path;
    r.detail = "PATH";
    return r;
  }
  for (const fs::path& d : dirs) {
    if (fs::exists(d / "bin" / "code.cmd", ec)) {
      r.found = true;
      r.path = (d / "bin" / "code.cmd").string();
      r.detail = d.string();
      return r;
    }
    if (fs::exists(d / "Code.exe", ec)) {
      r.found = true;
      r.path = (d / "Code.exe").string();
      r.detail = d.string();
      return r;
    }
  }
  r.detail = "VS Code not found (PATH, registry, %LOCALAPPDATA%\\Programs\\Microsoft VS Code)";
#else
  if (const std::string p = find_on_path({"code", "code-insiders", "codium"}); !p.empty()) {
    r.found = true;
    r.path = p;
    r.detail = "PATH";
    return r;
  }
#if defined(__APPLE__)
  const fs::path mac = "/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code";
  if (fs::exists(mac, ec)) {
    r.found = true;
    r.path = mac.string();
    r.detail = "/Applications";
    return r;
  }
#endif
  r.detail = "VS Code not found (`code` is not on PATH)";
#endif
  return r;
}

EditorInstall probe_vs() {
  EditorInstall r;
  std::error_code ec;
  if (const std::string e = env("TS_DEVENV"); !e.empty()) {
    r.found = fs::exists(e, ec);
    r.path = e;
    r.detail = r.found ? "TS_DEVENV" : "TS_DEVENV points at a missing file";
    return r;
  }
#if defined(_WIN32)
  std::string pf86 = env("ProgramFiles(x86)");
  if (pf86.empty()) pf86 = "C:\\Program Files (x86)";
  const fs::path vswhere = fs::path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
  if (fs::exists(vswhere, ec)) {
    const std::string dev = capture("\"" + vswhere.string() +
                                    "\" -latest -prerelease -products * -property productPath");
    if (!dev.empty() && fs::exists(dev, ec)) {
      r.found = true;
      r.path = dev;
      r.detail = "vswhere";
      return r;
    }
  }
  for (const char* ed : {"Community", "Professional", "Enterprise"}) {
    for (const char* ver : {"2022", "2019"}) {
      const std::string pf = env("ProgramFiles").empty() ? std::string("C:\\Program Files") : env("ProgramFiles");
      for (const std::string& base : {pf, pf86}) {
        const fs::path dev = fs::path(base) / "Microsoft Visual Studio" / ver / ed / "Common7" / "IDE" / "devenv.exe";
        if (fs::exists(dev, ec)) {
          r.found = true;
          r.path = dev.string();
          r.detail = "Program Files";
          return r;
        }
      }
    }
  }
  r.detail = "Visual Studio not found (vswhere found no install)";
#else
  r.detail = "Visual Studio is Windows only";
#endif
  return r;
}

bool launch(const LaunchPlan& plan, std::string* error) {
  {
    std::lock_guard<std::mutex> lk(g_editor_mu);
    g_last_launch = plan;
    if (g_dry_run) return true;
  }
#if defined(_WIN32)
  auto quote = [](const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    for (const char c : a) {
      if (c == '"') q += '\\';
      q += c;
    }
    return q + "\"";
  };
  std::string cmd;
  const bool batch = ends_with(plan.program, ".cmd") || ends_with(plan.program, ".bat");
  if (batch) {
    cmd = "cmd.exe /d /s /c \"" + quote(plan.program);
    for (const std::string& a : plan.args) cmd += " " + quote(a);
    cmd += "\"";
  } else {
    cmd = quote(plan.program);
    for (const std::string& a : plan.args) cmd += " " + quote(a);
  }
  std::wstring wcmd = fs::path(cmd).wstring();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const DWORD flags = batch ? CREATE_NO_WINDOW : 0;
  if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                      nullptr, &si, &pi)) {
    if (error) *error = "could not start " + plan.program + " (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  const pid_t child = fork();
  if (child < 0) {
    if (error) *error = "fork failed";
    return false;
  }
  if (child == 0) {
    const pid_t grandchild = fork();
    if (grandchild == 0) {
      setsid();
      std::vector<char*> argv;
      argv.push_back(const_cast<char*>(plan.program.c_str()));
      for (const std::string& a : plan.args) argv.push_back(const_cast<char*>(a.c_str()));
      argv.push_back(nullptr);
      execvp(plan.program.c_str(), argv.data());
      _exit(127);
    }
    _exit(grandchild < 0 ? 1 : 0);
  }
  int status = 0;
  pid_t waited = 0;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (error) *error = "could not start " + plan.program;
    return false;
  }
  return true;
#endif
}

std::string claim_file_name(const std::string& project_name, const std::string& project_dir) {
  std::string n = script_stem(project_name.empty() ? fs::path(project_dir).filename().string() : project_name);
  if (n.empty()) n = "claim";
  return n;
}

}  // namespace

// --- Info cache ----------------------------------------------------------------------

ScriptInfoCache::ScriptInfoCache() : lib_(std::make_unique<runtime::ScriptLibrary>()) {}
ScriptInfoCache::~ScriptInfoCache() = default;

void describe_script_into(runtime::ScriptLibrary& lib, const std::string& project_dir, const std::string& rel,
                          ScriptInfo* info) {
  if (!runtime::valid_script_path(rel)) {
    *info = ScriptInfo{};
    info->error = "scripts live under scripts/ and end in .lua, .graph.json, .cpp or .cs";
    return;
  }
  const runtime::ScriptLibrary::Entry* e = lib.get(project_dir, rel);
  if (!e) {
    *info = ScriptInfo{};
    info->error = "could not read " + rel;
    return;
  }
  if (info->version != 0 && info->version == e->version) return;
  const bool ok = e->ok;
  const std::string text = e->text;
  const std::string read_error = e->error;
  *info = ScriptInfo{};
  info->version = e->version;
  if (!ok) {
    info->error = read_error;
    return;
  }
  std::string err;
  info->ok = runtime::ScriptHost::describe(text, rel, &info->defaults, &err);
  info->error = err;
}

const ScriptInfo& ScriptInfoCache::get(const std::string& project_dir, const std::string& rel) {
  ScriptInfo& info = infos_[rel];
  describe_script_into(*lib_, project_dir, rel, &info);
  return info;
}

void ScriptInfoCache::clear() {
  lib_->clear();
  infos_.clear();
}

// --- Languages + templates -------------------------------------------------------

const char* lang_label(ScriptLang l) {
  switch (l) {
    case ScriptLang::Lua: return "Lua";
    case ScriptLang::Graph: return "Visual graph";
    case ScriptLang::Cpp: return "C++";
    case ScriptLang::CSharp: return "C#";
  }
  return "Lua";
}

const char* lang_tag(ScriptLang l) {
  switch (l) {
    case ScriptLang::Lua: return "Lua";
    case ScriptLang::Graph: return "Graph";
    case ScriptLang::Cpp: return "C++";
    case ScriptLang::CSharp: return "C#";
  }
  return "Lua";
}

const char* lang_ext(ScriptLang l) {
  switch (l) {
    case ScriptLang::Lua: return ".lua";
    case ScriptLang::Graph: return ".graph.json";
    case ScriptLang::Cpp: return ".cpp";
    case ScriptLang::CSharp: return ".cs";
  }
  return ".lua";
}

ScriptLang lang_of(const std::string& rel) {
  const std::string l = runtime::script_language(rel);
  if (l == "graph") return ScriptLang::Graph;
  if (l == "cpp") return ScriptLang::Cpp;
  if (l == "cs") return ScriptLang::CSharp;
  return ScriptLang::Lua;
}

std::string script_display_name(const std::string& rel) {
  std::string name = fs::path(rel).filename().string();
  for (const char* ext : {".graph.json", ".lua", ".cpp", ".cs"}) {
    if (ends_with(name, ext) && name.size() > std::strlen(ext)) {
      name.resize(name.size() - std::strlen(ext));
      break;
    }
  }
  return name;
}

const std::vector<ScriptTemplateInfo>& script_templates() {
  static const std::vector<ScriptTemplateInfo> t = {
      {"blank", "Blank", "Every hook stubbed out, ready to fill in"},
      {"gate", "Gate", "Opens on the action button; can shut itself"},
      {"pickup", "Pickup", "Pocketed by the first rider through its trigger"},
      {"sign", "Sign", "Shows a HUD line on the action button"},
      {"ticker", "Ticker", "Repeats on a timer (a tumbleweed hop)"},
  };
  return t;
}

std::string script_stem(const std::string& in) {
  std::string name = in;
  for (const char* ext : {".graph.json", ".lua", ".cpp", ".cs"}) {
    if (ends_with(name, ext) && name.size() > std::strlen(ext)) {
      name.resize(name.size() - std::strlen(ext));
      break;
    }
  }
  std::string out;
  for (const char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c == '_' || c == '-') {
      out.push_back(static_cast<char>(std::tolower(c)));
    } else if (c == ' ' && !out.empty() && out.back() != '_') {
      out.push_back('_');
    }
    if (out.size() >= 48) break;
  }
  while (!out.empty() && (out.back() == '_' || out.back() == '-')) out.pop_back();
  return out;
}

std::string script_class_name(const std::string& stem) {
  std::string out;
  bool up = true;
  for (const char ch : stem) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!std::isalnum(c)) {
      up = true;
      continue;
    }
    out.push_back(up ? static_cast<char>(std::toupper(c)) : static_cast<char>(c));
    up = false;
  }
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "Script" + out;
  return out;
}

bool script_template_source(ScriptLang lang, const std::string& template_id, const std::string& stem,
                            std::string* out) {
  const std::string cls = script_class_name(stem);
  auto fill = [&](const char* src) {
    *out = replace_all(replace_all(src, "{Class}", cls), "{stem}", stem);
    return true;
  };
  switch (lang) {
    case ScriptLang::Lua: {
      const sample_scripts::Template* t = sample_scripts::find_template(template_id);
      if (!t) return false;
      *out = t->source;
      return true;
    }
    case ScriptLang::Graph: {
      Graph g;
      if (!graph_template(template_id, stem, &g)) return false;
      *out = runtime::write_graph(g);
      return true;
    }
    case ScriptLang::Cpp:
      if (template_id == "blank") return fill(cpp_templates::kBlank);
      if (template_id == "gate") return fill(cpp_templates::kGate);
      if (template_id == "pickup") return fill(cpp_templates::kPickup);
      if (template_id == "sign") return fill(cpp_templates::kSign);
      if (template_id == "ticker") return fill(cpp_templates::kTicker);
      return false;
    case ScriptLang::CSharp:
      if (template_id == "blank") return fill(cs_templates::kBlank);
      if (template_id == "gate") return fill(cs_templates::kGate);
      if (template_id == "pickup") return fill(cs_templates::kPickup);
      if (template_id == "sign") return fill(cs_templates::kSign);
      if (template_id == "ticker") return fill(cs_templates::kTicker);
      return false;
  }
  return false;
}

NewScriptResult create_script_file(const std::string& project_dir, const std::string& name, ScriptLang lang,
                                   const std::string& template_id) {
  NewScriptResult r;
  if (project_dir.empty()) {
    r.message = "No claim folder to keep scripts in.";
    return r;
  }
  const std::string stem = script_stem(name);
  const std::string rel = std::string("scripts/") + stem + lang_ext(lang);
  if (stem.empty() || !runtime::valid_script_path(rel)) {
    r.message = "Give the script a name (letters, digits, _ or -).";
    return r;
  }
  std::string source;
  if (!script_template_source(lang, template_id, stem, &source)) {
    r.message = "No " + std::string(lang_label(lang)) + " template called " + template_id + ".";
    return r;
  }
  std::error_code ec;
  // One stem, one script: C++ / C# register by stem, and two languages
  // under one name would only confuse the Inspector.
  for (int i = 0; i < kScriptLangCount; ++i) {
    const std::string other = std::string("scripts/") + stem + lang_ext(static_cast<ScriptLang>(i));
    if (fs::exists(fs::path(project_dir) / other, ec)) {
      r.message = other + " already exists. Pick another name.";
      return r;
    }
  }
  const fs::path full = fs::path(project_dir) / rel;
  fs::create_directories(full.parent_path(), ec);
  {
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    out << source;
    if (!out) {
      r.message = "Could not write " + rel + ".";
      return r;
    }
  }
  r.ok = true;
  r.rel = rel;
  r.message = "Wrote " + rel + " (" + lang_label(lang) + ", from the " + template_id + " template)";
  if (lang == ScriptLang::CSharp && !runtime::csharp_scripts::sdk_available(nullptr)) {
    r.message += ". C# needs the .NET 8 SDK to run; it is saved and waits for it.";
  }
  return r;
}

std::vector<std::string> list_script_files(const std::string& project_dir, std::size_t max) {
  std::vector<std::string> out;
  if (project_dir.empty()) return out;
  const fs::path root(project_dir);
  std::error_code ec;
  if (!fs::is_directory(root / "scripts", ec)) return out;
  fs::recursive_directory_iterator it(root / "scripts", fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec)) continue;
    const std::string rel = fs::relative(it->path(), root, fec).generic_string();
    if (!fec && runtime::valid_script_path(rel)) {
      out.push_back(rel);
      if (out.size() >= max) break;
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// --- External editors ------------------------------------------------------------

const char* external_editor_label(ExternalEditor e) {
  switch (e) {
    case ExternalEditor::Auto: return "Automatic (VS Code, else Visual Studio, else the OS)";
    case ExternalEditor::VSCode: return "VS Code";
    case ExternalEditor::VisualStudio: return "Visual Studio";
    case ExternalEditor::Os: return "The OS default app";
  }
  return "Automatic";
}

const char* external_editor_name(ExternalEditor e) {
  switch (e) {
    case ExternalEditor::Auto: return "auto";
    case ExternalEditor::VSCode: return "vscode";
    case ExternalEditor::VisualStudio: return "vs";
    case ExternalEditor::Os: return "os";
  }
  return "auto";
}

ExternalEditor external_editor_from_name(const std::string& s) {
  if (s == "vscode" || s == "code") return ExternalEditor::VSCode;
  if (s == "vs" || s == "visualstudio") return ExternalEditor::VisualStudio;
  if (s == "os") return ExternalEditor::Os;
  return ExternalEditor::Auto;
}

void set_script_editor_preference(const std::string& pref) {
  std::lock_guard<std::mutex> lk(g_editor_mu);
  g_pref = external_editor_from_name(pref);
}

ExternalEditor script_editor_preference() {
  std::lock_guard<std::mutex> lk(g_editor_mu);
  return g_pref;
}

const EditorInstall& find_vscode(bool refresh) {
  std::lock_guard<std::mutex> lk(g_editor_mu);
  if (!g_vscode_override && (refresh || !g_vscode_probed)) {
    g_vscode = probe_vscode();
    g_vscode_probed = true;
  }
  return g_vscode;
}

const EditorInstall& find_visual_studio(bool refresh) {
  std::lock_guard<std::mutex> lk(g_editor_mu);
  if (!g_vs_override && (refresh || !g_vs_probed)) {
    g_vs = probe_vs();
    g_vs_probed = true;
  }
  return g_vs;
}

void override_editor_install(ExternalEditor which, const std::string& path) {
  std::lock_guard<std::mutex> lk(g_editor_mu);
  EditorInstall e;
  e.found = !path.empty();
  e.path = path;
  e.detail = path.empty() ? "not installed (test)" : "test";
  if (which == ExternalEditor::VSCode) {
    g_vscode = e;
    g_vscode_override = true;
  } else if (which == ExternalEditor::VisualStudio) {
    g_vs = e;
    g_vs_override = true;
  }
}

void set_external_launch_dry_run(bool on) {
  std::lock_guard<std::mutex> lk(g_editor_mu);
  g_dry_run = on;
}

const LaunchPlan& last_external_launch() { return g_last_launch; }

bool write_ide_files(const std::string& project_dir, const std::string& project_name, IdeFiles* out,
                     std::string* error) {
  IdeFiles files;
  std::error_code ec;
  const fs::path root = fs::absolute(project_dir, ec);
  if (project_dir.empty() || !fs::is_directory(root, ec)) {
    if (error) *error = "no claim folder at " + project_dir;
    return false;
  }
  const std::string stem = claim_file_name(project_name, project_dir);
  const std::string sdk_inc = runtime::native_scripts::sdk_include_dir();
  const std::string sdk_cs = runtime::csharp_scripts::binding_sources_dir();
  const std::vector<std::string> scripts = list_script_files(project_dir);
  bool any_cs = false, any_cpp = false;
  for (const std::string& s : scripts) {
    any_cs = any_cs || lang_of(s) == ScriptLang::CSharp;
    any_cpp = any_cpp || lang_of(s) == ScriptLang::Cpp;
  }
  std::string err;
  auto note_written = [&](const fs::path& p, bool wrote) {
    if (wrote) files.written.push_back(generic(p));
  };
  // Written once: the user may tune these.
  auto write_once = [&](const fs::path& p, const std::string& text) {
    if (fs::exists(p, ec)) return;
    note_written(p, write_if_changed(p, text, &err));
  };
  const std::string inc = sdk_inc.empty() ? std::string() : generic(fs::absolute(sdk_inc, ec));
  write_once(root / ".vscode" / "settings.json",
             "{\n"
             "  \"files.exclude\": { \".tombstone/native\": true, \".tombstone/dotnet\": true },\n"
             "  \"files.associations\": { \"*.graph.json\": \"json\" },\n"
             "  \"C_Cpp.default.cppStandard\": \"c++20\",\n"
             "  \"C_Cpp.default.includePath\": [\"" + json_escape(inc) + "\"],\n"
             "  \"dotnet.defaultSolution\": \"disable\"\n"
             "}\n");
  write_once(root / ".vscode" / "extensions.json",
             "{\n  \"recommendations\": [\"sumneko.lua\", \"ms-vscode.cpptools\", \"ms-dotnettools.csharp\"]\n}\n");
  write_once(root / ".vscode" / "c_cpp_properties.json",
             "{\n  \"configurations\": [\n    {\n      \"name\": \"TombStone scripts\",\n"
             "      \"includePath\": [\"${workspaceFolder}/scripts\", \"" + json_escape(inc) + "\"],\n"
             "      \"cppStandard\": \"c++20\",\n"
             "      \"compileCommands\": \"${workspaceFolder}/compile_commands.json\"\n    }\n  ],\n  \"version\": 4\n}\n");
  // Regenerated (paths follow the engine install).
  {
    const fs::path ws = root / (stem + ".code-workspace");
    note_written(ws, write_if_changed(ws,
                                      "{\n  \"folders\": [ { \"path\": \".\", \"name\": \"" +
                                          json_escape(project_name.empty() ? stem : project_name) +
                                          "\" } ],\n  \"settings\": { \"files.associations\": { \"*.graph.json\": "
                                          "\"json\" } }\n}\n",
                                      &err));
    files.workspace = generic(ws);
  }
  if (any_cpp || fs::exists(root / "compile_commands.json", ec)) {
    std::string cc = "[\n";
    bool first = true;
    for (const std::string& s : scripts) {
      if (lang_of(s) != ScriptLang::Cpp) continue;
      const std::string file = generic(root / s);
      cc += std::string(first ? "" : ",\n") + "  { \"directory\": \"" + json_escape(generic(root)) +
            "\", \"file\": \"" + json_escape(file) + "\", \"command\": \"c++ -std=c++20 -I\\\"" +
            json_escape(inc) + "\\\" -c \\\"" + json_escape(file) + "\\\"\" }";
      first = false;
    }
    cc += "\n]\n";
    note_written(root / "compile_commands.json", write_if_changed(root / "compile_commands.json", cc, &err));
    // Visual Studio "Open Folder" reads CppProperties.json.
    note_written(root / "CppProperties.json",
                 write_if_changed(root / "CppProperties.json",
                                  "{\n  \"configurations\": [\n    {\n      \"name\": \"TombStone scripts\",\n"
                                  "      \"inheritEnvironments\": [\"msvc_x64\"],\n"
                                  "      \"includePath\": [\"${workspaceRoot}\\\\scripts\", \"" +
                                      json_escape(inc) + "\"],\n"
                                      "      \"compilerSwitches\": \"/std:c++20\"\n    }\n  ]\n}\n",
                                  &err));
  }
  if (any_cs && !sdk_cs.empty()) {
    // Its own folder (.tombstone/ide), so the IDE's obj/ never lands in
    // scripts/ (the engine's build globs scripts/**/*.cs).
    const fs::path ide = root / ".tombstone" / "ide";
    for (const auto& e : fs::directory_iterator(sdk_cs, ec)) {
      const std::string ext = e.path().extension().string();
      if (ext == ".cs" || ext == ".csproj") {
        note_written(ide / "binding" / e.path().filename(),
                     write_if_changed(ide / "binding" / e.path().filename(), read_file(e.path()), &err));
      }
    }
    const std::string cls = script_class_name(stem);
    const fs::path proj = ide / (cls + ".Scripts.csproj");
    const std::string csproj =
        "<!-- Generated by TombStone so Visual Studio / VS Code understand this claim's\n"
        "     C# scripts (scripts/*.cs). The engine builds them itself; this project\n"
        "     is only for editing. Regenerated by \"Open in ...\". -->\n"
        "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
        "  <PropertyGroup>\n"
        "    <TargetFramework>net8.0</TargetFramework>\n"
        "    <AssemblyName>ProjectScripts</AssemblyName>\n"
        "    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n"
        "    <ImplicitUsings>disable</ImplicitUsings>\n"
        "    <Nullable>disable</Nullable>\n"
        "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n"
        "  </PropertyGroup>\n"
        "  <ItemGroup>\n"
        "    <Compile Include=\"../../scripts/**/*.cs\" />\n"
        "  </ItemGroup>\n"
        "  <ItemGroup>\n"
        "    <ProjectReference Include=\"binding/TombStone.Scripting.csproj\" />\n"
        "  </ItemGroup>\n"
        "</Project>\n";
    note_written(proj, write_if_changed(proj, csproj, &err));
    files.csproj = generic(proj);
  }
  if (!err.empty()) {
    if (error) *error = err;
    return false;
  }
  if (out) *out = std::move(files);
  return true;
}

bool plan_open_in_editor(ExternalEditor which, const std::string& project_dir, const std::string& project_name,
                         const std::string& rel, LaunchPlan* plan, std::string* error) {
  std::error_code ec;
  const fs::path file = fs::absolute(fs::path(project_dir) / rel, ec);
  if (rel.empty() || !fs::exists(file, ec)) {
    if (error) *error = "nothing to open at " + rel;
    return false;
  }
  IdeFiles ide;
  if (which == ExternalEditor::VSCode || which == ExternalEditor::VisualStudio) {
    std::string err;
    if (!write_ide_files(project_dir, project_name, &ide, &err)) {
      if (error) *error = "could not set the claim up for the IDE: " + err;
      return false;
    }
  }
  LaunchPlan p;
  if (which == ExternalEditor::VSCode) {
    const EditorInstall& vc = find_vscode();
    if (!vc.found) {
      if (error) *error = vc.detail.empty() ? "VS Code is not installed" : vc.detail;
      return false;
    }
    p.program = vc.path;
    p.args = {ide.workspace.empty() ? fs::absolute(project_dir, ec).string() : fs::path(ide.workspace).string(), "-g",
              file.string()};
  } else if (which == ExternalEditor::VisualStudio) {
    const EditorInstall& vs = find_visual_studio();
    if (!vs.found) {
      if (error) *error = vs.detail.empty() ? "Visual Studio is not installed" : vs.detail;
      return false;
    }
    p.program = vs.path;
    // C#: the generated project (IntelliSense against the SDK); otherwise
    // the claim folder (Open Folder reads CppProperties.json).
    const bool cs = lang_of(rel) == ScriptLang::CSharp && !ide.csproj.empty();
    p.args = {cs ? fs::path(ide.csproj).make_preferred().string() : fs::absolute(project_dir, ec).string(), "/Edit",
              file.string()};
  } else {
    if (error) *error = "no launch plan for the OS opener";
    return false;
  }
  if (plan) *plan = std::move(p);
  return true;
}

bool open_script_external(ExternalEditor which, const std::string& project_dir, const std::string& project_name,
                          const std::string& rel, std::string* message) {
  auto say = [&](const std::string& m) {
    if (message) *message = m;
  };
  if (project_dir.empty() || !runtime::valid_script_path(rel)) {
    say("Could not open " + rel + ": not a script under scripts/.");
    return false;
  }
  if (lang_of(rel) == ScriptLang::Graph && which == ExternalEditor::Auto) {
    if (!g_dry_run) BrandBook::get().open_graph(project_dir, project_name, rel);
    say("Opened " + rel + " in the Brand Book. Save it and the ride picks it up.");
    return true;
  }
  ExternalEditor pick = which;
  if (pick == ExternalEditor::Auto) pick = script_editor_preference();
  if (pick == ExternalEditor::Auto) {
    if (find_vscode().found) {
      pick = ExternalEditor::VSCode;
    } else if (find_visual_studio().found && lang_of(rel) != ScriptLang::Lua) {
      pick = ExternalEditor::VisualStudio;
    } else {
      pick = ExternalEditor::Os;
    }
  }
  if (pick == ExternalEditor::Os) {
    std::string err;
    const std::string full = (fs::path(project_dir) / rel).string();
    if (g_dry_run) {
      std::lock_guard<std::mutex> lk(g_editor_mu);
      g_last_launch = LaunchPlan{"(os)", {full}};
    } else if (!launcher::open_with_os(full, &err)) {
      say("Could not open " + rel + ": " + err);
      return false;
    }
    say("Opened " + rel + " in your editor. Save it and the ride picks it up.");
    return true;
  }
  LaunchPlan plan;
  std::string err;
  const char* who = pick == ExternalEditor::VSCode ? "VS Code" : "Visual Studio";
  if (!plan_open_in_editor(pick, project_dir, project_name, rel, &plan, &err)) {
    say(std::string("Could not open ") + rel + " in " + who + ": " + err +
        ". Pick another editor in Settings > Tools, or use Open (the OS default app).");
    return false;
  }
  if (!launch(plan, &err)) {
    say(std::string("Could not open ") + rel + " in " + who + ": " + err + ".");
    return false;
  }
  say(std::string("Opened ") + rel + " in " + who + " (claim folder + file). Save it and the ride picks it up.");
  return true;
}

// --- Inspector widgets -----------------------------------------------------------

ImVec4 lang_colour(ScriptLang l) {
  switch (l) {
    case ScriptLang::Lua: return theme::Accent();
    case ScriptLang::Graph: return theme::Copper();
    case ScriptLang::Cpp: return theme::Sand();
    case ScriptLang::CSharp: return theme::Success();
  }
  return theme::Accent();
}

namespace {

void open_menu_items(const ScriptStackHost& host, const std::string& rel) {
  auto run = [&](ExternalEditor e) {
    std::string msg;
    open_script_external(e, host.project_dir, host.project_name, rel, &msg);
    if (host.note) host.note(msg);
  };
  if (ux::MenuItem(lang_of(rel) == ScriptLang::Graph ? "Open in the Brand Book" : "Open")) run(ExternalEditor::Auto);
  const EditorInstall& vc = find_vscode();
  if (ux::MenuItem("Open in VS Code", nullptr, false, true)) run(ExternalEditor::VSCode);
  if (!vc.found && ImGui::IsItemHovered()) ux::SetTooltip("%s", vc.detail.c_str());
  const EditorInstall& vs = find_visual_studio();
  if (ux::MenuItem("Open in Visual Studio", nullptr, false, true)) run(ExternalEditor::VisualStudio);
  if (!vs.found && ImGui::IsItemHovered()) ux::SetTooltip("%s", vs.detail.c_str());
  if (lang_of(rel) != ScriptLang::Graph && ux::MenuItem("Open with the OS")) run(ExternalEditor::Os);
}

}  // namespace

StackResult draw_script_stack(std::vector<ScriptData>& stack, const ScriptStackHost& host) {
  StackResult r;
  auto commit = [&](const std::string& label) {
    r.change = StackChange::Commit;
    r.label = label;
  };
  int remove = -1, move_from = -1, move_to = -1;
  const int n = static_cast<int>(stack.size());
  for (int i = 0; i < n; ++i) {
    ScriptData& s = stack[static_cast<std::size_t>(i)];
    ImGui::PushID(i);
    const ScriptLang lang = lang_of(s.path);
    ImGui::BeginDisabled(host.read_only);
    bool on = s.enabled;
    if (ImGui::Checkbox("##on", &on)) {
      s.enabled = on;
      commit(on ? "Enable Script" : "Disable Script");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ux::SetTooltip(on ? "Runs when the ride starts (untick to switch it off)" : "Switched off: does not run");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    char head[256];
    std::snprintf(head, sizeof(head), "%s  (%s)%s###script_head", script_display_name(s.path).c_str(),
                  lang_tag(lang), s.enabled ? "" : "  - off");
    ImGui::PushStyleColor(ImGuiCol_Text, s.enabled ? lang_colour(lang) : theme::TextMuted());
    const bool open = ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopStyleColor();
    if (ImGui::BeginPopupContextItem("##script_ctx")) {
      ImGui::TextDisabled("%s", s.path.c_str());
      ImGui::Separator();
      open_menu_items(host, s.path);
      ImGui::Separator();
      ImGui::BeginDisabled(host.read_only);
      if (ux::MenuItem("Move up", nullptr, false, i > 0)) move_from = i, move_to = i - 1;
      if (ux::MenuItem("Move down", nullptr, false, i + 1 < n)) move_from = i, move_to = i + 1;
      if (ux::MenuItem("Reset props", nullptr, false, !s.props.empty())) {
        s.props.clear();
        commit("Reset Script Props");
      }
      if (ux::MenuItem("Remove script")) remove = i;
      ImGui::EndDisabled();
      ImGui::EndPopup();
    }
    // Drag a header onto another to reorder.
    if (!host.read_only && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      ImGui::SetDragDropPayload("TS_SCRIPT_SLOT", &i, sizeof(i));
      ImGui::Text("Move %s", script_display_name(s.path).c_str());
      ImGui::EndDragDropSource();
    }
    if (!host.read_only && ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TS_SCRIPT_SLOT")) {
        move_from = *static_cast<const int*>(p->Data);
        move_to = i;
      }
      ImGui::EndDragDropTarget();
    }
    // Header buttons: up / down / menu / remove.
    const float bw = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 4 - 6);
    ImGui::BeginDisabled(host.read_only || i == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move_from = i, move_to = i - 1;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ux::SetTooltip("Run earlier (move up)");
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(host.read_only || i + 1 >= n);
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move_from = i, move_to = i + 1;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ux::SetTooltip("Run later (move down)");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("...", ImVec2(bw, 0))) ImGui::OpenPopup("##script_more");
    if (ImGui::BeginPopup("##script_more")) {
      open_menu_items(host, s.path);
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(host.read_only);
    if (ImGui::Button("x", ImVec2(bw, 0))) remove = i;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ux::SetTooltip("Remove this script");

    if (open && remove != i) {
      ImGui::Indent(8.0f);
      ImGui::BeginDisabled(host.read_only);
      if (host.scripts && ImGui::BeginCombo("File", s.path.c_str())) {
        for (const std::string& rel : *host.scripts) {
          if (ImGui::Selectable(rel.c_str(), rel == s.path) && rel != s.path) {
            s.path = rel;
            commit("Script File");
          }
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
      // Open buttons right in the component (the header menu has them too).
      const char* open_label = lang == ScriptLang::Graph ? "Brand Book" : "Open";
      if (theme::SecondaryButton(open_label, ImVec2(86, 0))) {
        std::string msg;
        open_script_external(ExternalEditor::Auto, host.project_dir, host.project_name, s.path, &msg);
        if (host.note) host.note(msg);
      }
      if (ImGui::IsItemHovered()) ux::SetTooltip("Open %s (Settings > Tools picks the editor)", s.path.c_str());
      ImGui::SameLine();
      if (theme::SecondaryButton("VS Code", ImVec2(76, 0))) {
        std::string msg;
        open_script_external(ExternalEditor::VSCode, host.project_dir, host.project_name, s.path, &msg);
        if (host.note) host.note(msg);
      }
      if (ImGui::IsItemHovered()) ux::SetTooltip("Open in VS Code: %s", find_vscode().found ? find_vscode().path.c_str() : find_vscode().detail.c_str());
      ImGui::SameLine();
      if (theme::SecondaryButton("Visual Studio", ImVec2(110, 0))) {
        std::string msg;
        open_script_external(ExternalEditor::VisualStudio, host.project_dir, host.project_name, s.path, &msg);
        if (host.note) host.note(msg);
      }
      if (ImGui::IsItemHovered()) {
        ux::SetTooltip("Open in Visual Studio: %s",
                          find_visual_studio().found ? find_visual_studio().path.c_str() : find_visual_studio().detail.c_str());
      }

      static const ScriptInfo kNoInfo{};
      const ScriptInfo& info = host.info ? host.info(s.path) : kNoInfo;
      if (!info.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
        ImGui::TextWrapped("%s", info.error.empty() ? "Script does not run" : info.error.c_str());
        ImGui::PopStyleColor();
      } else if (info.defaults.empty()) {
        ImGui::TextDisabled("No props. Declare some in the file to tune them here.");
      }
      ImGui::BeginDisabled(host.read_only);
      ux::AutoTipSection("ScriptProp");  // prop names come from the script itself
      for (const ScriptProp& d : info.defaults) {
        const ScriptProp* o = s.find(d.name);
        const bool overridden = o && o->value.type == d.value.type;
        const ScriptValue v = overridden ? o->value : d.value;
        ImGui::PushID(d.name.c_str());
        if (!d.header.empty()) {
          ImGui::Spacing();
          ImGui::TextDisabled("%s", d.header.c_str());
        }
        if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
        if (d.widget == "color") {
          float c[4] = {1, 1, 1, 1};
          std::sscanf(v.text.c_str(), "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]);
          if (ImGui::ColorEdit4(d.name.c_str(), c, ImGuiColorEditFlags_Float)) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g,%.6g", c[0], c[1], c[2], c[3]);
            s.set(d.name, ScriptValue::of_text(buf));
            if (r.change == StackChange::None) r.change = StackChange::Live;
          }
        } else if (d.widget == "enum" && !d.options.empty()) {
          std::vector<std::string> opts;
          std::string cur;
          for (char ch : d.options + ",") {
            if (ch == ',') {
              if (!cur.empty()) opts.push_back(cur);
              cur.clear();
            } else {
              cur.push_back(ch);
            }
          }
          int cur_i = 0;
          for (int i = 0; i < static_cast<int>(opts.size()); ++i)
            if (opts[static_cast<std::size_t>(i)] == v.text) cur_i = i;
          if (ImGui::BeginCombo(d.name.c_str(), opts.empty() ? "" : opts[static_cast<std::size_t>(cur_i)].c_str())) {
            for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
              if (ImGui::Selectable(opts[static_cast<std::size_t>(i)].c_str(), i == cur_i)) {
                s.set(d.name, ScriptValue::of_text(opts[static_cast<std::size_t>(i)]));
                commit("Script Prop: " + d.name);
              }
            }
            ImGui::EndCombo();
          }
        } else
        switch (d.value.type) {
          case ScriptValue::Type::Bool: {
            bool b = v.flag;
            if (ImGui::Checkbox(d.name.c_str(), &b)) {
              s.set(d.name, ScriptValue::of_bool(b));
              commit("Script Prop: " + d.name);
            }
            break;
          }
          case ScriptValue::Type::Number: {
            double x = v.number;
            const double rmin = d.range_min, rmax = d.range_max;
            if (d.has_range) x = std::clamp(x, rmin, rmax);
            if (ImGui::DragScalar(d.name.c_str(), ImGuiDataType_Double, &x, 0.1f, d.has_range ? &rmin : nullptr,
                                  d.has_range ? &rmax : nullptr, "%.6g")) {
              s.set(d.name, ScriptValue::of_number(x));
              if (r.change == StackChange::None) r.change = StackChange::Live;
            }
            if (host.after_item) host.after_item("Script Prop");
            break;
          }
          case ScriptValue::Type::Text: {
            char buf[ScriptData::kMaxTextLength + 1];
            std::snprintf(buf, sizeof(buf), "%s", v.text.c_str());
            if (ImGui::InputText(d.name.c_str(), buf, sizeof(buf))) {
              s.set(d.name, ScriptValue::of_text(buf));
              if (r.change == StackChange::None) r.change = StackChange::Live;
            }
            if (host.after_item) host.after_item("Script Prop");
            break;
          }
        }
        ux::TipText(d.name + ": a prop declared in " + s.path + " (file default " + describe(d.value) +
                    ").\nTuned here per object; amber means changed from the file." +
                    (d.tooltip.empty() ? "" : "\n" + d.tooltip));
        if (overridden) {
          ImGui::PopStyleColor();
          ImGui::SameLine();
          if (ImGui::SmallButton("default")) {
            s.erase(d.name);
            commit("Reset Prop: " + d.name);
          }
          if (ImGui::IsItemHovered()) ux::SetTooltip("Back to the file's %s", describe(d.value).c_str());
        }
        ImGui::PopID();
      }
      // Overrides the file no longer declares (renamed / removed props).
      if (info.ok) {
        std::vector<std::string> stale;
        for (const ScriptProp& p : s.props) {
          const bool declared = std::any_of(info.defaults.begin(), info.defaults.end(),
                                            [&p](const ScriptProp& d) { return d.name == p.name; });
          if (!declared) stale.push_back(p.name);
        }
        for (const std::string& name : stale) {
          ImGui::PushID(name.c_str());
          ImGui::TextDisabled("%s (not in the file any more)", name.c_str());
          ImGui::SameLine();
          if (ImGui::SmallButton("drop")) {
            s.erase(name);
            commit("Reset Prop: " + name);
          }
          ImGui::PopID();
        }
      }
      ImGui::EndDisabled();
      ux::AutoTipSection("Script");
      ImGui::Unindent(8.0f);
    }
    ImGui::PopID();
  }
  if (remove >= 0 && remove < n) {
    const std::string name = script_display_name(stack[static_cast<std::size_t>(remove)].path);
    stack.erase(stack.begin() + remove);
    commit("Remove Script " + name);
  } else if (move_from >= 0 && move_to >= 0 && move_from < n && move_to < n && move_from != move_to) {
    ScriptData moving = std::move(stack[static_cast<std::size_t>(move_from)]);
    stack.erase(stack.begin() + move_from);
    stack.insert(stack.begin() + move_to, std::move(moving));
    commit("Reorder Scripts");
  }
  return r;
}

std::string AddScriptMenu::draw_items(const ScriptStackHost& host) {
  std::string picked;
  if (ux::MenuItem("New Script...", nullptr, false, !host.read_only)) {
    want_modal = true;
    error.clear();
  }
  if (ux::BeginMenu("Script", !host.read_only)) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##script_search", "search scripts", search, sizeof(search));
    std::string q = search;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    int shown = 0;
    if (host.scripts) {
      for (const std::string& rel : *host.scripts) {
        std::string low = rel;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!q.empty() && low.find(q) == std::string::npos) continue;
        char label[300];
        std::snprintf(label, sizeof(label), "%-6s %s", lang_tag(lang_of(rel)), rel.c_str());
        if (ux::MenuItem(label)) picked = rel;
        ++shown;
      }
    }
    if (shown == 0) {
      ImGui::TextDisabled(host.scripts && !host.scripts->empty() ? "Nothing matches." : "No scripts yet: New Script...");
    }
    ux::EndMenu();
  }
  return picked;
}

std::string AddScriptMenu::draw_modal(const ScriptStackHost& host) {
  std::string created;
  if (want_modal) {
    ImGui::OpenPopup("New Script##ts_new_script");
    want_modal = false;
  }
  ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("New Script##ts_new_script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("Name", "e.g. gate_keeper", name, sizeof(name));
    lang = std::clamp(lang, 0, kScriptLangCount - 1);
    ImGui::TextUnformatted("Language");
    for (int i = 0; i < kScriptLangCount; ++i) {
      if (i > 0) ImGui::SameLine();
      ImGui::RadioButton(lang_label(static_cast<ScriptLang>(i)), &lang, i);
    }
    const auto& tmpl = script_templates();
    templ = std::clamp(templ, 0, static_cast<int>(tmpl.size()) - 1);
    if (ImGui::BeginCombo("Template", tmpl[static_cast<std::size_t>(templ)].label)) {
      for (int i = 0; i < static_cast<int>(tmpl.size()); ++i) {
        if (ImGui::Selectable(tmpl[static_cast<std::size_t>(i)].label, i == templ)) templ = i;
        if (ImGui::IsItemHovered()) ux::SetTooltip("%s", tmpl[static_cast<std::size_t>(i)].blurb);
      }
      ImGui::EndCombo();
    }
    const std::string stem = script_stem(name);
    const ScriptLang l = static_cast<ScriptLang>(lang);
    ImGui::TextDisabled("Writes scripts/%s%s", stem.empty() ? "<name>" : stem.c_str(), lang_ext(l));
    if (l == ScriptLang::CSharp) {
      static int cs_ok = -1;
      if (cs_ok < 0) cs_ok = runtime::csharp_scripts::sdk_available(nullptr) ? 1 : 0;
      if (!cs_ok) theme::StatusWarn("No .NET 8 SDK here: the file is written but C# waits for the SDK to run.");
    } else if (l == ScriptLang::Cpp) {
      static int cpp_ok = -1;
      if (cpp_ok < 0) cpp_ok = runtime::native_scripts::toolchain_available(nullptr) ? 1 : 0;
      if (!cpp_ok) theme::StatusWarn("No C++ toolchain found: the file is written; it builds once one is installed.");
    }
    if (!error.empty()) theme::StatusError(error.c_str());
    ImGui::BeginDisabled(stem.empty());
    if (theme::PrimaryButton("Create and Attach", ImVec2(160, 0))) {
      const NewScriptResult res =
          create_script_file(host.project_dir, name, l, tmpl[static_cast<std::size_t>(templ)].id);
      if (res.ok) {
        created = res.rel;
        if (host.note) host.note(res.message);
        name[0] = '\0';
        error.clear();
        ImGui::CloseCurrentPopup();
      } else {
        error = res.message;
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme::SecondaryButton("Cancel", ImVec2(90, 0))) {
      error.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  return created;
}

void draw_scripts_panel_body(ScriptsPanelState& st, const ScriptsPanelHost& host) {
  const auto& tmpl = script_templates();
  *st.templ = std::clamp(*st.templ, 0, static_cast<int>(tmpl.size()) - 1);
  *st.lang = std::clamp(*st.lang, 0, kScriptLangCount - 1);
  const ScriptLang lang = static_cast<ScriptLang>(*st.lang);
  char caption[64];
  std::snprintf(caption, sizeof(caption), "scripts/<name>%s", lang_ext(lang));
  theme::SectionHeader("Write a script", caption);
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##new_script", "name, e.g. gate_keeper", st.name, st.name_size);
  const float half = (ImGui::GetContentRegionAvail().x - 90.0f - ImGui::GetStyle().ItemSpacing.x * 2) * 0.5f;
  ImGui::SetNextItemWidth(std::max(60.0f, half));
  if (ImGui::BeginCombo("##lang", lang_label(lang))) {
    for (int i = 0; i < kScriptLangCount; ++i) {
      if (ImGui::Selectable(lang_label(static_cast<ScriptLang>(i)), i == *st.lang)) *st.lang = i;
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(std::max(60.0f, half));
  if (ImGui::BeginCombo("##template", tmpl[static_cast<std::size_t>(*st.templ)].label)) {
    for (int i = 0; i < static_cast<int>(tmpl.size()); ++i) {
      if (ImGui::Selectable(tmpl[static_cast<std::size_t>(i)].label, i == *st.templ)) *st.templ = i;
      if (ImGui::IsItemHovered()) ux::SetTooltip("%s", tmpl[static_cast<std::size_t>(i)].blurb);
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(st.name[0] == '\0' || !host.create);
  if (theme::PrimaryButton("Write", ImVec2(82, 0))) {
    if (host.create(st.name, lang, tmpl[static_cast<std::size_t>(*st.templ)].id)) st.name[0] = '\0';
  }
  ImGui::EndDisabled();

  if (theme::SecondaryButton("Brand Book (visual scripts)", ImVec2(-1, 0))) {
    BrandBook::get().open(host.stack.project_dir, host.stack.project_name);
  }
  theme::SectionHeader("Scripts", host.playing ? "saves hot-reload mid-ride" : "drag onto an entity to attach");
  const std::vector<std::string> none;
  const std::vector<std::string>& scripts = host.stack.scripts ? *host.stack.scripts : none;
  if (scripts.empty()) {
    ux::EmptyState("scripts", {{"Write a starter script", [&] {
                                  if (host.create) host.create(st.name[0] ? st.name : "my_script", lang,
                                                               tmpl[static_cast<std::size_t>(*st.templ)].id);
                                }, "Writes scripts/my_script with the template picked above (or the name you typed)", host.create != nullptr},
                               {"Brand Book", [&] { BrandBook::get().open(host.stack.project_dir, host.stack.project_name); },
                                "Visual scripting: wire nodes instead of writing code"}});
  }
  const float list_h =
      std::min(220.0f, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::max<std::size_t>(scripts.size(), 1)) + 8.0f);
  ImGui::BeginChild("##script_list", ImVec2(0, list_h), ImGuiChildFlags_Borders);
  for (const std::string& rel : scripts) {
    const ScriptLang l = lang_of(rel);
    ImGui::PushID(rel.c_str());
    ImGui::TextColored(lang_colour(l), "%-5s", lang_tag(l));
    ImGui::SameLine(52.0f);
    if (ImGui::Selectable(rel.c_str(), rel == *st.selected, ImGuiSelectableFlags_AllowDoubleClick)) {
      *st.selected = rel;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        std::string msg;
        open_script_external(ExternalEditor::Auto, host.stack.project_dir, host.stack.project_name, rel, &msg);
        if (host.stack.note) host.stack.note(msg);
      }
    }
    script_drag_source(rel);
    if (ImGui::BeginPopupContextItem("##ctx")) {
      *st.selected = rel;
      open_menu_items(host.stack, rel);
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  const std::string rel = *st.selected;
  if (rel.empty() || std::find(scripts.begin(), scripts.end(), rel) == scripts.end()) return;
  const ScriptLang l = lang_of(rel);
  if (l == ScriptLang::CSharp) {
    static int cs_ok = -1;
    if (cs_ok < 0) cs_ok = runtime::csharp_scripts::sdk_available(nullptr) ? 1 : 0;
    if (!cs_ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
      ImGui::TextWrapped("C# scripts are switched off on this machine. Install the .NET 8 SDK and they'll come good. Lua, graphs and C++ keep working.");
      ImGui::PopStyleColor();
      ImGui::TextDisabled("https://dotnet.microsoft.com/download/dotnet/8.0");
    }
  }
  if (host.stack.info) {
    const ScriptInfo& info = host.stack.info(rel);
    if (info.ok) {
      std::string props;
      for (const ScriptProp& p : info.defaults) props += (props.empty() ? "" : ", ") + p.name + " = " + describe(p.value);
      ImGui::TextColored(theme::Success(), "Rides clean.");
      if (!props.empty()) ImGui::TextWrapped("props: %s", props.c_str());
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
      ImGui::TextWrapped("%s", info.error.c_str());
      ImGui::PopStyleColor();
    }
  }
  auto open_with = [&](ExternalEditor e) {
    std::string msg;
    open_script_external(e, host.stack.project_dir, host.stack.project_name, rel, &msg);
    if (host.stack.note) host.stack.note(msg);
  };
  if (theme::SecondaryButton(l == ScriptLang::Graph ? "Brand Book##open" : "Open in Editor", ImVec2(120, 0))) {
    open_with(ExternalEditor::Auto);
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("VS Code##open", ImVec2(80, 0))) open_with(ExternalEditor::VSCode);
  if (ImGui::IsItemHovered()) {
    const EditorInstall& vc = find_vscode();
    ux::SetTooltip("Open the claim folder on this file in VS Code\n%s", vc.found ? vc.path.c_str() : vc.detail.c_str());
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Visual Studio##open", ImVec2(110, 0))) open_with(ExternalEditor::VisualStudio);
  if (ImGui::IsItemHovered()) {
    const EditorInstall& vs = find_visual_studio();
    ux::SetTooltip("Open the claim (C#: its scripts project) on this file in Visual Studio\n%s",
                      vs.found ? vs.path.c_str() : vs.detail.c_str());
  }
  const bool can_attach = !host.playing && !host.target_name.empty() && host.attach &&
                          !(host.target_has && host.target_has(rel));
  ImGui::BeginDisabled(!can_attach);
  char attach[96];
  std::snprintf(attach, sizeof(attach), "Attach to %s", host.target_name.empty() ? "selection" : host.target_name.c_str());
  if (theme::CopperButton(attach)) host.attach(rel);
  ImGui::EndDisabled();
}

std::string accept_script_drop() {
  std::string rel;
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kScriptPayload)) {
      rel.assign(static_cast<const char*>(p->Data));
    }
    ImGui::EndDragDropTarget();
  }
  return rel;
}

bool dragging_script() {
  const ImGuiPayload* p = ImGui::GetDragDropPayload();
  return p && p->IsDataType(kScriptPayload);
}

std::string accept_script_payload() {
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kScriptPayload)) {
    return std::string(static_cast<const char*>(p->Data));
  }
  return {};
}

void script_drag_source(const std::string& rel) {
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload(kScriptPayload, rel.c_str(), rel.size() + 1);
    ImGui::Text("Script: %s", rel.c_str());
    ImGui::TextDisabled("drop on an entity to attach");
    ImGui::EndDragDropSource();
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
