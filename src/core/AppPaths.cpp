#include "core/AppPaths.h"

#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <cstdint>
#endif

namespace ts {
namespace tombstone {
namespace core {

namespace fs = std::filesystem;

std::string executable_dir() {
  std::error_code ec;
#if defined(_WIN32)
  std::wstring buf(1024, L'\0');
  for (int tries = 0; tries < 6; ++tries) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return fs::path(buf).parent_path().string();
    }
    buf.resize(buf.size() * 2);
  }
  return {};
#elif defined(__APPLE__)
  char buf[4096];
  std::uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return {};
  const fs::path p = fs::weakly_canonical(fs::path(buf), ec);
  return (ec ? fs::path(buf) : p).parent_path().string();
#else
  const fs::path p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.parent_path().string();
#endif
}

}  // namespace core
}  // namespace tombstone
}  // namespace ts
