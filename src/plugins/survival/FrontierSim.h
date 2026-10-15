#pragma once

// Frontier Survival, the simulation: chunk streaming around riders,
// mining / placing, saddlebags and crafting, health and combat, creatures
// (wander / chase / attack, spawned by biome and night), the day / night
// cycle and settlers. One instance per World.
//
// Roles (runtime::World::net_role()):
//   Local   editor Play and ts_game: simulates, saves.
//   Server  ts_server: simulates, saves, and sends clients what they need
//           (world seed, edited chunks, tile edits, creature state, each
//           rider's own saddlebags). Inventory and combat live here only.
//   Client  ts_client: never simulates. Generates chunks from the seed,
//           applies the server's edits and state, and asks the server for
//           hotbar / craft / swap changes over the "frontier" channel.

#include "plugins/survival/ChunkMap.h"
#include "plugins/survival/Data.h"
#include "plugins/survival/Inventory.h"
#include "runtime/World.h"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace survival {

inline constexpr const char* kChannel = "frontier";

struct HouseCheck {
  bool ok = false;
  int area = 0;   // floor tiles inside
  int doors = 0;
  std::string why;  // when not ok
};

class FrontierSim {
 public:
  static constexpr int kSlots = kMaxPlayers;
  static constexpr int kMaxHp = 100;
  static constexpr float kShotSpeed = 420.0f;   // px / s
  static constexpr int kStateEvery = 6;          // ticks between state messages

  struct Player {
    Inventory inv;
    int hotbar = 0;
    int hp = kMaxHp;
    int max_hp = kMaxHp;
    double cooldown = 0.0;   // until the next swing / shot
    double hurt = 0.0;       // invulnerable after a hit
    double toast_cd = 0.0;   // throttle for "too tough" style hints
    std::uint32_t prev_buttons = 0;
    bool present = false;
    bool inv_dirty = true;
    int deaths = 0;
    int target_tx = 0, target_ty = 0;  // tile in front of the rider
    float step_x = 0.0f, step_y = 0.0f;  // where the last footstep fell
    bool stepped = false;
    std::set<std::pair<int, int>> sent;  // edited chunks this client has
  };

  struct Mob {
    std::uint32_t id = 0;
    int def = 0;
    float x = 0.0f, y = 0.0f;  // centre, world px
    int hp = 1;
    std::uint8_t state = 0;    // 0 wander, 1 chase, 2 idle
    float timer = 0.0f;
    float cd = 0.0f;
    float dx = 0.0f, dy = 0.0f;
    float hurt = 0.0f;         // flash timer
    int home_tx = 0, home_ty = 0;  // settlers
    bool settler = false;
  };

  struct Shot {
    float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
    int damage = 0;
    float life = 1.0f;
    int owner = -1;
  };

  // ---- Lifecycle -----------------------------------------------------------
  // Local / Server: read data + world settings, open the save
  // (<project>/saves/<world>/), place riders. save = false keeps the ride
  // in memory (smokes).
  bool begin(runtime::World& world, const std::string& project_dir, const GameData& data,
             bool save, std::string* error);
  // Client: data only; the server's Welcome supplies seed and clock.
  void begin_client(const GameData& data);
  void end();  // saves (Local / Server)
  bool active() const { return active_; }
  bool mirror() const { return mirror_; }
  bool has_seed() const { return seeded_; }

  // ---- Per tick (Local / Server) --------------------------------------------
  void step(runtime::World& world);
  // Client: stream chunks around the riders it can see (called from draw).
  void client_frame(const runtime::World& world);
  void receive(runtime::World& world, int from_slot, const std::vector<std::uint8_t>& bytes);

  // ---- Actions (any role; a client forwards them to the server) ------------
  void select(runtime::World& world, int slot, int hotbar);
  bool craft(runtime::World& world, int slot, int recipe, std::string* why = nullptr);
  void swap(runtime::World& world, int slot, int a, int b);
  bool can_craft(int slot, int recipe, std::string* why = nullptr) const;
  // Station objects within reach of the rider (Local / Server; client: the
  // mirror's tiles).
  bool near_station(int slot, std::uint8_t station) const;

  // ---- Direct (Lua, smokes) -------------------------------------------------
  int give(int slot, int item, int n);
  int take(int slot, int item, int n);
  void hurt_player(runtime::World& world, int slot, int damage, const std::string& by);
  void heal_player(int slot, int n);
  std::uint32_t spawn(int creature, float x, float y);
  bool set_tile(int tx, int ty, const Tile& t);
  Tile tile(int tx, int ty);
  HouseCheck check_house(int bed_tx, int bed_ty);

