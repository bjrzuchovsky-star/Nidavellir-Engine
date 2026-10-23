#include "editor/vault/FilePick.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace ts {
namespace tombstone {
namespace editor {
namespace filepick {

#ifdef _WIN32
namespace {

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

}  // namespace

bool available() { return true; }

std::vector<std::string> open_files(const std::string& title) {
  std::vector<std::string> out;
  std::wstring buf(65536, L'\0');
  const std::wstring wtitle = widen(title);
  static const wchar_t kFilter[] =
      L"Vault assets (textures, models, audio, fonts, prefabs)\0"
      L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.gltf;*.glb;*.obj;*.fbx;*.dae;*.mdl;*.wav;*.ogg;*.mp3;*.flac;*.ttf;*.otf;*.json\0"
      L"All files\0*.*\0";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFilter = kFilter;
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(buf.size());
  ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
  ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return out;
  // One file: the full path. Several: the folder, then each name, then "\0\0".
  const wchar_t* p = buf.c_str();
  const std::wstring first = p;
  p += first.size() + 1;
  if (*p == L'\0') {
    out.push_back(narrow(first));
    return out;
  }
  while (*p) {
    const std::wstring name = p;
    out.push_back(narrow(first + L"\\" + name));
    p += name.size() + 1;
  }
  return out;
}
#else
bool available() { return false; }
std::vector<std::string> open_files(const std::string&) { return {}; }
#endif

}  // namespace filepick
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
