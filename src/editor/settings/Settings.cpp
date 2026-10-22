#include "editor/settings/Settings.h"

#include "editor/settings/JsonMini.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

std::string SettingsStore::default_config_dir() {
  return "./TombStoneConfig";
}

std::string SettingsStore::default_settings_path() {
  return (fs::path(default_config_dir()) / "settings.json").string();
}

std::string SettingsStore::default_projects_root() {
  return "./TombStoneProjects";
}

Settings SettingsStore::make_defaults() {
  Settings s;
  s.projects_root = default_projects_root();
  s.username.clear();
  s.auto_login_dev = false;
  s.theme = "dark";
  s.last_project_path.clear();
  return s;
}

Settings SettingsStore::load(std::string* error_out) {
  const fs::path path = default_settings_path();
  if (!fs::exists(path)) {
    return make_defaults();
  }

  std::ifstream in(path);
  if (!in) {
    if (error_out) {
      *error_out = "Could not open settings file: " + path.string();
    }
    return make_defaults();
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  auto parsed = json_mini::parse_object(text);
  if (!parsed) {
    if (error_out) {
      *error_out = "Failed to parse settings JSON: " + path.string();
    }
    return make_defaults();
  }

  Settings s = make_defaults();
  s.projects_root =
      json_mini::get_string(*parsed, "projects_root", s.projects_root);
  s.username = json_mini::get_string(*parsed, "username", s.username);
  s.auto_login_dev =
      json_mini::get_bool(*parsed, "auto_login_dev", s.auto_login_dev);
  s.theme = json_mini::get_string(*parsed, "theme", s.theme);
  if (s.theme != "dark" && s.theme != "light") {
    s.theme = "dark";
  }
  s.last_project_path =
      json_mini::get_string(*parsed, "last_project_path", s.last_project_path);

  s.vault_backend = json_mini::get_string(*parsed, "vault_backend", s.vault_backend);
  if (s.vault_backend != "local" && s.vault_backend != "remote") s.vault_backend = "local";
  s.vault_local_dir = json_mini::get_string(*parsed, "vault_local_dir", s.vault_local_dir);
  s.vault_url = json_mini::get_string(*parsed, "vault_url", s.vault_url);
  s.vault_api_key = json_mini::get_string(*parsed, "vault_api_key", s.vault_api_key);
  s.check_updates_on_start =
      json_mini::get_bool(*parsed, "check_updates_on_start", s.check_updates_on_start);
  s.update_skip_version = json_mini::get_string(*parsed, "update_skip_version", s.update_skip_version);
  s.update_last_check = json_mini::get_string(*parsed, "update_last_check", s.update_last_check);
  s.update_cached_version = json_mini::get_string(*parsed, "update_cached_version", s.update_cached_version);
  s.update_cached_url = json_mini::get_string(*parsed, "update_cached_url", s.update_cached_url);

  if (s.projects_root.empty()) {
    s.projects_root = default_projects_root();
  }
  return s;
}

bool SettingsStore::save(const Settings& settings, std::string* error_out) {
  const fs::path dir = default_config_dir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    if (error_out) {
      *error_out = "Could not create config dir '" + dir.string() +
                   "': " + ec.message();
    }
    return false;
  }

  const fs::path path = default_settings_path();
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    if (error_out) {
      *error_out = "Could not write settings file: " + path.string();
    }
    return false;
  }

  Settings normalized = settings;
  if (normalized.projects_root.empty()) {
    normalized.projects_root = default_projects_root();
  }
  if (normalized.theme != "dark" && normalized.theme != "light") {
    normalized.theme = "dark";
  }

  const std::string body = json_mini::write_object(
      {
          {"projects_root", normalized.projects_root},
          {"username", normalized.username},
          {"theme", normalized.theme},
          {"last_project_path", normalized.last_project_path},
          {"vault_backend", normalized.vault_backend == "remote" ? "remote" : "local"},
          {"vault_local_dir", normalized.vault_local_dir},
          {"vault_url", normalized.vault_url},
          {"vault_api_key", normalized.vault_api_key},
          {"update_skip_version", normalized.update_skip_version},
          {"update_last_check", normalized.update_last_check},
          {"update_cached_version", normalized.update_cached_version},
          {"update_cached_url", normalized.update_cached_url},
      },
      {
          {"auto_login_dev", normalized.auto_login_dev},
          {"check_updates_on_start", normalized.check_updates_on_start},
      });
  out << body;
  if (!out) {
    if (error_out) {
      *error_out = "Failed while writing settings file: " + path.string();
    }
    return false;
  }
  return true;
}

bool SettingsStore::ensure_projects_root(const std::string& projects_root,
                                         std::string* error_out) {
  if (projects_root.empty()) {
    if (error_out) {
      *error_out = "projects_root path is empty";
    }
    return false;
  }

  const fs::path root = projects_root;
  std::error_code ec;
  if (!fs::exists(root)) {
    fs::create_directories(root, ec);
    if (ec) {
      if (error_out) {
        *error_out = "Could not create projects_root '" + root.string() +
                     "': " + ec.message();
      }
      return false;
    }
  } else if (!fs::is_directory(root)) {
    if (error_out) {
      *error_out = "projects_root is not a directory: " + root.string();
    }
    return false;
  }

  // Probe writability with a temporary file.
  const fs::path probe = root / ".ts_write_probe";
  {
    std::ofstream out(probe, std::ios::trunc);
    if (!out) {
      if (error_out) {
        *error_out = "projects_root is not writable: " + root.string();
      }
      return false;
    }
    out << "ok\n";
  }
  fs::remove(probe, ec);
  return true;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
