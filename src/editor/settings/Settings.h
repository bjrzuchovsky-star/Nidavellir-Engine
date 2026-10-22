#pragma once

#include <string>

namespace ts {
namespace tombstone {
namespace editor {

// Editor user settings persisted under the config directory.
//
// Default paths (relative to process cwd, portable -- no machine-absolute
// paths):
//   config file:     ./TombStoneConfig/settings.json
//   projects_root:   ./TombStoneProjects
//
// Change projects_root from the Settings screen; the project list reloads
// from the new folder. Local config + projects dirs are gitignored.
struct Settings {
  std::string projects_root = "./TombStoneProjects";
  std::string username;                 // last-used login name
  bool auto_login_dev = false;          // DEV_LOGIN on Admin start when true
  std::string theme = "dark";           // "dark" | "light" stub
  std::string last_project_path;        // last opened project folder
  // The Vault (asset library shared by every claim; see docs/vault.md).
  std::string vault_backend = "local";  // "local" | "remote"
  std::string vault_local_dir;          // "" = Documents/TombStone/Vault
  std::string vault_url;                // ts_vault server, e.g. https://vault.example.com
  std::string vault_api_key;            // sent as a Bearer token; never logged
};

class SettingsStore {
 public:
  // Config directory and settings.json path under cwd.
  static std::string default_config_dir();
  static std::string default_settings_path();
  static std::string default_projects_root();

  static Settings make_defaults();

  // Loads settings.json if present; otherwise returns defaults (does not write).
  // On parse failure, returns defaults and sets error_out when non-null.
  static Settings load(std::string* error_out = nullptr);

  // Ensures config dir exists and writes settings.json. Returns false on I/O
  // failure and fills error_out when non-null.
  static bool save(const Settings& settings, std::string* error_out = nullptr);

  // Validates projects_root exists or can be created and is writable.
  static bool ensure_projects_root(const std::string& projects_root,
                                   std::string* error_out = nullptr);
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
