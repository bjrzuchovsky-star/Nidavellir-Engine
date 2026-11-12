#pragma once

// Headless capture options shared by every ts_admin *--shot mode.
// Defaults stay whatever each mode already used. --shot-size WxH (for
// example 1920x1080) replaces that size. --shot-clean hides log and
// status lines that would show internal paths on a store screenshot.

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

namespace ts {
namespace tombstone {
namespace editor {
namespace shot {

inline int g_width = 0;
inline int g_height = 0;
inline bool g_clean = false;

inline bool clean() { return g_clean; }
inline bool size_set() { return g_width > 0 && g_height > 0; }

// Each capture passes its historical size. An override replaces both axes.
inline void resolve(int fallback_w, int fallback_h, int* w, int* h) {
  if (w) *w = size_set() ? g_width : fallback_w;
  if (h) *h = size_set() ? g_height : fallback_h;
}

// "1920x1080", "1920X1080" or "1920 x 1080". 64..8192 on each axis.
inline bool parse_size(const char* text, int* w, int* h) {
  if (!text || !w || !h) return false;
  int a = 0, b = 0, used = 0;
  char sep = 0;
  // %n is not counted in sscanf's return. used is the first unparsed byte.
  if (std::sscanf(text, " %d %c %d %n", &a, &sep, &b, &used) != 3) return false;
  if (sep != 'x' && sep != 'X') return false;
  const char* rest = text + used;
  while (*rest && std::isspace(static_cast<unsigned char>(*rest))) ++rest;
  if (*rest) return false;
  if (a < 64 || b < 64 || a > 8192 || b > 8192) return false;
  *w = a;
  *h = b;
  return true;
}

// Store screenshots should not show engine-internal paths. Matched when
// --shot-clean is on; ordinary editing is unchanged.
inline bool hide_log_line(std::string_view text) {
  if (!g_clean || text.empty()) return false;
  std::string lower;
  lower.reserve(text.size());
  for (unsigned char c : text) lower.push_back(static_cast<char>(std::tolower(c)));
  auto has = [&](const char* n) { return lower.find(n) != std::string::npos; };
  if (has(".tombstone")) return true;
  if (has("tombstoneconfig")) return true;
  if (has("tombstoneprojects")) return true;
  if (has("tombstoneexports")) return true;
  if (has("tombstoneupdate")) return true;
  if (has("meta.log")) return true;
  if (has("imgui.ini")) return true;
  // Plugin id and the VR simulator readout. The hierarchy label is separate.
  if (has("ts.move.rider3d")) return true;
  if (has("vr preview")) return true;
  if (has("desktop simulator")) return true;
  if (has("headset mirror")) return true;
  return false;
}

}  // namespace shot
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
