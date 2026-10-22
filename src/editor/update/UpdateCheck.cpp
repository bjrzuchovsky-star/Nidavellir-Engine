#include "editor/update/UpdateCheck.h"

#include "scene/JsonTree.h"
#include "vault/Sha256.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>

#ifndef TS_ENGINE_VERSION
#define TS_ENGINE_VERSION "0.0.0"
#endif

namespace ts {
namespace tombstone {
namespace editor {
namespace update {

namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool parse_uint(std::string_view s, int* out) {
  if (s.empty() || s.size() > 9) return false;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *out = v;
  return true;
}

bool all_digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Semver 2.0 pre-release precedence: dot-separated identifiers, numeric
// ones compare numerically and sort before alphanumeric ones.
int compare_pre(const std::string& a, const std::string& b) {
  if (a == b) return 0;
  if (a.empty()) return 1;
  if (b.empty()) return -1;
  std::stringstream sa(a), sb(b);
  std::string x, y;
  for (;;) {
    const bool ha = static_cast<bool>(std::getline(sa, x, '.'));
    const bool hb = static_cast<bool>(std::getline(sb, y, '.'));
    if (!ha && !hb) return 0;
    if (!ha) return -1;
    if (!hb) return 1;
    const bool nx = all_digits(x), ny = all_digits(y);
    if (nx && ny) {
      const long long ix = std::atoll(x.c_str()), iy = std::atoll(y.c_str());
      if (ix != iy) return ix < iy ? -1 : 1;
    } else if (nx != ny) {
      return nx ? -1 : 1;
    } else if (x != y) {
      return x < y ? -1 : 1;
    }
  }
}

}  // namespace

const char* current_version() { return TS_ENGINE_VERSION; }

std::string strip_v(std::string_view tag) {
  std::size_t i = 0;
  while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
  if (i < tag.size() && (tag[i] == 'v' || tag[i] == 'V')) ++i;
  std::string out(tag.substr(i));
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return out;
}

bool parse_semver(std::string_view text, SemVer* out) {
  std::string s = strip_v(text);
  if (const std::size_t plus = s.find('+'); plus != std::string::npos) s.resize(plus);
  SemVer v;
  if (const std::size_t dash = s.find('-'); dash != std::string::npos) {
    v.pre = s.substr(dash + 1);
    s.resize(dash);
    if (v.pre.empty()) return false;
    for (char c : v.pre) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return false;
    }
  }
  std::vector<std::string> parts;
  std::stringstream ss(s);
  std::string p;
  while (std::getline(ss, p, '.')) parts.push_back(p);
  if (!s.empty() && s.back() == '.') return false;
  if (parts.size() < 2 || parts.size() > 3) return false;
  if (!parse_uint(parts[0], &v.major) || !parse_uint(parts[1], &v.minor)) return false;
  if (parts.size() == 3 && !parse_uint(parts[2], &v.patch)) return false;
  if (out) *out = v;
  return true;
}

int compare_semver(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return compare_pre(a.pre, b.pre);
}

bool is_newer(std::string_view latest_tag, std::string_view current) {
  SemVer a, b;
  if (!parse_semver(latest_tag, &a) || !parse_semver(current, &b)) return false;
  return compare_semver(a, b) > 0;
}

bool parse_release_json(const std::string& text, ReleaseInfo* out, std::string* error) {
  json::Value root;
  std::string err;
  if (!json::parse(text, &root, &err)) {
    if (error) *error = "release JSON: " + err;
    return false;
  }
  if (!root.is_object()) {
    if (error) *error = "release JSON is not an object";
    return false;
  }
  ReleaseInfo r;
  r.tag = root.str("tag_name");
  if (r.tag.empty()) {
    if (error) *error = "release JSON has no tag_name";
    return false;
  }
  r.version = strip_v(r.tag);
  r.name = root.str("name");
  r.html_url = root.str("html_url");
  r.draft = root.flag("draft", false);
  r.prerelease = root.flag("prerelease", false);
  for (const json::Value& a : root["assets"].items()) {
    if (!a.is_object()) continue;
    ReleaseAsset ra;
    ra.name = a.str("name");
    ra.url = a.str("browser_download_url");
    ra.size = static_cast<std::uint64_t>(std::max(0.0, a.num("size", 0.0)));
    const std::string digest = lower(a.str("digest"));
    if (digest.rfind("sha256:", 0) == 0 && vault::is_sha256_hex(digest.substr(7))) ra.sha256 = digest.substr(7);
    if (!ra.name.empty() && !ra.url.empty()) r.assets.push_back(std::move(ra));
  }
  *out = std::move(r);
  return true;
}

const ReleaseAsset* select_update_asset(const ReleaseInfo& r, AssetKind* kind) {
  const std::string ver = strip_v(r.tag.empty() ? r.version : r.tag);
  const std::string want_update = lower("TombStone-" + ver + "-update.exe");
  const std::string want_setup = lower("TombStone-" + ver + "-setup.exe");
  const ReleaseAsset* setup = nullptr;
  for (const ReleaseAsset& a : r.assets) {
    const std::string n = lower(a.name);
    if (n == want_update) {
      if (kind) *kind = AssetKind::Update;
      return &a;
    }
    if (n == want_setup && !setup) setup = &a;
  }
  if (kind) *kind = setup ? AssetKind::Setup : AssetKind::None;
  return setup;
}

const ReleaseAsset* find_sums_asset(const ReleaseInfo& r) {
  for (const ReleaseAsset& a : r.assets) {
    if (lower(a.name) == "sha256sums.txt") return &a;
  }
  return nullptr;
}

std::string sums_lookup(const std::string& sums_text, const std::string& file_name) {
  std::stringstream ss(sums_text);
  std::string line;
  const std::string want = lower(file_name);
  while (std::getline(ss, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    const std::size_t sp = line.find_first_of(" \t");
    if (sp == std::string::npos) continue;
    const std::string hex = lower(line.substr(0, sp));
    std::size_t i = sp;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i < line.size() && line[i] == '*') ++i;  // binary-mode marker
    if (lower(line.substr(i)) == want && vault::is_sha256_hex(hex)) return hex;
  }
  return {};
}

bool check_due(std::int64_t last_check_unix, std::int64_t now, std::int64_t interval) {
  if (last_check_unix <= 0) return true;
  if (now < last_check_unix) return true;  // clock moved back: don't wait forever
  return now - last_check_unix >= interval;
}

bool should_notify(std::string_view latest, std::string_view current, std::string_view skipped) {
  if (!is_newer(latest, current)) return false;
  if (!skipped.empty()) {
    SemVer a, s;
    if (parse_semver(latest, &a) && parse_semver(skipped, &s) && compare_semver(a, s) == 0) return false;
  }
  return true;
}

std::string fake_update_version() {
  const char* v = std::getenv("TS_FAKE_UPDATE");
  if (!v || !*v) return {};
  SemVer sv;
  if (!parse_semver(v, &sv)) return {};
  return strip_v(v);
}

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace update
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
