#pragma once

// Seeded procedural generation for the chunked frontier. Integer-only
// value noise (no floating point), so a seed builds the same world on
// every compiler and machine: the server and its clients generate chunks
// independently and only edits travel over the wire.
//
// Biomes: prairie (grass, cottonwoods, sage, ponds), desert (sand,
// saguaros, boulders), badlands (red rock, mesa outcrops with copper and
// iron), caves (rock mazes with copper, iron and gold). The ground around
// the origin is always open prairie: riders start there.

#include "plugins/survival/Tiles.h"

#include <cstdint>

namespace ts {
namespace tombstone {
namespace survival {

class WorldGen {
 public:
  static constexpr int kSpawnClear = 6;  // tiles around (0, 0) kept open

  explicit WorldGen(std::uint64_t seed = 1849) : seed_(seed) {}
  std::uint64_t seed() const { return seed_; }

  Biome biome_at(int tx, int ty) const;
  Tile tile_at(int tx, int ty) const;
  // kChunk x kChunk tiles, row-major, of chunk (cx, cy).
  void generate(int cx, int cy, Tile* out) const;

  // 0..65535 smooth noise (cell = feature size in tiles), exposed for the
  // editor preview.
  static std::uint32_t noise(std::uint64_t seed, int x, int y, int cell);
  static std::uint32_t fbm(std::uint64_t seed, int x, int y, int cell);
  // Per-tile white noise 0..9999.
  static std::uint32_t roll(std::uint64_t seed, int x, int y);

 private:
  std::uint64_t seed_;
};

// FNV-1a over a chunk's tiles: determinism checks and the save format.
std::uint64_t chunk_hash(const Tile* tiles);

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
