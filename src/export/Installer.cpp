#include "export/Installer.h"

#include "core/JsonMini.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace exporter {

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_all(const fs::path& p, const std::string& s) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o << s;
  return static_cast<bool>(o);
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Inno Setup: '{' starts a constant ("{{" is a literal brace); quoted
// [Files] / [Icons] values double their quotes.
std::string inno(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '{') out += "{{";
    else if (c == '"') out += "\"\"";
    else if (c == '\r' || c == '\n') out += ' ';
    else out += c;
  }
  return out;
}

// Pascal string literal inside [Code].
std::string pascal(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "''";
    else if (c == '\r' || c == '\n') out += ' ';
    else out += c;
  }
  return out + "'";
}

std::string win_path(const std::string& p) {
  std::string s = fs::path(p).lexically_normal().string();
  std::replace(s.begin(), s.end(), '/', '\\');
  return s;
}

std::string quote_cmd(const std::string& p) { return "\"" + p + "\""; }

}  // namespace

// ---------------------------------------------------------------- settings

ExportSettings load_settings(const std::string& project_dir) {
  ExportSettings s;
  const auto obj = json_mini::parse_object(read_all(fs::path(project_dir) / kSettingsFile));
  if (!obj) return s;
  s.icon = json_mini::get_string(*obj, "icon", "");
  s.version = json_mini::get_string(*obj, "version", "");
  s.publisher = json_mini::get_string(*obj, "publisher", "");
  s.license = json_mini::get_string(*obj, "license", "");
  s.install_mode = json_mini::get_string(*obj, "install_mode", "ask");
  if (s.install_mode != "user" && s.install_mode != "all") s.install_mode = "ask";
  s.installer = json_mini::get_bool(*obj, "installer", false);
  s.portable_zip = json_mini::get_bool(*obj, "portable_zip", true);
  s.start_menu = json_mini::get_bool(*obj, "start_menu", true);
  s.desktop_icon = json_mini::get_bool(*obj, "desktop_icon", true);
  return s;
}

bool save_settings(const std::string& project_dir, const ExportSettings& s, std::string* error) {
  const std::string text = json_mini::write_object(
      {{"icon", s.icon},
       {"version", s.version},
       {"publisher", s.publisher},
       {"license", s.license},
       {"install_mode", s.install_mode}},
      {{"installer", s.installer},
       {"portable_zip", s.portable_zip},
       {"start_menu", s.start_menu},
       {"desktop_icon", s.desktop_icon}});
  if (!write_all(fs::path(project_dir) / kSettingsFile, text)) {
    if (error) *error = "cannot write " + (fs::path(project_dir) / kSettingsFile).string();
    return false;
  }
  return true;
}

// --------------------------------------------------------------------- ico

bool write_ico_from_png(const std::string& png, const std::string& ico, std::string* error) {
  const std::string bytes = read_all(png);
  if (bytes.size() < 24 || bytes.compare(1, 3, "PNG") != 0) {
    if (error) *error = png + " is not a PNG";
    return false;
  }
  auto be32 = [&](std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
  };
  const std::uint32_t w = be32(16), h = be32(20);
  std::string out;
  auto u8 = [&](unsigned v) { out.push_back(static_cast<char>(v & 0xff)); };
  auto u16 = [&](unsigned v) { u8(v); u8(v >> 8); };
  auto u32 = [&](std::uint32_t v) { u16(v & 0xffff); u16(v >> 16); };
  u16(0); u16(1); u16(1);                       // ICONDIR: reserved, type icon, 1 image
  u8(w >= 256 ? 0 : w); u8(h >= 256 ? 0 : h);   // ICONDIRENTRY
  u8(0); u8(0); u16(1); u16(32);
  u32(static_cast<std::uint32_t>(bytes.size()));
  u32(6 + 16);
  out += bytes;
  if (!write_all(ico, out)) {
    if (error) *error = "cannot write " + ico;
    return false;
  }
  return true;
}

// ------------------------------------------------------------------- ISCC