  // ---- Queries --------------------------------------------------------------
  const GameData& data() const { return data_; }
  ChunkMap& chunks() { return chunks_; }
  const ChunkMap& chunks() const { return chunks_; }
  const Player& player(int slot) const { return players_[static_cast<std::size_t>(slot)]; }
  Player& player_mut(int slot) { return players_[static_cast<std::size_t>(slot)]; }
  const std::vector<Mob>& mobs() const { return mobs_; }
  const std::vector<Shot>& shots() const { return shots_; }
  int my_slot() const { return my_slot_; }
  double clock() const { return clock_; }
  double day_length() const { return day_len_; }
  double phase() const;      // 0 dawn .. 1
  bool night() const;
  float darkness() const;    // 0 day .. ~0.6 deep night
  std::uint64_t kills() const { return kills_; }
  std::uint64_t spawned_total() const { return spawned_total_; }
  std::size_t settlers() const;
  const std::string& save_dir() const { return save_dir_; }
  const std::vector<std::string>& warnings() const { return warnings_; }
  std::uint64_t edits_sent() const { return edits_sent_; }
  std::uint64_t edits_received() const { return edits_received_; }
  bool data_mismatch() const { return data_mismatch_; }

  // Optional chase pathing (ts.ai): a unit heading from (x, y) toward the
  // target around walls; false = fall back to the straight chase.
  using MobSteer = std::function<bool(const runtime::World& w, std::uint32_t mob, float x, float y,
                                      float tx, float ty, float half,
                                      const std::function<bool(float, float)>& walkable,
                                      float* dx, float* dy)>;
  static void set_mob_steer(MobSteer steer);
  bool tile_walkable_px(float x, float y, float half) const { return walkable_px(x, y, half); }

  // Wire helpers (also used by the smokes).
  std::vector<std::uint8_t> encode_state() const;

 private:
  void toast(runtime::World& w, int slot, const std::string& text, double seconds = 2.5);
  void hint(runtime::World& w, int slot, const std::string& text);
  // A positional sound at (x, y) world px (servers replicate it).
  void sound(runtime::World& w, const char* clip, float x, float y, float volume = 1.0f,
             float pitch = 1.0f, float variation = 0.1f);
  void footsteps(runtime::World& w, int slot);
  // A particle burst every rider sees (World::fx replicates it).
  void burst(runtime::World& w, const char* preset, float x, float y, int count = 0,
             float dir = 0.0f);
  bool rider_box(const runtime::World& w, int slot, runtime::Aabb* box) const;
  void place_safe(runtime::World& w, int slot);
  void act(runtime::World& w, int slot, std::uint32_t buttons);
  bool melee(runtime::World& w, int slot, const ItemDef* item);
  void mine(runtime::World& w, int slot, const ItemDef* item);
  void place(runtime::World& w, int slot);
  void fire(runtime::World& w, int slot, const ItemDef* item);
  void step_mobs(runtime::World& w);
  void spawn_near(runtime::World& w, int slot);
  void step_shots(runtime::World& w);
  void kill(runtime::World& w, std::size_t mob, int killer);
  bool walkable_px(float x, float y, float half) const;
  bool box_blocked(float x0, float y0, float x1, float y1) const;
  bool box_has_body(const runtime::World& w, float x0, float y0, float x1, float y1) const;
  void settle(runtime::World& w, int slot, int tx, int ty);
  std::uint32_t rand();
  float randf() { return static_cast<float>(rand() % 10000u) / 10000.0f; }

  // Server side net.
  void post(runtime::World& w, int slot, std::vector<std::uint8_t> bytes);
  void send_welcome(runtime::World& w, int slot);
  void send_inventory(runtime::World& w, int slot);
  void sync_chunks(runtime::World& w, int slot);
  // Client side net.
  void apply(const std::vector<std::uint8_t>& bytes);
  void request(runtime::World& w, std::vector<std::uint8_t> bytes);

  bool load_save(std::string* error);
  bool write_save() const;

  GameData data_;
  ChunkMap chunks_;
  std::array<Player, kSlots> players_{};
  std::array<std::pair<float, float>, kSlots> spawn_{};
  std::vector<Mob> mobs_;
  std::vector<Shot> shots_;
  std::vector<std::pair<int, int>> homes_;  // beds with a settler
  std::vector<std::string> warnings_;
  std::string project_dir_;
  std::string save_dir_;
  double clock_ = 0.0;
  double day_len_ = 480.0;
  double autosave_at_ = 60.0;
  std::uint64_t ticks_ = 0;
  std::uint64_t rng_ = 0x9e3779b97f4a7c15ull;
  std::uint32_t next_mob_ = 1;
  std::uint64_t kills_ = 0;
  std::uint64_t spawned_total_ = 0;
  std::uint64_t edits_sent_ = 0;
  std::uint64_t edits_received_ = 0;
  int my_slot_ = 0;
  bool active_ = false;
  bool mirror_ = false;
  bool seeded_ = false;
  bool saving_ = false;
  bool data_mismatch_ = false;
};

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
