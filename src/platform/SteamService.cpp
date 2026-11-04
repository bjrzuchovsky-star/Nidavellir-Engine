#include "platform/SteamService.h"

#include "core/AppPaths.h"
#include "core/Brand.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#if defined(NIDAVELLIR_STEAM)
#include <steam/steam_api.h>
#endif

namespace ts {
namespace tombstone {
namespace steam {

namespace fs = std::filesystem;

SteamService& SteamService::instance() {
  static SteamService s;
  return s;
}

bool SteamService::enabled() const {
#if defined(NIDAVELLIR_STEAM)
  return true;
#else
  return false;
#endif
}

void SteamService::write_appid_file() const {
#if defined(NIDAVELLIR_STEAM)
  const std::string dir = core::executable_dir();
  if (dir.empty()) return;
  const fs::path file = fs::path(dir) / "steam_appid.txt";
  std::error_code ec;
  if (fs::exists(file, ec)) return;
  std::ofstream out(file);
  if (out) out << brand::kSteamAppId << '\n';
#else
  (void)this;
#endif
}

bool SteamService::init() {
#if defined(NIDAVELLIR_STEAM)
  write_appid_file();
  if (SteamAPI_RestartAppIfNecessary(brand::kSteamAppId)) {
    std::cout << "[steam] restarting through Steam (app " << brand::kSteamAppId << ")\n";
    return false;
  }
  if (!SteamAPI_Init()) {
    std::cerr << "[steam] SteamAPI_Init failed. Continuing without a persona.\n";
    return true;
  }
  inited_ = true;
  const char* who = persona();
  std::cout << "[steam] signed in" << (who && *who ? " as " : "") << (who ? who : "") << '\n';
#else
  (void)this;
#endif
  return true;
}

void SteamService::shutdown() {
#if defined(NIDAVELLIR_STEAM)
  if (inited_) {
    SteamAPI_Shutdown();
    inited_ = false;
  }
#else
  (void)this;
#endif
}

void SteamService::run_callbacks() {
#if defined(NIDAVELLIR_STEAM)
  if (inited_) SteamAPI_RunCallbacks();
#else
  (void)this;
#endif
}

const char* SteamService::persona() const {
#if defined(NIDAVELLIR_STEAM)
  if (inited_ && SteamFriends()) {
    const char* name = SteamFriends()->GetPersonaName();
    return name ? name : "";
  }
#else
  (void)this;
#endif
  return "";
}

}  // namespace steam
}  // namespace tombstone
}  // namespace ts