std::string find_iscc() {
  std::error_code ec;
  std::vector<fs::path> c;
  if (const char* e = std::getenv("TS_ISCC")) c.emplace_back(e);
#if defined(_WIN32)
  const char* exe = "ISCC.exe";
  for (const char* var : {"ProgramFiles(x86)", "ProgramFiles"}) {
    if (const char* p = std::getenv(var)) c.push_back(fs::path(p) / "Inno Setup 6" / exe);
  }
  if (const char* p = std::getenv("LOCALAPPDATA")) {
    c.push_back(fs::path(p) / "Programs" / "Inno Setup 6" / exe);
  }
  const char sep = ';';
#else
  const char* exe = "iscc";
  const char sep = ':';
#endif
  if (const char* path = std::getenv("PATH")) {
    std::string p = path;
    std::size_t start = 0;
    while (start <= p.size()) {
      const std::size_t end = p.find(sep, start);
      const std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!dir.empty()) c.push_back(fs::path(dir) / exe);
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  for (const fs::path& p : c) {
    if (fs::is_regular_file(p, ec)) return p.string();
  }
  return {};
}

std::string inno_script(const InstallerSpec& s) {
  const std::string exe = inno(s.exe_name);
  const std::string name = inno(s.name);
  std::ostringstream o;
  o << "; " << s.name << " " << s.version << " - Windows installer written by the TombStone\n"
    << "; Freight Office. Rebuild with: ISCC.exe \"" << s.slug << "-installer.iss\"\n\n"
    << "[Setup]\n"
    << "AppId=TombStone." << inno(s.slug) << "\n"
    << "AppName=" << name << "\n"
    << "AppVersion=" << inno(s.version) << "\n"
    << "AppVerName=" << name << " " << inno(s.version) << "\n";
  if (!s.publisher.empty()) o << "AppPublisher=" << inno(s.publisher) << "\n";
  o << "VersionInfoVersion=" << [&] {
    // x.y.z.w digits only (Inno rejects "1.0.0-beta").
    std::string v;
    int parts = 0;
    std::string cur;
    for (char c : s.version + ".") {
      if (std::isdigit(static_cast<unsigned char>(c))) {
        cur += c;
        continue;
      }
      if (parts < 4 && (c == '.' || !cur.empty())) {
        v += (parts ? "." : "") + (cur.empty() ? std::string("0") : cur);
      }
      ++parts;
      cur.clear();
      if (c != '.') break;
    }
    return v.empty() ? std::string("1.0.0") : v;
  }() << "\n"
    << "DefaultDirName={autopf}\\" << name << "\n"
    << "DefaultGroupName=" << name << "\n"
    << "DisableProgramGroupPage=yes\n"
    << "UninstallDisplayIcon={app}\\" << exe << "\n"
    << "UninstallDisplayName=" << name << "\n"
    << "OutputDir=" << inno(win_path(s.out_dir)) << "\n"
    << "OutputBaseFilename=" << inno(s.slug + "-" + s.version + "-setup") << "\n"
    << "Compression=lzma2\nSolidCompression=yes\nWizardStyle=modern\n"
    << "ArchitecturesAllowed=x64compatible\nArchitecturesInstallIn64BitMode=x64compatible\n"
    << "MinVersion=10.0\n";
  if (s.install_mode == "all") {
    o << "PrivilegesRequired=admin\nPrivilegesRequiredOverridesAllowed=commandline\n";
  } else if (s.install_mode == "user") {
    o << "PrivilegesRequired=lowest\nPrivilegesRequiredOverridesAllowed=commandline\n";
  } else {
    // Ask: per-user by default, "Install for all users" on the first page.
    o << "PrivilegesRequired=lowest\nPrivilegesRequiredOverridesAllowed=dialog commandline\n";
  }
  if (!s.icon_ico.empty()) o << "SetupIconFile=" << inno(win_path(s.icon_ico)) << "\n";
  if (!s.license_file.empty()) o << "LicenseFile=" << inno(win_path(s.license_file)) << "\n";
  o << "\n[Languages]\nName: \"english\"; MessagesFile: \"compiler:Default.isl\"\n";
  if (s.desktop_icon) {
    o << "\n[Tasks]\nName: \"desktopicon\"; Description: \"{cm:CreateDesktopIcon}\"; "
         "GroupDescription: \"{cm:AdditionalIcons}\"\n";
  }
  o << "\n[Files]\nSource: \"" << inno(win_path(s.game_dir)) << "\\*\"; DestDir: \"{app}\"; "
    << "Excludes: \"export_report.txt,smoke.log,telegraph.log\"; "
    << "Flags: ignoreversion recursesubdirs createallsubdirs\n";
  o << "\n[Icons]\n";
  if (s.start_menu) {
    o << "Name: \"{autoprograms}\\" << name << "\"; Filename: \"{app}\\" << exe
      << "\"; WorkingDir: \"{app}\"\n";
  }
  if (s.desktop_icon) {
    o << "Name: \"{autodesktop}\\" << name << "\"; Filename: \"{app}\\" << exe
      << "\"; WorkingDir: \"{app}\"; Tasks: desktopicon\n";
  }
  o << "\n[Run]\nFilename: \"{app}\\" << exe << "\"; Description: \"{cm:LaunchProgram,"
    << name << "}\"; Flags: nowait postinstall skipifsilent\n";
  // GPU gate: the runtime needs OpenGL 3.3. The game checks itself too
  // (friendly box at start); this tells the player at install time.
  o << "\n[Code]\n"
    << "procedure CurStepChanged(CurStep: TSetupStep);\n"
    << "var\n  Code: Integer;\n"
    << "begin\n"
    << "  if CurStep = ssPostInstall then begin\n"
    << "    if Exec(ExpandConstant('{app}\\" << s.exe_name << "'), '--check-gpu --quiet', "
    << "ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then begin\n"
    << "      Log('GPU check exit code ' + IntToStr(Code));\n"
    << "      if Code <> 0 then\n"
    << "        SuppressibleMsgBox(" << pascal(s.name)
    << " + ' needs a graphics card with OpenGL 3.3 or newer, and this PC''s driver did not offer it.'"
    << " + #13#10#13#10 + 'The game is installed, but it may not start. Updating the graphics driver (NVIDIA, AMD or Intel) usually fixes this.', "
    << "mbInformation, MB_OK, IDOK);\n"
    << "    end;\n"
    << "  end;\n"
    << "end;\n";
  return o.str();
}

bool build_installer(const InstallerSpec& s, std::string* iss_path, std::string* setup_exe,
                     std::string* log, std::string* error) {
  std::error_code ec;
  const fs::path out = fs::absolute(fs::path(s.out_dir), ec);
  const fs::path iss = out / (s.slug + "-installer.iss");
  if (!write_all(iss, inno_script(s))) {
    *error = "cannot write " + iss.string();
    return false;
  }
  *iss_path = iss.string();
  const std::string iscc = find_iscc();
  if (iscc.empty()) {
    *error = "ISCC not found (install Inno Setup 6: tools/installer/install_inno.ps1, or set TS_ISCC)";
    return false;
  }
  const fs::path exe = out / (s.slug + "-" + s.version + "-setup.exe");
  fs::remove(exe, ec);
  const fs::path log_file = out / (s.slug + "-installer.log");
  std::string cmd = quote_cmd(iscc) + " /Q " + quote_cmd(iss.string()) + " > " +
                    quote_cmd(log_file.string()) + " 2>&1";
#if defined(_WIN32)
  cmd = "\"" + cmd + "\"";
#endif
  const int rc = std::system(cmd.c_str());
  if (log) *log = read_all(log_file);
  if (rc != 0 || !fs::is_regular_file(exe, ec)) {
    *error = "ISCC failed (exit " + std::to_string(rc) + "), see " + log_file.string();
    return false;
  }
  fs::remove(log_file, ec);
  *setup_exe = exe.string();
  return true;
}

// ------------------------------------------------------------- PE imports

std::vector<std::string> pe_imports(const std::string& path, std::string* error) {
  std::vector<std::string> out;
  const std::string b = read_all(path);
  auto rd = [&](std::size_t at, int n) -> std::uint32_t {
    if (at + n > b.size()) return 0;
    std::uint32_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b[at + i]);
    return v;
  };
  if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') {
    if (error) *error = path + ": not a PE file";
    return out;
  }
  const std::size_t pe = rd(0x3c, 4);
  if (pe + 24 > b.size() || b.compare(pe, 4, std::string("PE\0\0", 4)) != 0) {
    if (error) *error = path + ": no PE header";
    return out;
  }
  const unsigned nsec = rd(pe + 6, 2);
  const unsigned opt_size = rd(pe + 20, 2);
  const std::size_t opt = pe + 24;
  const unsigned magic = rd(opt, 2);
  const std::size_t dirs = opt + (magic == 0x20b ? 112 : 96);
  const std::size_t sec = opt + opt_size;
  auto off = [&](std::uint32_t rva) -> std::size_t {
    for (unsigned i = 0; i < nsec; ++i) {
      const std::size_t h = sec + i * 40;
      const std::uint32_t va = rd(h + 12, 4), vs = std::max(rd(h + 8, 4), rd(h + 16, 4));
      if (rva >= va && rva < va + vs) return rd(h + 20, 4) + (rva - va);
    }
    return 0;
  };
  auto cstr = [&](std::size_t at) {
    std::string s;
    while (at < b.size() && b[at] && s.size() < 260) s += b[at++];
    return s;
  };
  // Directory 1: imports (20-byte descriptors, name RVA at +12).
  if (std::size_t at = off(rd(dirs + 8, 4)); at) {
    for (; at + 20 <= b.size(); at += 20) {
      const std::uint32_t name = rd(at + 12, 4);
      if (!name && !rd(at, 4)) break;
      if (const std::size_t n = off(name)) out.push_back(cstr(n));
    }
  }
  // Directory 13: delay imports (32-byte descriptors, name RVA at +4).
  if (std::size_t at = off(rd(dirs + 13 * 8, 4)); at) {
    for (; at + 32 <= b.size(); at += 32) {
      const std::uint32_t name = rd(at + 4, 4);
      if (!name) break;
      if (const std::size_t n = off(name)) out.push_back(cstr(n));
    }
  }
  return out;
}

