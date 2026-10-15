#include "export/WinIcon.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ts {
namespace tombstone {
namespace exporter {

bool stamp_exe_icon(const std::string& exe, const std::string& png, std::string* error) {
#if defined(_WIN32)
  std::ifstream in(std::filesystem::path(png), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string bytes = ss.str();
  if (bytes.size() < 24 || bytes.compare(1, 3, "PNG") != 0) {
    if (error) *error = png + " is not a PNG";
    return false;
  }
  auto be32 = [&](std::size_t at) {
    return (static_cast<unsigned>(static_cast<unsigned char>(bytes[at])) << 24) |
           (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
           (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
           static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 3]));
  };
  const unsigned w = be32(16), h = be32(20);
#pragma pack(push, 2)
  struct GrpDir {
    WORD reserved, type, count;
    BYTE width, height, colors, reserved2;
    WORD planes, bits;
    DWORD bytes;
    WORD id;
  } grp{0, 1, 1, static_cast<BYTE>(w >= 256 ? 0 : w), static_cast<BYTE>(h >= 256 ? 0 : h),
        0, 0, 1, 32, static_cast<DWORD>(bytes.size()), 1};
#pragma pack(pop)
  const std::wstring wexe = std::filesystem::path(exe).wstring();
  HANDLE hres = BeginUpdateResourceW(wexe.c_str(), FALSE);
  if (!hres) {
    if (error) *error = "BeginUpdateResource failed on " + exe;
    return false;
  }
  const LANGID lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
  bool ok = UpdateResourceW(hres, MAKEINTRESOURCEW(3) /*RT_ICON*/, MAKEINTRESOURCEW(1), lang,
                            const_cast<char*>(bytes.data()), static_cast<DWORD>(bytes.size())) &&
            UpdateResourceW(hres, MAKEINTRESOURCEW(14) /*RT_GROUP_ICON*/, MAKEINTRESOURCEW(1),
                            lang, &grp, sizeof(grp));
  if (!EndUpdateResourceW(hres, ok ? FALSE : TRUE)) ok = false;
  if (!ok && error) *error = "UpdateResource failed on " + exe;
  return ok;
#else
  (void)exe;
  (void)png;
  if (error) *error = "exe icon stamping is Windows-only (Linux uses the window icon)";
  return false;
#endif
}

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
