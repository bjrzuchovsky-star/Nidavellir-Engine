#pragma once

// Semantic-ish plugin versions ("1.4.2"). A dependency asks for a minimum
// version; it is met by the same major version at or above that minimum
// (a new major means a breaking change). Header-only so dynamic plugins
// need nothing from the host to use it.

#include <cstdio>
#include <string>

namespace ts {
namespace tombstone {
namespace plugin {

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  // "1", "1.2", "1.2.3" (anything after a '-' or '+' is ignored). False and
  // 0.0.0 for junk.
  static bool parse(const std::string& text, Version* out) {
    Version v;
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    int idx = 0;
    bool digit = false;
    for (char c : text) {
      if (c == '-' || c == '+') break;
      if (c >= '0' && c <= '9') {
        if (*parts[idx] > 100000) return false;
        *parts[idx] = *parts[idx] * 10 + (c - '0');
        digit = true;
      } else if (c == '.' && digit && idx < 2) {
        ++idx;
        digit = false;
      } else {
        if (out) *out = Version{};
        return false;
      }
    }
    if (!digit) {
      if (out) *out = Version{};
      return false;
    }
    if (out) *out = v;
    return true;
  }

  static Version from(const std::string& text) {
    Version v;
    parse(text, &v);
    return v;
  }

  std::string str() const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d.%d.%d", major, minor, patch);
    return buf;
  }

  int compare(const Version& o) const {
    if (major != o.major) return major < o.major ? -1 : 1;
    if (minor != o.minor) return minor < o.minor ? -1 : 1;
    if (patch != o.patch) return patch < o.patch ? -1 : 1;
    return 0;
  }
  bool operator==(const Version& o) const { return compare(o) == 0; }
  bool operator<(const Version& o) const { return compare(o) < 0; }

  // Does this (installed) version satisfy a dependency on `min`?
  bool satisfies(const Version& min) const {
    return major == min.major && compare(min) >= 0;
  }
};

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts
