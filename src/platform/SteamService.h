#pragma once

// Thin Steamworks wrapper (SDK 1.65). Built only when CMake NIDAVELLIR_STEAM
// is ON and STEAMWORKS_SDK_DIR points at the SDK. That SDK is licensed and
// must not be committed. With the option OFF every method is a no-op and
// the editor behaves as before.
//
// Clans (lobbies, invites, P2P) live in ClanDirectory.h. This class only
// boots the Steam API. The address join in the Team panel stays the fallback.

namespace ts {
namespace tombstone {
namespace steam {

class SteamService {
 public:
  static SteamService& instance();

  // True only in a NIDAVELLIR_STEAM build.
  bool enabled() const;
  // True after SteamAPI_Init succeeded this process.
  bool ready() const;

  // SteamAPI_RestartAppIfNecessary(3326740), then SteamAPI_Init.
  // Writes steam_appid.txt next to the exe when it is missing (dev runs).
  // Returns false when Steam is relaunching this process: the caller exits 0.
  // Init failure (Steam not running) logs and returns true so the editor still opens.
  bool init();

  void shutdown();
  void run_callbacks();

  // Persona name, or "" when Steam is off or not signed in.
  const char* persona() const;

 private:
  SteamService() = default;
  bool inited_ = false;
  void write_appid_file() const;
};

}  // namespace steam
}  // namespace tombstone
}  // namespace ts
