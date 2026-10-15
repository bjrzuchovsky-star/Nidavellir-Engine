#pragma once

// MmoServer's private state (shared by MmoServer.cpp and MmoBridge.cpp).

#include "mmo/MmoServer.h"
#include "runtime/World.h"

namespace ts {
namespace tombstone {
namespace mmo {

struct MmoServer::Character {
  std::int64_t id = 0;
  std::int64_t account = 0;
  std::string account_name;
  std::uint64_t entity = 0;
  std::string name;
  std::string zone;
  float x = 0.0f, y = 0.0f;
  bool admin = false;
  std::int64_t muted_until = 0;
  std::map<std::string, int> inventory;
  story::Vars vars;
  story::QuestLog quests;
  double playtime = 0.0;
  // connection
  net::PeerId peer = net::kNoPeer;
  double linkdead_since = -1.0;
  std::string token_digest;
  // movement
  std::uint32_t ack = 0;
  std::map<std::uint32_t, MoveCmd> pending;
  float budget = 3.0f;  // moves allowed now (token bucket, tick_rate / s)
  bool have_claim = false;
  float claim_x = 0.0f, claim_y = 0.0f;
  std::uint32_t claim_seq = 0;
  bool corrected = false;
  float strikes = 0.0f;
  std::uint64_t corrections = 0;
  std::uint64_t dropped = 0;
  std::uint32_t echo_ms = 0;
  std::uint16_t rtt_ms = 0;
  // interest: id -> last sent position
  std::unordered_map<std::uint64_t, std::pair<float, float>> known;
  double full_refresh = 0.0;
  bool zone_sent = false;
  // chat
  float chat_tokens = 8.0f;
  std::set<std::string> channels;
  bool dirty = false;
  std::uint32_t kick = 0;  // set by strike(); acted on after the tick's loops
};

struct MmoServer::ZoneRt {
  const ZoneDef* def = nullptr;
  std::unique_ptr<runtime::World> world;
  std::unordered_map<std::uint64_t, std::string> actor_names;
  struct Item {
    ItemSpawn spawn;
    double back_at = 0.0;  // available when now >= back_at
  };
  std::vector<Item> items;
  struct Ent {
    EntityKind kind;
    std::string name;
    float x, y;
  };
  std::unordered_map<std::uint64_t, Ent> ents;  // this tick
  InterestGrid grid;
  int riders = 0;
};

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
