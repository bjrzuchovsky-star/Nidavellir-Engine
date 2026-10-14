#include "plugin/DynamicLibrary.h"

#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ts {
namespace tombstone {
namespace plugin {

namespace {

#if defined(_WIN32)
std::string last_windows_error() {
  const DWORD code = GetLastError();
  char* msg = nullptr;
  const DWORD n = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
  std::string out = n && msg ? std::string(msg, n) : std::string();
  if (msg) LocalFree(msg);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                          out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }
  return out.empty() ? "Windows error " + std::to_string(code) : out;
}
#endif

}  // namespace

DynamicLibrary::~DynamicLibrary() { close(); }

const char* DynamicLibrary::native_extension() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

bool DynamicLibrary::open(const std::string& path, std::string* error) {
  close();
#if defined(_WIN32)
  const std::wstring wide = std::filesystem::path(path).wstring();
  HMODULE h = LoadLibraryW(wide.c_str());
  if (!h) {
    if (error) *error = last_windows_error();
    return false;
  }
  handle_ = reinterpret_cast<void*>(h);
#else
  void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char* e = dlerror();
    if (error) *error = e ? e : "dlopen failed";
    return false;
  }
  handle_ = h;
#endif
  path_ = path;
  return true;
}

void* DynamicLibrary::symbol(const char* name, std::string* error) const {
  if (!handle_) {
    if (error) *error = "library not open";
    return nullptr;
  }
#if defined(_WIN32)
  FARPROC p = GetProcAddress(reinterpret_cast<HMODULE>(handle_), name);
  if (!p) {
    if (error) *error = std::string("missing symbol ") + name;
    return nullptr;
  }
  return reinterpret_cast<void*>(p);
#else
  dlerror();
  void* p = dlsym(handle_, name);
  if (!p) {
    if (error) *error = std::string("missing symbol ") + name;
    return nullptr;
  }
  return p;
#endif
}

void DynamicLibrary::close() {
  if (!handle_) return;
#if defined(_WIN32)
  FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
  dlclose(handle_);
#endif
  handle_ = nullptr;
  path_.clear();
}

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts
