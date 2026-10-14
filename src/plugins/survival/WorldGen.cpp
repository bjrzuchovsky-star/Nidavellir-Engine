#include "plugins/survival/WorldGen.h"

#include <cstdlib>
#include <initializer_list>

namespace ts {
namespace tombstone {
namespace survival {

namespace {

std::uint64_t mix(std::uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

std::uint32_t lattice(std::uint64_t seed, int x, int y) {
  const std::uint64_t k = mix(seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32 |
                                      static_cast<std::uint32_t>(y)));
  return static_cast<std::uint32_t>(k & 0xFFFFu);
}

// 16.16 smoothstep of t in [0, 65536].
std::int64_t smooth(std::int64_t t) {
  return t * t / 65536 * (3 * 65536 - 2 * t) / 65536;
}

std::int64_t lerp(std::int64_t a, std::int64_t b, std::int64_t t) {
  return a + (b - a) * t / 65536;
}

constexpr std::uint64_t kHeat = 0x48454154ull;
constexpr std::uint64_t kDry = 0x445259ull;
constexpr std::uint64_t kCave = 0x43415645ull;
constexpr std::uint64_t kDetail = 0x44455441ull;
constexpr std::uint64_t kTunnel = 0x54554e4eull;
constexpr std::uint64_t kScatter = 0x53434154ull;
constexpr std::uint64_t kOre = 0x4f5245ull;

}  // namespace

std::uint32_t WorldGen::noise(std::uint64_t seed, int x, int y, int cell) {
  const int ix = floor_div(x, cell);
  const int iy = floor_div(y, cell);
  const std::int64_t fx = smooth(static_cast<std::int64_t>(floor_mod(x, cell)) * 65536 / cell);
  const std::int64_t fy = smooth(static_cast<std::int64_t>(floor_mod(y, cell)) * 65536 / cell);
  const std::int64_t a = lattice(seed, ix, iy);
  const std::int64_t b = lattice(seed, ix + 1, iy);
  const std::int64_t c = lattice(seed, ix, iy + 1);
  const std::int64_t d = lattice(seed, ix + 1, iy + 1);
  return static_cast<std::uint32_t>(lerp(lerp(a, b, fx), lerp(c, d, fx), fy));
}

std::uint32_t WorldGen::fbm(std::uint64_t seed, int x, int y, int cell) {
  const std::uint32_t n1 = noise(seed, x, y, cell);
  const std::uint32_t n2 = noise(seed ^ 0x9e3779b9ull, x, y, cell > 3 ? cell / 2 : 2);
  const std::uint32_t n3 = noise(seed ^ 0x7f4a7c15ull, x, y, cell > 7 ? cell / 4 : 2);
  return (n1 * 4 + n2 * 2 + n3) / 7;
}

std::uint32_t WorldGen::roll(std::uint64_t seed, int x, int y) {
  return lattice(seed ^ kScatter, x, y) * 10000u / 65536u;
}

Biome WorldGen::biome_at(int tx, int ty) const {
  const int ax = std::abs(tx), ay = std::abs(ty);
  if (ax <= kSpawnClear + 4 && ay <= kSpawnClear + 4) return Biome::Prairie;
  const std::uint32_t cave = fbm(seed_ ^ kCave, tx, ty, 96);
  if (cave > 41000 && (ax > 24 || ay > 24)) return Biome::Caves;
  const std::uint32_t dry = fbm(seed_ ^ kDry, tx, ty, 160);
  const std::uint32_t heat = fbm(seed_ ^ kHeat, tx, ty, 200);
  if (dry > 34500) return Biome::Desert;
  if (heat > 35500) return Biome::Badlands;
  return Biome::Prairie;
}

Tile WorldGen::tile_at(int tx, int ty) const {
  Tile t;
  const Biome b = biome_at(tx, ty);
  const std::uint32_t r = roll(seed_, tx, ty);
  const std::uint32_t detail = fbm(seed_ ^ kDetail, tx, ty, 12);
  const bool spawn = std::abs(tx) <= kSpawnClear && std::abs(ty) <= kSpawnClear;
  switch (b) {
    case Biome::Prairie:
      t.ground = detail > 43000 ? kDirt : kGrass;
      if (detail < 15500 && !spawn) {
        t.ground = kWater;
      } else if (!spawn) {
        if (r < 550) t.object = kTree;
        else if (r < 850) t.object = kBush;
        else if (r < 920) t.object = kBoulder;
      }
      break;
    case Biome::Desert:
      t.ground = kSand;
      if (r < 260) t.object = kCactus;
      else if (r < 400) t.object = kBoulder;
      else if (r < 450) t.object = kBush;
      break;
    case Biome::Badlands: {
      t.ground = detail > 40000 ? kDirt : kRedrock;
      const std::uint32_t mesa = fbm(seed_ ^ kTunnel, tx, ty, 20);
      if (mesa > 41000) {
        const std::uint32_t ore = lattice(seed_ ^ kOre, tx, ty) * 10000u / 65536u;
        t.object = ore < 700 ? kCopperOre : ore < 1050 ? kIronOre : kMesaRock;
      } else if (r < 180) {
        t.object = kBoulder;
      } else if (r < 300) {
        t.object = kCactus;
      }
      break;
    }
    case Biome::Caves: {
      t.ground = kCaveFloor;
      const std::uint32_t tunnel = fbm(seed_ ^ kTunnel, tx, ty, 18);
      const int d = static_cast<int>(tunnel) - 32768;
      if (std::abs(d) > 4200) {
        const std::uint32_t ore = lattice(seed_ ^ kOre, tx, ty) * 10000u / 65536u;
        t.object = ore < 550 ? kCopperOre : ore < 950 ? kIronOre : ore < 1150 ? kGoldOre : kCaveRock;
      }
      break;
    }
  }
  return t;
}

void WorldGen::generate(int cx, int cy, Tile* out) const {
  for (int y = 0; y < kChunk; ++y) {
    for (int x = 0; x < kChunk; ++x) {
      out[y * kChunk + x] = tile_at(cx * kChunk + x, cy * kChunk + y);
    }
  }
}

std::uint64_t chunk_hash(const Tile* tiles) {
  std::uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < kChunk * kChunk; ++i) {
    for (std::uint8_t b : {tiles[i].ground, tiles[i].object, tiles[i].damage}) {
      h ^= b;
      h *= 1099511628211ull;
    }
  }
  return h;
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
