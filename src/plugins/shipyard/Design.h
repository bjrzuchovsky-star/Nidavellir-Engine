#pragma once

// Ship designs (.ship.json): parts placed on a grid, plus the numbers the
// Airship Works shows and the rules a design must pass before it flies:
//   connected     every part touches the rest (edge to edge)
//   steam         boilers make at least the steam the parts burn
//   thrust        engines push forward; back / side / turn cover warned
//   crew          bunk berths for the hands the parts need (warning)

#include "plugins/shipyard/Parts.h"

#include <array>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace shipyard {

inline constexpr int kMaxGrid = 40;  // cells per side
inline constexpr int kDesignVersion = 1;

struct Placed {
  std::string part;  // PartDef id
  int x = 0, y = 0;  // top-left cell
  int rot = 0;       // quarter turns clockwise
  bool operator==(const Placed& o) const = default;
};

struct Design {
  std::string name = "Unnamed airship";
  std::vector<Placed> parts;
  bool operator==(const Design& o) const = default;
};

// A part resolved against the library (designs and live ships).
struct PartPose {
  int def = -1;
  int x = 0, y = 0, rot = 0;
};

// Footprint after rotation.
void footprint(const PartDef& d, int rot, int* w, int* h);
// Unit push of an engine at `rot` in grid space (rot 0 = (0,-1), up).
void engine_dir(int rot, float* dx, float* dy);
// Centre of a placed part in grid px (cell centres at (i + 0.5) * kCell).
void part_center(const PartDef& d, const PartPose& p, float* cx, float* cy);

enum Dir4 { kFwd = 0, kBack = 1, kLeft = 2, kRight = 3 };

struct ShipStats {
  int parts = 0;
  int cells = 0;
  float mass = 0.0f;
  float cx = 0.0f, cy = 0.0f;  // centre of mass, grid px
  float inertia = 0.0f;        // ton px^2 about the centre of mass
  std::array<float, 4> thrust{};  // fwd (-y) / back / left (-x) / right
  float torque_cw = 0.0f, torque_ccw = 0.0f;  // best turning push each way
  float power_made = 0.0f, power_used = 0.0f;
  int hp = 0;
  int guns = 0;
  float dps = 0.0f;
  float shield = 0.0f;
  int berths = 0, hands = 0;
  int capacity = 0;
  float accel = 0.0f;      // px / s^2 forward
  float top_speed = 0.0f;  // px / s forward (with damping)
  float radius = 0.0f;     // px, centre of mass to the farthest corner
};

// The physics constants every ship uses (ShipSim too).
inline constexpr float kLinearDamping = 0.55f;   // 1 / s
inline constexpr float kAngularDamping = 1.1f;   // 1 / s

ShipStats compute_stats(const PartLibrary& lib, const std::vector<PartPose>& parts);

struct Validation {
  bool ok = false;               // flyable (no errors)
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<int> loose;        // part indices not attached to part 0
  ShipStats stats;
};

// Resolve against the library; unknown ids are reported in *missing.
std::vector<PartPose> resolve(const PartLibrary& lib, const Design& d,
                              std::vector<std::string>* missing = nullptr);
Validation validate(const PartLibrary& lib, const Design& d);
// Connected groups (edge-adjacent cells) of the given parts, each a list of
// indices into `parts`, largest first (ties: lowest first index).
std::vector<std::vector<int>> components(const PartLibrary& lib,
                                         const std::vector<PartPose>& parts);

// ---- Editing (the designer and smokes) ----------------------------------------
// Index of the part covering cell (cx, cy), or -1.
int part_at(const PartLibrary& lib, const Design& d, int cx, int cy);
// Place `part` with its top-left at (x, y). False (with *why) when it is off
// the grid, unknown, or overlaps.
bool place_part(const PartLibrary& lib, Design* d, const std::string& part, int x, int y,
                int rot, std::string* why = nullptr);
bool erase_at(const PartLibrary& lib, Design* d, int cx, int cy);
// Turn the part under the cell a quarter clockwise if it still fits.
bool rotate_at(const PartLibrary& lib, Design* d, int cx, int cy);

// ---- Files ---------------------------------------------------------------------
std::string write_design(const Design& d);
bool parse_design(const std::string& text, Design* out, std::string* error);
bool save_design(const std::string& path, const Design& d, std::string* error);
bool load_design(const std::string& path, Design* out, std::string* error);
// <project>/shipyard/designs/*.ship.json, sorted (project-relative paths).
std::vector<std::string> list_designs(const std::string& project_dir);

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
