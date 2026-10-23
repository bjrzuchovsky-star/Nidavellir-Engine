#include "editor/ux/Layouts.h"

#include "editor/settings/Settings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts::tombstone::editor::ux {

namespace fs = std::filesystem;

namespace {
std::string g_pending;
bool g_has_pending = false;

fs::path file_for(bool three_d, const std::string& name) {
  return fs::path(layouts_dir(three_d)) / (clean_layout_name(name) + ".ini");
}
}  // namespace

std::string layouts_dir(bool three_d) {
  return (fs::path(SettingsStore::default_config_dir()) / "layouts" / (three_d ? "3d" : "2d")).string();
}

std::string clean_layout_name(const std::string& name) {
  std::string out;
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == ' ' || c == '-' || c == '_') out += c;
    if (out.size() >= 48) break;
  }
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::vector<std::string> list_layouts(bool three_d) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(layouts_dir(three_d), ec)) {
    if (e.is_regular_file(ec) && e.path().extension() == ".ini") out.push_back(e.path().stem().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool layout_exists(bool three_d, const std::string& name) {
  std::error_code ec;
  return !clean_layout_name(name).empty() && fs::exists(file_for(three_d, name), ec);
}

bool save_layout(bool three_d, const std::string& name, std::string* error) {
  const std::string clean = clean_layout_name(name);
  if (clean.empty()) {
    if (error) *error = "Give the layout a name (letters, numbers, spaces, - or _).";
    return false;
  }
  if (!ImGui::GetCurrentContext()) {
    if (error) *error = "No editor window to take the layout from.";
    return false;
  }
  std::error_code ec;
  fs::create_directories(layouts_dir(three_d), ec);
  size_t n = 0;
  const char* ini = ImGui::SaveIniSettingsToMemory(&n);
  const fs::path p = file_for(three_d, clean);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "Could not write " + tmp.string();
      return false;
    }
    out << "; TombStone layout \"" << clean << "\" (" << (three_d ? "3D" : "2D") << " editor)\n";
    out.write(ini, static_cast<std::streamsize>(n));
    if (!out) {
      if (error) *error = "Could not write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
  }
  if (ec) {
    if (error) *error = "Could not replace " + p.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool queue_load_layout(bool three_d, const std::string& name, std::string* error) {
  const fs::path p = file_for(three_d, name);
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    if (error) *error = "Layout not found: " + p.string();
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  g_pending = ss.str();
  if (g_pending.find("[Window]") == std::string::npos && g_pending.find("[Docking]") == std::string::npos) {
    g_pending.clear();
    if (error) *error = "Layout file has no panels in it (could not be read): " + p.string();
    return false;
  }
  g_has_pending = true;
  return true;
}

bool delete_layout(bool three_d, const std::string& name, std::string* error) {
  std::error_code ec;
  if (!fs::remove(file_for(three_d, name), ec) || ec) {
    if (error) *error = "Could not delete layout " + name + (ec ? ": " + ec.message() : std::string());
    return false;
  }
  return true;
}

bool layout_pending() { return g_has_pending; }

bool apply_pending_layout() {
  if (!g_has_pending || !ImGui::GetCurrentContext()) return false;
  g_has_pending = false;
  // Replace (not merge): drop the current window/dock settings, then read
  // the saved ones; ImGui re-docks the live windows from them.
  ImGui::ClearIniSettings();
  ImGui::LoadIniSettingsFromMemory(g_pending.c_str(), g_pending.size());
  g_pending.clear();
  return true;
}

}  // namespace ts::tombstone::editor::ux
