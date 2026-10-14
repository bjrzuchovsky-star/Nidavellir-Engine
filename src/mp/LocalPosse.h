#pragma once

// A whole posse in one process: a GameServer on a loopback port plus one to
// four GameClients, each behind its own link simulator. The editor's "Play
// with posse" rides it (P1 on the keyboard, the rest on simple bot reins),
// and the multiplayer smoke test drives it headless.

#include "mp/GameClient.h"
#include "mp/GameServer.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mp {

struct PosseConfig {
  std::string project_dir;
  std::optional<scene_json::SceneDoc> scene;  // unsaved editor scene
  std::string project_name;
  int riders = 2;           // 1..4 in-process clients
  std::uint16_t port = 0;   // 0 = any free port
  net::LinkSim sim;         // applied to every client link
  int snapshot_every = 2;
  double interp_ms = 100.0;
};

// Wandering reins for bot riders: a slow square, a pause, repeat (each
// rider out of step with the others). Deterministic in the tick.
runtime::PlayerInput bot_input(int rider_index, std::uint64_t tick);

class LocalPosse {
 public:
  bool start(PosseConfig config, std::string* error_out = nullptr);
  void stop();
  bool running() const { return server_.running(); }

  // Real elapsed seconds. Rider 0 gets `p1`; the others ride bot_input()
  // unless bots are off (then they sit still).
  void update(double real_seconds, const runtime::PlayerInput& p1);

  // Saddle up one more in-process rider / send the newest one home.
  bool add_rider(std::string* error_out = nullptr);
  bool remove_rider();

  void set_link_sim(const net::LinkSim& sim);
  net::LinkSim link_sim() const { return config_.sim; }
  void set_bots(bool on) { bots_ = on; }
  bool bots() const { return bots_; }

  GameServer& server() { return server_; }
  const GameServer& server() const { return server_; }
  const std::vector<std::unique_ptr<GameClient>>& clients() const { return clients_; }
  std::uint16_t port() const { return server_.port(); }
  std::uint64_t bot_tick() const { return bot_tick_; }

 private:
  PosseConfig config_;
  GameServer server_;
  std::vector<std::unique_ptr<GameClient>> clients_;
  bool bots_ = true;
  std::uint64_t bot_tick_ = 0;
  double bot_acc_ = 0.0;
  int names_used_ = 0;
};

// Frontier names for posse riders ("Ike", "Doc", "Belle", "Kid").
const char* posse_name(int index);

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
