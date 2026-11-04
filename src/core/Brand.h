#pragma once

// User-facing product identity. Internal namespaces (ts::), file formats
// and the C# TombStone.Script API are unchanged. docs/glossary.md maps the
// old western words to the Nordic UI labels.

namespace ts {
namespace tombstone {
namespace brand {

inline constexpr const char* kProduct = "Nidavellir Engine";
inline constexpr const char* kCompany = "BORENJO Studios";
inline constexpr const char* kTagline = "Where games are forged";
// ASCII only: some Windows title bars mangle UTF-8 from GLFW.
inline constexpr const char* kWindowTitle = "Nidavellir Engine";
inline constexpr const char* kWindowTitlePrefix = "Nidavellir Engine | ";

// Glossary (UI labels). Code ids stay claim / posse / vault / rider.
inline constexpr const char* kRealm = "Realm";
inline constexpr const char* kClan = "Clan";
inline constexpr const char* kJoinClan = "Join clan";
inline constexpr const char* kMarket = "Market";
inline constexpr const char* kVault = "Vault";  // kept: clearer than Hoard
inline constexpr const char* kWanderer = "Wanderer";

inline constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/bjrzuchovsky-star/Nidavellir-Engine/releases/latest";
inline constexpr const char* kReleasesPageUrl =
    "https://github.com/bjrzuchovsky-star/Nidavellir-Engine/releases";
inline constexpr const char* kDocsBlobPrefix =
    "https://github.com/bjrzuchovsky-star/Nidavellir-Engine/blob/main/docs/";

// Installer asset names. The updater accepts the old TombStone- prefix too.
inline constexpr const char* kAssetPrefix = "Nidavellir";
inline constexpr const char* kLegacyAssetPrefix = "TombStone";

// Same AppId as 0.2.x so the update installer upgrades in place.
inline constexpr const char* kInstallerAppId = "{6F1D3B0A-7C52-4E1B-9A3D-5B2C8E4F7A10}";

inline constexpr const char* kThemeForge = "forge";
inline constexpr const char* kThemeDusk = "dusk";
inline constexpr const char* kThemeLegacyDark = "dark";  // 0.2.x settings.json
inline constexpr const char* kThemeParchment = "light";

inline constexpr unsigned kSteamAppId = 3326740;
inline constexpr unsigned kSteamDepotId = 3326741;

}  // namespace brand
}  // namespace tombstone
}  // namespace ts
