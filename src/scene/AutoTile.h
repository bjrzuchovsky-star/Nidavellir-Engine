#pragma once

// Autotiling for the 2D tilemap painter: a terrain brush that picks each
// cell's tile from which of its neighbours are the same terrain. Three
// kinds, kept in <project>/tiles/<name>.autotile.json:
//
//   blob47  8-neighbour "blob": corners count only when both touching
//           edges are terrain, leaving 47 distinct tiles. Layout slot i is
//           the i-th of blob47_masks() (sorted masks), tile id base + i
//           unless "map" lists ids.
//   wang16  4-edge (N/E/S/W) Wang tiles: slot = N*1 + E*2 + S*4 + W*8.
//   rules   rule tiles: the first rule whose 3x3 pattern matches wins
//           ('x' terrain, 'o' not terrain, '.' anything), else "fill".
//
// Pure data + algorithms (no UI), shared by the editor and tests.

#include "scene/TileMap.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {

// Neighbour bits (clockwise from north).
enum : std::uint8_t {
  kNbN = 1, kNbNE = 2, kNbE = 4, kNbSE = 8, kNbS = 16, kNbSW = 32, kNbW = 64, kNbNW = 128
};

enum class AutoTileMode { Blob47, Wang16, Rules };
const char* to_string(AutoTileMode m);
bool autotile_mode_from_string(const std::string& s, AutoTileMode* out);

struct TileRule {
  int tile = 0;
  // Rows NW N NE / W - E / SW S SE written "x.x/.-./o.o" (centre ignored).
  std::string pattern = ".../.-./...";
  bool matches(std::uint8_t mask8) const;
  bool operator==(const TileRule&) const = default;
};

struct AutoTileSet {
  std::string name = "Terrain";
  AutoTileMode mode = AutoTileMode::Blob47;
  std::string tileset;        // project-relative image ("" = built-in palette)
  int tile_size = 32;
  int base = 1;               // tile id of layout slot 0
  std::vector<int> map;       // optional explicit ids per slot
  std::vector<TileRule> rules;  // mode rules
  int fill = 0;               // rules: no rule matched (0 = first rule's tile)
  std::vector<int> members;   // extra ids that count as this terrain
  bool edges_connect = true;  // the map border counts as terrain

  int slot_count() const;               // 47 / 16 / rules.size()
  int slot_tile(int slot) const;        // tile id of a layout slot
  bool member(int tile_id) const;       // is this id part of the terrain?
  int resolve(std::uint8_t mask8) const;  // tile for a neighbour mask
  int full_tile() const { return resolve(0xFF); }
  bool operator==(const AutoTileSet&) const = default;
};

// Blob reduction: drop corner bits whose two edges are not both set.
std::uint8_t blob_reduce(std::uint8_t mask8);
// The 47 reduced masks in ascending order (slot i <-> mask).
const std::array<std::uint8_t, 47>& blob47_masks();
int blob47_slot(std::uint8_t mask8);   // 0..46
int wang16_slot(std::uint8_t mask8);   // 0..15

std::uint8_t neighbour_mask(const TileMapData& map, const AutoTileSet& set, int col, int row);
// Re-pick the tile of every terrain cell in [c0..c1] x [r0..r1] (clamped).
std::size_t retile(TileMapData& map, const AutoTileSet& set, int c0, int r0, int c1, int r1);
std::size_t retile_all(TileMapData& map, const AutoTileSet& set);
// Brush: brush x brush cells become terrain (or empty with erase), then
// the cells and their neighbours are re-tiled. Returns cells changed.
std::size_t paint_autotile(TileMapData& map, const AutoTileSet& set, int col, int row, int brush,
                           bool erase);

namespace autotile_json {
bool parse(const std::string& text, AutoTileSet* out, std::string* error);
std::string write(const AutoTileSet& set);
bool load_file(const std::string& path, AutoTileSet* out, std::string* error);
bool save_file(const std::string& path, const AutoTileSet& set, std::string* error);
// <project>/tiles/*.autotile.json, project-relative, sorted.
std::vector<std::string> list(const std::string& project_dir);
}  // namespace autotile_json

// A ready-to-paint template tileset for a mode (each slot drawn from its
// mask: terrain fill + rim), RGBA8, `cols` slots per row. For starting a
// new terrain before the artist paints over it.
struct TemplateImage {
  int width = 0, height = 0, cols = 0;
  std::vector<unsigned char> rgba;
};
TemplateImage autotile_template(AutoTileMode mode, int tile_size, const unsigned char fill_rgba[4],
                                const unsigned char rim_rgba[4]);

}  // namespace tombstone
}  // namespace ts