bool is_system_dll(const std::string& dll) {
  const std::string d = lower(dll);
  if (d.rfind("api-ms-win-", 0) == 0 || d.rfind("ext-ms-", 0) == 0) return true;
  static const std::set<std::string> kSys = {
      "kernel32.dll", "kernelbase.dll", "ntdll.dll",    "user32.dll",    "gdi32.dll",
      "shell32.dll",  "ole32.dll",      "oleaut32.dll", "advapi32.dll",  "comdlg32.dll",
      "comctl32.dll", "opengl32.dll",   "glu32.dll",    "ws2_32.dll",    "winmm.dll",
      "imm32.dll",    "version.dll",    "shlwapi.dll",  "setupapi.dll",  "dwmapi.dll",
      "uxtheme.dll",  "bcrypt.dll",     "crypt32.dll",  "dbghelp.dll",   "hid.dll",
      "winhttp.dll",  "wininet.dll",    "iphlpapi.dll", "rpcrt4.dll",    "psapi.dll",
      "userenv.dll",  "secur32.dll",    "xinput1_4.dll", "xinput9_1_0.dll", "dsound.dll",
      "dinput8.dll",  "mfplat.dll",     "propsys.dll",  "mmdevapi.dll",  "avrt.dll",
      "ksuser.dll",   "normaliz.dll",   "dxgi.dll",     "d3d11.dll",     "d3d12.dll",
      "powrprof.dll", "shcore.dll",     "cfgmgr32.dll", "wtsapi32.dll",  "ucrtbase.dll",
      "msvcrt.dll",   "mswsock.dll",    "dnsapi.dll",   "netapi32.dll",  "gdiplus.dll",
      "msimg32.dll",  "winspool.drv",   "mpr.dll",      "wintrust.dll",  "ncrypt.dll",
      "d3dcompiler_47.dll", "dwrite.dll", "d2d1.dll",   "windowscodecs.dll", "mf.dll",
      "mfreadwrite.dll", "combase.dll", "sechost.dll",  "wldap32.dll",   "credui.dll"};
  return kSys.count(d) != 0;
}

