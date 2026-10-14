#pragma once

// The authoritative TombStone server. Owns a runtime::World built from a
// project, runs it at the fixed 60 Hz tick, seats up to four riders (one
// player entity each, cloned from the scene's rider when the scene has
// fewer), applies their input, and sends every client a snapshot of the
// world (unreliable, every snapshot_every ticks) plus reliable events:
// riders joining and leaving, script toasts and Telegraph lines.
//
// Headless and single-threaded: whoever owns it calls update() (ts_server's
// loop, the editor's frame, the smoke test). No window, no GL.

#include "mp/Protocol.h"
#include "net/SimTransport.h"
#include "net/Transport.h"
#include "runtime/World.h"
#include "scene/SceneJson.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mp {

struct ServerConfig {
  std::string project_dir;
  std::uint16_t port = 24642;  // 0 = any free port (see GameServer::port())
  int max_players = kMaxPosse;
  int snapshot_every = 2;      // ticks between snapshots (2 = 30 Hz)
  net::LinkSim sim;            // server-side lag (testing)
  // Ride this scene instead of <project_dir>/scene.json (the editor's
  // unsaved workspace).
  std::optional<scene_json::SceneDoc> scene;
  std::string project_name;    // "" = folder name
  // Testing: transport to use instead of ENet (still wrapped in the sim).
  std::unique_ptr<net::Transport> transport;
};

// One seat at the table, as the Network panel shows it.
struct ServerRider {
  int slot = 0;
  std::string name;
  std::uint64_t rider = 0;     // player entity
  std::uint16_t rtt_ms = 0;    // as the client measured it
  std::uint32_t last_seq = 0;  // last input applied
  std::size_t queued = 0;      // inputs waiting (jitter buffer)
  std::uint64_t joined_tick = 0;
  net::LinkStats link;
  double kbps_out = 0.0;       // kilobytes / s to this rider
  double kbps_in = 0.0;
};

struct ServerStats {
  std::uint64_t ticks = 0;
  std::uint64_t snapshots_sent = 0;
  double snapshot_hz = 0.0;     // per rider, measured
  double kbps_out = 0.0;        // all riders
  double kbps_in = 0.0;
  std::size_t last_snapshot_bytes = 0;
  std::uint64_t joins = 0;
  std::uint64_t leaves = 0;
  std::uint64_t rejected = 0;
};

class GameServer {
 public:
  GameServer();
  ~GameServer();
  GameServer(const GameServer&) = delete;
  GameServer& operator=(const GameServer&) = delete;

  bool start(ServerConfig config, std::string* error_out = nullptr);
  // Tell every rider goodbye (kReasonShutdown) and close.
  void stop();
  bool running() const { return running_; }

  // Real elapsed seconds: services the network and runs 0..8 fixed ticks.
  int update(double real_seconds);
  // Exactly one tick now (network in, simulate, snapshot out).
  void tick_once();
  // Service the network only.
  void poll();

  std::uint16_t port() const;
  const runtime::World& world() const { return world_; }
  runtime::World& world() { return world_; }
  std::uint64_t tick() const { return world_.tick(); }
  const std::string& project_name() const { return project_name_; }

  std::vector<ServerRider> riders() const;
  int rider_count() const;
  const ServerStats& stats() const { return stats_; }

  // Lag on the server's own link (the posse puts it on the clients).
  void set_link_sim(const net::LinkSim& sim);
  net::LinkSim link_sim() const;

  // Human-readable happenings since the last take ("Ike rode in as P2").
  std::vector<std::string> take_log();
  // Telegraph lines from the world (scripts, toasts, triggers) since the
  // last take; they also go to every rider.
  std::vector<runtime::LogEntry> take_world_logs();

 private:
  struct Peer {
    net::PeerId id = net::kNoPeer;
    bool seated = false;
    int slot = -1;
    std::string name;
    std::uint64_t rider = 0;
    std::uint16_t rtt_ms = 0;
    std::uint32_t last_seq = 0;
    std::uint32_t echo_time_ms = 0;
    std::map<std::uint32_t, runtime::PlayerInput> pending;
    runtime::PlayerInput last;
    std::uint64_t joined_tick = 0;
    std::uint64_t bytes_out_mark = 0;
    std::uint64_t bytes_in_mark = 0;
    double kbps_out = 0.0;
    double kbps_in = 0.0;
  };

  void handle(const net::TransportEvent& ev);
  void on_hello(Peer& p, const HelloMsg& hello);
  void on_input(Peer& p, const InputMsg& in);
  void unseat(Peer& p, std::uint32_t reason);
  void seat_visible(int slot, std::uint64_t rider, bool on);
  runtime::InputFrame gather_input();
  void broadcast(net::Delivery delivery, const std::vector<std::uint8_t>& msg);
  void send_snapshots();
  void send_roster();
  void forward_world_output();
  void measure(double real_seconds);
  Peer* peer(net::PeerId id);
  void log(std::string line);


  ServerConfig config_;
  std::unique_ptr<net::Transport> transport_;
  net::SimTransport* sim_ = nullptr;  // owned by transport_
  runtime::World world_;
  std::string project_name_;
  std::string scene_json_;
  std::uint64_t scene_hash_ = 0;
  std::map<net::PeerId, Peer> peers_;
  std::array<std::uint64_t, kMaxPosse> slot_rider_{};
  bool running_ = false;
  double accumulator_ = 0.0;
  ServerStats stats_;
  std::vector<std::string> log_;
  std::vector<runtime::LogEntry> world_logs_;
  std::vector<std::string> toasts_sent_;
  std::vector<runtime::NetActor> rows_;
  double measure_window_ = 0.0;
  std::uint64_t measure_snapshots_ = 0;
};

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
