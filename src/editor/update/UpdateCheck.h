#pragma once

// "New update available" check for the Project Manager (docs/updates.md).
// This header is the pure part: semver compare, the GitHub release JSON,
// which asset to fetch, SHA256SUMS.txt lookups and the 6-hour throttle.
// No network here; UpdateService.h does the background work through the
// Vault's HTTP client (vault/Http.h).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace update {

// The GitHub "latest release" endpoint for the engine.
inline constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/bjrzuchovsky-star/TombStone_Engine/releases/latest";
inline constexpr const char* kReleasesPageUrl =
    "https://github.com/bjrzuchovsky-star/TombStone_Engine/releases";
// At most one check per this many seconds (cached in settings.json).
inline constexpr std::int64_t kCheckIntervalSeconds = 6 * 60 * 60;

// The engine version compiled in from the repo's VERSION file.
const char* current_version();

struct SemVer {
  int major = 0, minor = 0, patch = 0;
  std::string pre;  // "beta.1" for 1.2.0-beta.1; empty for a release
};

// "v1.2.3", "1.2.3", "1.2" (patch 0), "1.2.3-rc.1", "1.2.3+build" (build
// metadata ignored). False for anything else.
bool parse_semver(std::string_view text, SemVer* out);
// <0, 0, >0 like strcmp. A pre-release sorts before its release.
int compare_semver(const SemVer& a, const SemVer& b);
// "v0.2.0" vs "0.1.0" -> true. False when either does not parse.
bool is_newer(std::string_view latest_tag, std::string_view current);
// "v0.2.0" -> "0.2.0" (only the leading v/V goes).
std::string strip_v(std::string_view tag);

struct ReleaseAsset {
  std::string name;
  std::string url;     // browser_download_url
  std::uint64_t size = 0;
  std::string sha256;  // from the asset's "digest" ("sha256:<hex>") when GitHub gives one
};

struct ReleaseInfo {
  std::string tag;      // tag_name, e.g. "v0.2.0"
  std::string version;  // tag without the v
  std::string name;
  std::string html_url;
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
};

// Parses GitHub's GET /repos/{o}/{r}/releases/latest body.
bool parse_release_json(const std::string& text, ReleaseInfo* out, std::string* error);

enum class AssetKind { None, Update, Setup };
// TombStone-<ver>-update.exe first, else TombStone-<ver>-setup.exe (ver =
// the tag without the v; names compare case-insensitively). nullptr when
// neither is attached.
const ReleaseAsset* select_update_asset(const ReleaseInfo& r, AssetKind* kind = nullptr);
const ReleaseAsset* find_sums_asset(const ReleaseInfo& r);  // SHA256SUMS.txt
// Lowercase hex for `file_name` in a SHA256SUMS.txt ("<hex>  <name>" or
// "<hex> *<name>"); "" when the file is not listed.
std::string sums_lookup(const std::string& sums_text, const std::string& file_name);

// True when a check is due: never checked, the clock went backwards, or
// kCheckIntervalSeconds have passed.
bool check_due(std::int64_t last_check_unix, std::int64_t now_unix,
               std::int64_t interval = kCheckIntervalSeconds);
// Show the banner for `latest`? Newer than `current` and not the skipped one.
bool should_notify(std::string_view latest, std::string_view current, std::string_view skipped);

// TS_FAKE_UPDATE=9.9.9 forces the banner (screenshots / testing). "" when unset or not a version.
std::string fake_update_version();

std::int64_t now_unix();

}  // namespace update
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