bool check_folder_dependencies(const std::string& game_dir, std::vector<std::string>* missing,
                               std::vector<std::string>* shipped_dlls, std::string* error) {
  std::error_code ec;
  std::set<std::string> have;
  std::vector<fs::path> bins;
  for (auto it = fs::recursive_directory_iterator(game_dir, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = lower(it->path().extension().string());
    if (ext == ".dll") {
      have.insert(lower(it->path().filename().string()));
      if (shipped_dlls) shipped_dlls->push_back(it->path().filename().string());
      bins.push_back(it->path());
    } else if (ext == ".exe") {
      bins.push_back(it->path());
    }
  }
  if (bins.empty()) {
    if (error) *error = "no .exe or .dll in " + game_dir;
    return false;
  }
  std::set<std::string> miss;
  for (const fs::path& p : bins) {
    std::string err;
    const std::vector<std::string> imports = pe_imports(p.string(), &err);
    if (!err.empty()) {
      if (error) *error = err;
      continue;  // Linux exports carry ELF binaries; nothing to check.
    }
    for (const std::string& dll : imports) {
      if (is_system_dll(dll) || have.count(lower(dll))) continue;
      // Managed C# scripts host .NET through hostfxr at run time (no import).
      miss.insert(dll + " (needed by " + p.filename().string() + ")");
    }
  }
  missing->assign(miss.begin(), miss.end());
  return missing->empty();
}

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
