#include "plugins/survival/Tiles.h"

#include <cstring>

namespace ts {
namespace tombstone {
namespace survival {

namespace {

const GroundDef kGrounds[kGroundCount] = {
    {"none", {0.05f, 0.04f, 0.04f}, true},
    {"sand", {0.86f, 0.74f, 0.50f}, true},
    {"grass", {0.45f, 0.58f, 0.28f}, true},
    {"dirt", {0.52f, 0.38f, 0.24f}, true},
    {"redrock", {0.70f, 0.38f, 0.24f}, true},
    {"cave_floor", {0.30f, 0.26f, 0.24f}, true},
    {"water", {0.24f, 0.46f, 0.62f}, false},
    {"planks", {0.62f, 0.45f, 0.27f}, true},
};

const ObjectDef kObjects[kObjCount] = {
    {"none", "Nothing", false, 0, 0, "", 0, {0, 0, 0}, 0.0f, false},
    {"tree", "Cottonwood", true, 6, 0, "wood", 4, {0.22f, 0.45f, 0.20f}, 0.0f, false},
    {"cactus", "Saguaro", true, 4, 0, "cactus_flesh", 2, {0.30f, 0.58f, 0.30f}, 0.0f, false},
    {"boulder", "Boulder", true, 8, 1, "stone", 3, {0.55f, 0.52f, 0.48f}, 0.0f, false},
    {"cave_rock", "Cave rock", true, 6, 1, "stone", 1, {0.40f, 0.36f, 0.34f}, 0.0f, false},
    {"mesa_rock", "Mesa rock", true, 7, 1, "stone", 1, {0.62f, 0.30f, 0.20f}, 0.0f, false},
    {"copper_ore", "Copper vein", true, 9, 1, "copper_ore", 2, {0.80f, 0.47f, 0.25f}, 0.0f, false},
    {"iron_ore", "Iron vein", true, 12, 2, "iron_ore", 2, {0.70f, 0.66f, 0.62f}, 0.0f, false},
    {"gold_ore", "Gold vein", true, 14, 2, "gold_nugget", 2, {0.95f, 0.80f, 0.25f}, 0.0f, false},
    {"bush", "Sagebrush", false, 1, 0, "fiber", 2, {0.48f, 0.56f, 0.40f}, 0.0f, false},
    {"wood_wall", "Plank wall", true, 5, 0, "wood_wall", 1, {0.48f, 0.33f, 0.19f}, 0.0f, false},
    {"door", "Swinging door", false, 4, 0, "door", 1, {0.55f, 0.38f, 0.22f}, 0.0f, false},
    {"workbench", "Workbench", true, 4, 0, "workbench", 1, {0.58f, 0.42f, 0.26f}, 0.0f, true},
    {"forge", "Forge", true, 8, 0, "forge", 1, {0.36f, 0.33f, 0.32f}, 3.0f, true},
    {"bed", "Bedroll", false, 2, 0, "bed", 1, {0.65f, 0.25f, 0.22f}, 0.0f, false},
    {"torch", "Torch", false, 1, 0, "torch", 1, {0.95f, 0.70f, 0.30f}, 4.5f, false},
    {"campfire", "Campfire", true, 3, 0, "campfire", 1, {0.90f, 0.45f, 0.18f}, 6.0f, true},
};

}  // namespace

const char* biome_name(Biome b) {
  switch (b) {
    case Biome::Desert: return "desert";
    case Biome::Badlands: return "badlands";
    case Biome::Caves: return "caves";
    case Biome::Prairie: break;
  }
  return "prairie";
}

bool biome_from_name(const char* s, Biome* out) {
  for (int i = 0; i < kBiomeCount; ++i) {
    if (std::strcmp(s, biome_name(static_cast<Biome>(i))) == 0) {
      *out = static_cast<Biome>(i);
      return true;
    }
  }
  return false;
}

const GroundDef& ground_def(std::uint8_t g) {
  return kGrounds[g < kGroundCount ? g : 0];
}

const ObjectDef& object_def(std::uint8_t o) {
  return kObjects[o < kObjCount ? o : 0];
}

bool ground_by_id(const char* id, std::uint8_t* out) {
  for (int i = 1; i < kGroundCount; ++i) {
    if (std::strcmp(kGrounds[i].id, id) == 0) {
      *out = static_cast<std::uint8_t>(i);
      return true;
    }
  }
  return false;
}

bool object_by_id(const char* id, std::uint8_t* out) {
  for (int i = 1; i < kObjCount; ++i) {
    if (std::strcmp(kObjects[i].id, id) == 0) {
      *out = static_cast<std::uint8_t>(i);
      return true;
    }
  }
  return false;
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
