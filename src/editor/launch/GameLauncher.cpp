#include "editor/launch/GameLauncher.h"

#include <cstdint>
#include <filesystem>
#include <string>
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
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace ts {
namespace tombstone {
namespace editor {
namespace launcher {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr const char* kGameExe = "ts_game.exe";
#else
constexpr const char* kGameExe = "ts_game";
#endif

// Project folder as an absolute path with no trailing separator (a
// trailing backslash would escape the closing quote on Windows).
fs::path clean_project_path(const std::string& project_dir) {
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(project_dir), ec);
  if (ec) {
    p = fs::path(project_dir);
  }
  p = p.lexically_normal();
  if (!p.has_filename() && p.has_parent_path() && p != p.root_path()) {
    p = p.parent_path();
  }
  return p;
}

}  // namespace

std::string self_executable_path() {
#if defined(_WIN32)
  std::wstring buf(1024, L'\0');
  for (int tries = 0; tries < 6; ++tries) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                       static_cast<DWORD>(buf.size()));
    if (n == 0) {
      return {};
    }
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf).string();
    }
    buf.resize(buf.size() * 2);
  }
  return {};
#elif defined(__APPLE__)
  char buf[4096];
  std::uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) {
    return {};
  }
  std::error_code ec;
  const fs::path p = fs::weakly_canonical(fs::path(buf), ec);
  return ec ? std::string(buf) : p.string();
#else
  std::error_code ec;
  const fs::path p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.string();
#endif
}

std::vector<std::string> game_executable_candidates() {
  std::vector<std::string> out;
  const std::string self = self_executable_path();
  if (self.empty()) {
    return out;
  }
  const fs::path dir = fs::path(self).parent_path();
  // Installed side by side.
  out.push_back((dir / kGameExe).string());
  // Build tree: build/apps/admin/ts_admin -> build/apps/game/ts_game.
  out.push_back((dir.parent_path() / "game" / kGameExe).string());
  // Multi-config (Visual Studio): build/apps/admin/Release/ts_admin.exe ->
  // build/apps/game/Release/ts_game.exe.
  if (dir.has_parent_path()) {
    out.push_back((dir.parent_path().parent_path() / "game" / dir.filename() /
                   kGameExe)
                      .string());
  }
  return out;
}

std::string find_game_executable() {
  for (const std::string& c : game_executable_candidates()) {
    std::error_code ec;
    if (fs::is_regular_file(fs::path(c), ec)) {
      return c;
    }
  }
  return {};
}

bool spawn_game(const std::string& exe, const std::string& project_dir,
                std::string* error_out) {
  std::error_code ec;
  if (exe.empty() || !fs::is_regular_file(fs::path(exe), ec)) {
    if (error_out) *error_out = "ts_game not found: " + exe;
    return false;
  }
  const fs::path project = clean_project_path(project_dir);
#if defined(_WIN32)
  const std::wstring exe_w = fs::path(exe).wstring();
  std::wstring cmd = L"\"" + exe_w + L"\" --project \"" + project.wstring() +
                     L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // Shares ts_admin's console so the game's log lands next to the editor's.
  if (!CreateProcessW(exe_w.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &si, &pi)) {
    if (error_out) {
      *error_out = "CreateProcess failed (error " +
                   std::to_string(static_cast<unsigned long>(GetLastError())) +
                   ")";
    }
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  if (access(exe.c_str(), X_OK) != 0) {
    if (error_out) *error_out = "ts_game is not executable: " + exe;
    return false;
  }
  const std::string project_s = project.string();
  // Double fork: the grandchild runs the game and is adopted by init, so
  // the editor never has to reap it.
  const pid_t child = fork();
  if (child < 0) {
    if (error_out) *error_out = "fork failed";
    return false;
  }
  if (child == 0) {
    const pid_t grandchild = fork();
    if (grandchild == 0) {
      const char* argv[] = {exe.c_str(), "--project", project_s.c_str(),
                            nullptr};
      execv(exe.c_str(), const_cast<char* const*>(argv));
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
    if (error_out) *error_out = "could not start ts_game";
    return false;
  }
  return true;
#endif
}

// --- ts_client (Play with posse > Open a client window) ------------------------

namespace {

#if defined(_WIN32)
constexpr const char* kClientExe = "ts_client.exe";

std::wstring quote_w(const std::wstring& s) {
  std::wstring out = L"\"";
  for (wchar_t c : s) {
    if (c == L'"') out += L'\\';
    out += c;
  }
  return out + L"\"";
}
#else
constexpr const char* kClientExe = "ts_client";
#endif

// Start `exe args...` detached (it outlives the editor).
bool spawn_detached(const std::string& exe, const std::vector<std::string>& args,
                    std::string* error_out) {
  std::error_code ec;
  if (exe.empty() || !fs::is_regular_file(fs::path(exe), ec)) {
    if (error_out) *error_out = "not found: " + exe;
    return false;
  }
#if defined(_WIN32)
  const std::wstring exe_w = fs::path(exe).wstring();
  std::wstring cmd = quote_w(exe_w);
  for (const std::string& a : args) cmd += L" " + quote_w(fs::path(a).wstring());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe_w.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      nullptr, &si, &pi)) {
    if (error_out) {
      *error_out = "CreateProcess failed (error " +
                   std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
    }
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  if (access(exe.c_str(), X_OK) != 0) {
    if (error_out) *error_out = "not executable: " + exe;
    return false;
  }
  std::vector<const char*> argv;
  argv.push_back(exe.c_str());
  for (const std::string& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  // Double fork: the grandchild is adopted by init, so no zombie waits on us.
  const pid_t child = fork();
  if (child < 0) {
    if (error_out) *error_out = "fork failed";
    return false;
  }
  if (child == 0) {
    const pid_t grandchild = fork();
    if (grandchild == 0) {
      execv(exe.c_str(), const_cast<char* const*>(argv.data()));
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
    if (error_out) *error_out = "could not start " + exe;
    return false;
  }
  return true;
#endif
}

}  // namespace

std::vector<std::string> client_executable_candidates() {
  std::vector<std::string> out;
  const std::string self = self_executable_path();
  if (self.empty()) return out;
  const fs::path dir = fs::path(self).parent_path();
  out.push_back((dir / kClientExe).string());
  out.push_back((dir.parent_path() / "client" / kClientExe).string());
  if (dir.has_parent_path()) {
    out.push_back(
        (dir.parent_path().parent_path() / "client" / dir.filename() / kClientExe).string());
  }
  return out;
}

std::string find_client_executable() {
  for (const std::string& c : client_executable_candidates()) {
    std::error_code ec;
    if (fs::is_regular_file(fs::path(c), ec)) return c;
  }
  return {};
}

bool spawn_client(const std::string& exe, const std::string& endpoint,
                  const std::string& name, const std::string& project_dir,
                  std::string* error_out) {
  std::vector<std::string> args = {"--connect", endpoint, "--name", name};
  if (!project_dir.empty()) {
    args.push_back("--project");
    args.push_back(clean_project_path(project_dir).string());
  }
  return spawn_detached(exe, args, error_out);
}

}  // namespace launcher
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
