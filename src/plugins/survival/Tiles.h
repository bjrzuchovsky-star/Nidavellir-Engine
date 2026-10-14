#pragma once

// Frontier Survival: the tile world's building blocks. A tile is a ground
// (floor) plus an optional object (tree, rock, ore, wall, workbench...).
// The tables here are engine-side; items, recipes and creatures come from
// the project's data files (Data.h).

#include <cstdint>

namespace ts {
namespace tombstone {
namespace survival {

inline constexpr int kChunk = 32;          // tiles per chunk side
inline constexpr float kTile = 32.0f;      // world px per tile
inline constexpr int kGenVersion = 1;      // bump when generation changes

enum Ground : std::uint8_t {
  kGroundNone = 0,
  kSand,
  kGrass,
  kDirt,
  kRedrock,
  kCaveFloor,
  kWater,
  kPlanks,
  kGroundCount,
};

enum Object : std::uint8_t {
  kObjNone = 0,
  kTree,
  kCactus,
  kBoulder,
  kCaveRock,
  kMesaRock,
  kCopperOre,
  kIronOre,
  kGoldOre,
  kBush,
  kWoodWall,
  kDoor,
  kWorkbench,
  kForge,
  kBed,
  kTorch,
  kCampfire,
  kObjCount,
};

enum class Biome : std::uint8_t { Prairie, Desert, Badlands, Caves };
inline constexpr int kBiomeCount = 4;
const char* biome_name(Biome b);           // "prairie"...
bool biome_from_name(const char* s, Biome* out);

struct Tile {
  std::uint8_t ground = kGroundNone;
  std::uint8_t object = kObjNone;
  std::uint8_t damage = 0;  // mining progress on the object
  bool operator==(const Tile& o) const = default;
};

struct GroundDef {
  const char* id;     // "grass"
  float color[3];
  bool walkable;      // false: water
};

struct ObjectDef {
  const char* id;      // "tree" (items name objects by this id)
  const char* name;    // "Cottonwood"
  bool solid;          // blocks movement
  int hardness;        // hit points against mining power
  int min_tier;        // pick tier needed (0 = bare hands)
  const char* drop;    // item id ("" = nothing)
  int drop_count;
  float color[3];
  float light;         // glow radius in tiles at night (0 = none)
  bool station;        // crafting station
};

const GroundDef& ground_def(std::uint8_t g);
const ObjectDef& object_def(std::uint8_t o);
// Lookup by id; false when unknown.
bool ground_by_id(const char* id, std::uint8_t* out);
bool object_by_id(const char* id, std::uint8_t* out);

inline bool tile_solid(const Tile& t) {
  return !ground_def(t.ground).walkable || object_def(t.object).solid;
}

// Floor division that rounds toward -infinity (chunk / tile coordinates).
inline int floor_div(int a, int b) {
  const int q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
inline int floor_mod(int a, int b) { return a - floor_div(a, b) * b; }

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
