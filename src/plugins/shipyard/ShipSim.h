#pragma once

// The airship simulation (ts.shipyard): rigid bodies built from parts,
// thrust allocated per engine, steam and crew, turrets and shot, part damage
// and ships breaking into drifting wrecks, pirate captains, and the wire
// format a posse server uses to mirror all of it onto clients.
//
// Roles (runtime::World::net_role()):
//   Local / Server  simulate (fixed tick). A server sends every client the
//                   ships' layouts ('F' on spawn / split / join) and a state
//                   message ('S': poses, part hp, shot) every kStateEvery.
//   Client          never simulates: applies what the server sends.
//
// Controls (a rider's PlayerInput): W / S forward / astern, A / D strafe,
// Q (kButtonPrev) / E (kButtonAction) turn, F / C / left mouse fire.

#include "plugins/shipyard/Design.h"
#include "runtime/World.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace shipyard {

inline constexpr const char* kChannel = "shipyard";

enum class Team : std::uint8_t { Posse = 0, Pirates = 1, Derelict = 2 };
const char* team_name(Team t);

struct LivePart {
  int def = -1;
  int x = 0, y = 0, rot = 0;  // grid cell, quarter turns
  int hp = 1;
  bool powered = true;
  bool firing = false;  // engines: pushing this tick; guns: just fired
  float cooldown = 0.0f;
  float aim = 0.0f;     // guns: barrel heading (world rad, 0 = up)
};

struct Ship {
  std::uint32_t id = 0;
  std::string name;
  int owner = -1;  // rider slot, -1 = none
  Team team = Team::Pirates;
  bool ai = false;
  std::vector<LivePart> parts;
  // Body (centre of mass, world px; angle rad clockwise, 0 = bow up).
  float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
  float angle = 0.0f, spin = 0.0f;
  float prev_x = 0.0f, prev_y = 0.0f;
  // Derived from the parts (relayout()).
  float cx = 0.0f, cy = 0.0f;  // centre of mass, grid px
  float mass = 1.0f, inertia = 1.0f, radius = 8.0f;
  float shield = 0.0f, shield_max = 0.0f;
  float gun_range = 0.0f;
  // Helm this tick.
  float in_x = 0.0f, in_y = 0.0f;
  int turn = 0;
  bool fire = false;
  // Pirate captain.
  float orbit = 1.0f;
  std::uint32_t target = 0;
  // Bookkeeping.
  bool layout_dirty = true;
  float hit_flash = 0.0f;
  std::vector<int> cells;  // cell map over [gx0, gx0 + gw) x [gy0, gy0 + gh)
  int gx0 = 0, gy0 = 0, gw = 0, gh = 0;

  int part_at_cell(int x, int y) const;
  std::vector<PartPose> poses() const;
  // Grid px -> world px, and back.
  void to_world(float gx, float gy, float* wx, float* wy) const;
  void to_grid(float wx, float wy, float* gx, float* gy) const;
};

struct Shot {
  float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
  int damage = 0;
  float life = 1.0f;
  std::uint32_t ship = 0;  // who fired
  Team team = Team::Pirates;
};

struct EnemySpawn {
  std::string design;  // project-relative .ship.json
  float x = 0.0f, y = 0.0f;
};

// shipyard/sky.json.
struct SkySettings {
  std::string player_design = "shipyard/designs/sloop.ship.json";
  std::vector<EnemySpawn> enemies;
  bool crew = true;            // parts need hands from bunks
  float respawn = 4.0f;        // s after a rider's ship is lost
  float reinforce = 25.0f;     // s after the last pirate falls (0 = never)
  bool parse(const std::string& text, std::string* error);
};

class ShipSim {
 public:
  static constexpr int kSlots = kMaxPlayers;
  static constexpr int kStateEvery = 2;

  // ---- Lifecycle -------------------------------------------------------------
  // Local / Server: designs come from project_dir (missing ones fall back to
  // the built-in sloop / marauder).
  void begin(runtime::World* world, const std::string& project_dir, const PartLibrary& lib,
             const SkySettings& sky);
  void begin_client(const PartLibrary& lib);
  void end();
  bool active() const { return active_; }
  bool mirror() const { return mirror_; }

