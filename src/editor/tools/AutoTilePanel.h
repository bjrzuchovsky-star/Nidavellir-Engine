#pragma once

// Autotiles panel, Dusk theme: the project's terrains (tiles/*.autotile.json:
// 47-blob, 16-tile Wang or rule tiles). Pick one as the tilemap painter's
// brush, make new terrains (with a template tileset to paint over), see
// each layout slot with its neighbour mask, and try the terrain on a
// scratch map right in the panel.

#include "editor/assets/TextureCache.h"
#include "scene/TileMap.h"

#include <imgui.h>
#include "scene/AutoTile.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class AutoTilePanel {
 public:
  static AutoTilePanel& get();
  void open(const std::string& project_dir, const std::string& project_name);
  bool is_open() const { return open_; }
  void close() { open_ = false; }
  void draw();
  void pin(float x, float y, float w, float h) {
    pinned_ = true;
    pin_[0] = x; pin_[1] = y; pin_[2] = w; pin_[3] = h;
  }

  // The terrain the painter should use, or nullptr (plain tiles). Only for
  // the project the panel last opened.
  const AutoTileSet* brush(const std::string& project_dir) const;
  const std::string& brush_path() const { return brush_path_; }
  bool select_brush(const std::string& rel_path);  // "" = plain tiles
  void clear_brush() { brush_path_.clear(); }
  // Sets of the open project (refreshes the list).
  void rescan();
  const std::vector<std::string>& sets() const { return paths_; }
  const AutoTileSet* set(const std::string& rel) const;
  // "Re-tile the selected TileMap" was clicked (the editor consumes it).
  bool take_retile_request() {
    const bool r = retile_request_;
    retile_request_ = false;
    return r;
  }

  // New terrain: tiles/<slug>.autotile.json + assets/tiles/<slug>.png
  // template. Returns the set path ("" + *error on failure).
  static std::string create_terrain(const std::string& project_dir, const std::string& name,
                                    AutoTileMode mode, int tile_size, const unsigned char fill[4],
                                    const unsigned char rim[4], std::string* error);

  // Paints a little demo island on the scratch map (screenshots / smoke).
  void paint_demo();

 private:
  void draw_slots(const AutoTileSet& s);
  void draw_scratch(const AutoTileSet& s);
  void tile_quad(ImDrawList* dl, const AutoTileSet& s, int tile, const ImVec2& p0, const ImVec2& p1);

  bool open_ = false;
  bool pinned_ = false;
  float pin_[4] = {};
  std::string project_dir_, project_name_;
  std::vector<std::string> paths_;
  std::vector<AutoTileSet> loaded_;
  std::vector<std::string> errors_;
  int current_ = -1;
  std::string brush_path_;
  bool retile_request_ = false;
  char new_name_[64] = "Grass";
  int new_mode_ = 0;
  int new_size_ = 32;
  float new_fill_[4] = {0.42f, 0.62f, 0.24f, 1.0f};
  float new_rim_[4] = {0.27f, 0.40f, 0.14f, 1.0f};
  std::string status_;
  TileMapData scratch_{20, 12, 32};
  TextureCache textures_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
