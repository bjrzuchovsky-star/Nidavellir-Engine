#pragma once

// The infinite tile world, kept as 32x32 chunks streamed around riders.
// A chunk comes from (in order) memory, the parked store (edited chunks
// unloaded without a save folder: client mirrors, unsaved rides), the save
// folder (<project>/saves/<world>/c_<cx>_<cy>.chunk), else the generator.
// Edited chunks are written back when they unload and on save_all().

#include "plugins/survival/WorldGen.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace survival {

struct Chunk {
  int cx = 0;
  int cy = 0;
  std::array<Tile, kChunk * kChunk> tiles{};
  bool dirty = false;   // changed since it was last written
  bool edited = false;  // differs from what the generator makes
  Tile& at(int lx, int ly) { return tiles[static_cast<std::size_t>(ly * kChunk + lx)]; }
  const Tile& at(int lx, int ly) const { return tiles[static_cast<std::size_t>(ly * kChunk + lx)]; }
};

struct TileEdit {
  int tx = 0;
  int ty = 0;
  Tile tile;
};

class ChunkMap {
 public:
  using Key = std::pair<int, int>;

  // Forget everything; save_dir "" keeps edits in memory only.
  void reset(std::uint64_t seed, const std::string& save_dir);
  std::uint64_t seed() const { return gen_.seed(); }
  const WorldGen& gen() const { return gen_; }
  const std::string& save_dir() const { return save_dir_; }

  Chunk* find(int cx, int cy);
  const Chunk* find(int cx, int cy) const;
  Chunk& ensure(int cx, int cy);

  // Tile access in tile coordinates. peek: loaded chunks only.
  const Tile* peek(int tx, int ty) const;
  Tile get(int tx, int ty);  // loads / generates
  // Store a tile (loads its chunk); records the edit. False when unchanged.
  bool set(int tx, int ty, const Tile& t);
  // Replace a whole chunk (a server's copy on a client).
  void put_chunk(const Chunk& c);

  // Load chunks within load_radius of each centre (world px); save and
  // drop those beyond keep_radius of all of them. Returns chunks loaded.
  int stream(const std::vector<std::pair<float, float>>& centres, int load_radius = 2,
             int keep_radius = 3);
  void save_all();
  std::size_t loaded() const { return chunks_.size(); }
  const std::map<Key, std::unique_ptr<Chunk>>& chunks() const { return chunks_; }
  std::size_t written() const { return written_; }
  std::size_t read_from_disk() const { return read_; }

  std::vector<TileEdit> take_edits();

  // Save format: "TSCH", version, gen version, seed, cx, cy, edited flag,
  // then RLE runs of (ground, object, damage).
  static std::vector<std::uint8_t> encode(const Chunk& c, std::uint64_t seed);
  static bool decode(const std::vector<std::uint8_t>& bytes, Chunk* out,
                     std::uint64_t* seed_out);
  std::string chunk_path(int cx, int cy) const;

 private:
  bool load_from_disk(int cx, int cy, Chunk* out);
  bool write(const Chunk& c);
  void unload(std::map<Key, std::unique_ptr<Chunk>>::iterator it);

  WorldGen gen_;
  std::string save_dir_;
  std::map<Key, std::unique_ptr<Chunk>> chunks_;
  std::map<Key, std::unique_ptr<Chunk>> parked_;
  std::vector<TileEdit> edits_;
  std::size_t written_ = 0;
  std::size_t read_ = 0;
};

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