  // ---- Per tick ----------------------------------------------------------------
  void step(runtime::World& world);
  // Without a World (smokes): helms are whatever set_helm said.
  void step_bare();
  void receive(runtime::World& world, int from_slot, const std::vector<std::uint8_t>& bytes);

  // ---- Direct (Lua, smokes) --------------------------------------------------
  std::uint32_t spawn(const Design& d, float x, float y, Team team, bool ai, int owner = -1,
                      float angle = 0.0f);
  bool spawn_file(const std::string& rel_path, float x, float y, Team team, bool ai,
                  std::uint32_t* id, std::string* error);
  void set_helm(std::uint32_t id, float in_x, float in_y, int turn, bool fire);
  // Hurt part `index` of ship `id`; true when the part was destroyed.
  bool damage_part(std::uint32_t id, int index, int amount);
  bool remove_ship(std::uint32_t id);
  Ship* find(std::uint32_t id);
  const Ship* find(std::uint32_t id) const;
  std::uint32_t ship_of(int slot) const;

  // ---- Queries -------------------------------------------------------------------
  const std::vector<Ship>& ships() const { return ships_; }
  std::vector<Ship>& ships_mut() { return ships_; }
  const std::vector<Shot>& shots() const { return shots_; }
  const PartLibrary& lib() const { return lib_; }
  const SkySettings& sky() const { return sky_; }
  std::uint64_t ticks() const { return ticks_; }
  int my_slot() const { return my_slot_; }
  std::uint64_t parts_destroyed() const { return parts_destroyed_; }
  std::uint64_t splits() const { return splits_; }
  std::uint64_t shots_fired() const { return shots_fired_; }
  std::uint64_t states_received() const { return states_received_; }
  const std::vector<std::string>& warnings() const { return warnings_; }
  // FNV-1a over every body, part and shot (determinism checks).
  std::uint64_t hash() const;

  // Physics pieces (smokes check them against hand math).
  // Engine push this tick for the ship's helm: local force + torque.
  void allocate_thrust(Ship& s, float* fx, float* fy, float* torque) const;
  void apply_power(Ship& s) const;
  void relayout(Ship& s) const;

  // Wire (smokes).
  std::vector<std::uint8_t> encode_full(const Ship& s) const;
  std::vector<std::uint8_t> encode_state() const;

  // The built-in designs the sample ships with.
  static Design sloop_design();
  static Design marauder_design();

 private:
  void tick(runtime::World* w);
  void riders(runtime::World& w);
  void captains();
  void physics();
  void guns();
  void step_shots();
  void break_up(std::size_t index);
  std::uint32_t pick_target(const Ship& s, float range) const;
  void reinforce();
  void post(runtime::World& w, int slot, std::vector<std::uint8_t> bytes);
  void apply(const std::vector<std::uint8_t>& bytes);

  PartLibrary lib_;
  SkySettings sky_;
  std::string project_dir_;
  Design player_design_;
  std::vector<Ship> ships_;
  std::vector<Shot> shots_;
  std::array<bool, kSlots> present_{};
  std::array<double, kSlots> respawn_at_{};
  std::array<std::pair<float, float>, kSlots> spawn_at_{};
  std::vector<std::string> warnings_;
  std::uint32_t next_id_ = 1;
  std::uint64_t ticks_ = 0;
  std::uint64_t parts_destroyed_ = 0;
  std::uint64_t splits_ = 0;
  std::uint64_t shots_fired_ = 0;
  // Sounds made this tick (guns, breakups); tick() hands them to the world.
  std::vector<runtime::SoundEvent> sounds_;
  void sound(const char* clip, float x, float y, float volume, float pitch);
  std::uint64_t states_received_ = 0;
  double reinforce_at_ = -1.0;
  int my_slot_ = 0;
  bool active_ = false;
  bool mirror_ = false;
  bool full_all_ = false;  // resend every layout next tick
};

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
