#include "plugins/survival/ChunkMap.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace ts {
namespace tombstone {
namespace survival {

namespace fs = std::filesystem;

namespace {

constexpr std::uint8_t kMagic[4] = {'T', 'S', 'C', 'H'};
constexpr std::uint8_t kFormat = 1;

void put_u64(std::vector<std::uint8_t>* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_i32(std::vector<std::uint8_t>* out, std::int32_t v) {
  const auto u = static_cast<std::uint32_t>(v);
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>(u >> (8 * i)));
}

}  // namespace

void ChunkMap::reset(std::uint64_t seed, const std::string& save_dir) {
  gen_ = WorldGen(seed);
  save_dir_ = save_dir;
  chunks_.clear();
  parked_.clear();
  edits_.clear();
  written_ = read_ = 0;
}

Chunk* ChunkMap::find(int cx, int cy) {
  auto it = chunks_.find({cx, cy});
  return it == chunks_.end() ? nullptr : it->second.get();
}

const Chunk* ChunkMap::find(int cx, int cy) const {
  auto it = chunks_.find({cx, cy});
  return it == chunks_.end() ? nullptr : it->second.get();
}

Chunk& ChunkMap::ensure(int cx, int cy) {
  if (Chunk* c = find(cx, cy)) return *c;
  auto chunk = std::make_unique<Chunk>();
  auto parked = parked_.find({cx, cy});
  if (parked != parked_.end()) {
    chunk = std::move(parked->second);
    parked_.erase(parked);
  } else if (!load_from_disk(cx, cy, chunk.get())) {
    chunk->cx = cx;
    chunk->cy = cy;
    gen_.generate(cx, cy, chunk->tiles.data());
  }
  Chunk& ref = *chunk;
  chunks_[{cx, cy}] = std::move(chunk);
  return ref;
}

const Tile* ChunkMap::peek(int tx, int ty) const {
  const Chunk* c = find(floor_div(tx, kChunk), floor_div(ty, kChunk));
  return c ? &c->at(floor_mod(tx, kChunk), floor_mod(ty, kChunk)) : nullptr;
}

Tile ChunkMap::get(int tx, int ty) {
  return ensure(floor_div(tx, kChunk), floor_div(ty, kChunk))
      .at(floor_mod(tx, kChunk), floor_mod(ty, kChunk));
}

bool ChunkMap::set(int tx, int ty, const Tile& t) {
  Chunk& c = ensure(floor_div(tx, kChunk), floor_div(ty, kChunk));
  Tile& cur = c.at(floor_mod(tx, kChunk), floor_mod(ty, kChunk));
  if (cur == t) return false;
  cur = t;
  c.dirty = c.edited = true;
  edits_.push_back({tx, ty, t});
  return true;
}

void ChunkMap::put_chunk(const Chunk& c) {
  parked_.erase({c.cx, c.cy});
  auto chunk = std::make_unique<Chunk>(c);
  chunk->dirty = false;
  chunks_[{c.cx, c.cy}] = std::move(chunk);
}

int ChunkMap::stream(const std::vector<std::pair<float, float>>& centres, int load_radius,
                     int keep_radius) {
  int loaded = 0;
  std::vector<Key> centre_chunks;
  for (const auto& [x, y] : centres) {
    const int tx = static_cast<int>(std::floor(x / kTile));
    const int ty = static_cast<int>(std::floor(y / kTile));
    centre_chunks.push_back({floor_div(tx, kChunk), floor_div(ty, kChunk)});
  }
  for (const Key& k : centre_chunks) {
    for (int dy = -load_radius; dy <= load_radius; ++dy) {
      for (int dx = -load_radius; dx <= load_radius; ++dx) {
        if (!find(k.first + dx, k.second + dy)) {
          ensure(k.first + dx, k.second + dy);
          ++loaded;
        }
      }
    }
  }
  if (centre_chunks.empty()) return loaded;
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    bool keep = false;
    for (const Key& k : centre_chunks) {
      if (std::abs(it->first.first - k.first) <= keep_radius &&
          std::abs(it->first.second - k.second) <= keep_radius) {
        keep = true;
        break;
      }
    }
    if (keep) {
      ++it;
    } else {
      auto next = std::next(it);
      unload(it);
      it = next;
    }
  }
  return loaded;
}

void ChunkMap::unload(std::map<Key, std::unique_ptr<Chunk>>::iterator it) {
  Chunk& c = *it->second;
  if (c.edited) {
    if (save_dir_.empty() || !write(c)) {
      parked_[it->first] = std::move(it->second);
    }
  }
  chunks_.erase(it);
}

void ChunkMap::save_all() {
  if (save_dir_.empty()) return;
  for (auto& [k, c] : chunks_) {
    if (c->dirty && c->edited) write(*c);
  }
  for (auto it = parked_.begin(); it != parked_.end();) {
    if (write(*it->second)) it = parked_.erase(it);
    else ++it;
  }
}

std::vector<TileEdit> ChunkMap::take_edits() {
  std::vector<TileEdit> out;
  out.swap(edits_);
  return out;
}

std::string ChunkMap::chunk_path(int cx, int cy) const {
  return (fs::path(save_dir_) / ("c_" + std::to_string(cx) + "_" + std::to_string(cy) + ".chunk"))
      .string();
}

std::vector<std::uint8_t> ChunkMap::encode(const Chunk& c, std::uint64_t seed) {
  std::vector<std::uint8_t> out(kMagic, kMagic + 4);
  out.push_back(kFormat);
  out.push_back(static_cast<std::uint8_t>(kGenVersion));
  put_u64(&out, seed);
  put_i32(&out, c.cx);
  put_i32(&out, c.cy);
  out.push_back(c.edited ? 1 : 0);
  const int n = kChunk * kChunk;
  for (int i = 0; i < n;) {
    int run = 1;
    while (i + run < n && run < 255 && c.tiles[static_cast<std::size_t>(i + run)] ==
                                          c.tiles[static_cast<std::size_t>(i)]) {
      ++run;
    }
    const Tile& t = c.tiles[static_cast<std::size_t>(i)];
    out.push_back(static_cast<std::uint8_t>(run));
    out.push_back(t.ground);
    out.push_back(t.object);
    out.push_back(t.damage);
    i += run;
  }
  return out;
}

bool ChunkMap::decode(const std::vector<std::uint8_t>& b, Chunk* out, std::uint64_t* seed_out) {
  if (b.size() < 23 || !std::equal(kMagic, kMagic + 4, b.begin()) || b[4] != kFormat) return false;
  std::uint64_t seed = 0;
  for (int i = 0; i < 8; ++i) seed |= static_cast<std::uint64_t>(b[6 + i]) << (8 * i);
  auto i32 = [&](std::size_t at) {
    std::uint32_t u = 0;
    for (int i = 0; i < 4; ++i) u |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
    return static_cast<std::int32_t>(u);
  };
  out->cx = i32(14);
  out->cy = i32(18);
  out->edited = b[22] != 0;
  std::size_t pos = 23;
  int filled = 0;
  while (pos + 4 <= b.size() && filled < kChunk * kChunk) {
    const int run = b[pos];
    Tile t;
    t.ground = b[pos + 1];
    t.object = b[pos + 2];
    t.damage = b[pos + 3];
    pos += 4;
    if (run == 0 || filled + run > kChunk * kChunk || t.ground >= kGroundCount ||
        t.object >= kObjCount) {
      return false;
    }
    for (int k = 0; k < run; ++k) out->tiles[static_cast<std::size_t>(filled++)] = t;
  }
  if (filled != kChunk * kChunk || pos != b.size()) return false;
  if (seed_out) *seed_out = seed;
  out->dirty = false;
  return true;
}

bool ChunkMap::load_from_disk(int cx, int cy, Chunk* out) {
  if (save_dir_.empty()) return false;
  std::ifstream in(chunk_path(cx, cy), std::ios::binary);
  if (!in) return false;
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  std::uint64_t seed = 0;
  Chunk c;
  if (!decode(bytes, &c, &seed) || seed != gen_.seed() || c.cx != cx || c.cy != cy) return false;
  *out = c;
  ++read_;
  return true;
}

bool ChunkMap::write(const Chunk& c) {
  std::error_code ec;
  fs::create_directories(save_dir_, ec);
  const std::string path = chunk_path(c.cx, c.cy);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::vector<std::uint8_t> bytes = encode(c, gen_.seed());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) return false;
  }
  const_cast<Chunk&>(c).dirty = false;
  ++written_;
  return true;
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
