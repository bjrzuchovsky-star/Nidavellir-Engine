#pragma once

// game.json: the manifest an exported game ships next to its executable
// (written by ts_export / the Freight Office). ts_game reads it when run with
// no --project (or when --project points at an export folder), mounts the
// .tspak it names through ts_vfs, and takes its window size / icon / title.
//
//   { "name": "...", "version": "1.0.0", "dimension": "2d|3d|vr",
//     "start_scene": "scene.json", "window_width": 1280,
//     "window_height": 720, "icon": "icon.png", "pak": "game.tspak",
//     "profile": "shipping|dev" }

#include <string>

struct GLFWwindow;

namespace ts {
namespace tombstone {
namespace game {

struct GameManifest {
  bool loaded = false;
  std::string dir;  // folder holding game.json (the export root)
  std::string name;
  std::string version;
  std::string dimension = "2d";
  std::string start_scene = "scene.json";
  std::string icon;  // path inside the pak / project
  std::string pak = "game.tspak";
  std::string profile;
  int window_width = 1280;
  int window_height = 720;
};

// The manifest in effect (defaults when the game is not an export).
const GameManifest& manifest();

bool load_manifest(const std::string& dir, GameManifest* out, std::string* error);

// Turns the launch into a project folder: --project as given, else the
// executable's folder when it holds game.json. When that folder has the
// manifest and its pak, the pak is mounted (verified, cached by content
// hash) and *project becomes the mounted folder; plugin DLLs in
// <export>/plugins join TS_PLUGIN_PATH. False (with error) when nothing to
// run. A folder with no pak runs its loose files as before.
bool resolve_launch(const char* argv0, std::string* project, std::string* error);

// Window size + icon from the manifest (1280x720, no icon by default).
int window_width();
int window_height();
void apply_window_icon(GLFWwindow* win, const std::string& project_dir);

}  // namespace game
}  // namespace tombstone
}  // namespace ts
