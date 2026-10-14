#pragma once

// A TombStone multiplayer client. Connects to a GameServer, receives the
// server's scene in the welcome, and keeps a runtime::World it never
// simulates: snapshots pose it. Its own rider is predicted (input applied
// locally the tick it is pressed, replayed on top of every authoritative
// snapshot, small corrections smoothed away); the other riders and every
// other entity are interpolated between snapshots a little in the past.
//
// No window: ts_client draws world() with ts_gfx, the editor's posse and
// the smoke test drive it headless.

#include "mp/Protocol.h"
#include "net/SimTransport.h"
#include "net/Transport.h"
#include "runtime/World.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mp {

struct ClientConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 24642;
  std::string name = "Stranger";
  // Optional local copy of the project: sprites and sheets load from here
  // (without it the riders draw as solid stand-ins).
  std::string project_dir;
  net::LinkSim sim;          // lag on this client's link (both ways)
  double interp_ms = 100.0;  // how far behind remote entities are drawn
  // Testing: transport to use instead of ENet (still wrapped in the sim).
  std::unique_ptr<net::Transport> transport;
};

enum class ClientState { Idle, Connecting, Joined, Rejected, Disconnected };
const char* to_string(ClientState state);

struct ClientStats {
  double rtt_ms = 0.0;               // smoothed, input -> snapshot echo
  std::uint64_t snapshots = 0;       // received and used
  std::uint64_t stale_snapshots = 0; // arrived after a newer one
  double snapshot_hz = 0.0;
  double kbps_in = 0.0;
  double kbps_out = 0.0;
  std::uint64_t corrections = 0;     // reconciliations that moved us
  float last_correction_px = 0.0f;
  float max_correction_px = 0.0f;
  std::size_t pending_inputs = 0;    // sent, not yet acknowledged
  double interp_ms = 0.0;
  std::uint64_t server_tick = 0;     // newest snapshot
};

struct ClientToast {
  int slot = -1;
  std::string text;
  double until = 0.0;  // client clock seconds
};

class GameClient {
 public:
  GameClient();
  ~GameClient();
  GameClient(const GameClient&) = delete;
  GameClient& operator=(const GameClient&) = delete;

  bool connect(ClientConfig config, std::string* error_out = nullptr);
  // Ride off politely (kReasonQuit) and close.
  void disconnect();
  // Drop the link with no goodbye (tests: the server must time us out).
  void vanish();

  // Real elapsed seconds: services the network and runs 0..8 client ticks
  // with `input` (sent to the server and predicted locally).
  int update(double real_seconds, const runtime::PlayerInput& input);
  // One client tick now.
  void tick(const runtime::PlayerInput& input);
  void poll();
  // Pose world() for drawing right now: remote entities interpolated,
  // own rider predicted. update() / tick() already call it.
  void pose();

  ClientState state() const { return state_; }
  bool joined() const { return state_ == ClientState::Joined; }
  bool has_snapshot() const { return !snaps_.empty(); }
  const std::string& status() const { return status_; }  // reject / leave reason
  int slot() const { return slot_; }
  std::uint64_t rider() const { return rider_; }
  const std::string& name() const { return config_.name; }
  const std::string& project_name() const { return project_name_; }

  const runtime::World& world() const { return world_; }
  runtime::World& world() { return world_; }
  // Own rider: predicted position (no smoothing) and the newest
  // authoritative one.
  float predicted_x() const { return pred_x_; }
  float predicted_y() const { return pred_y_; }
  bool server_rider(float* x, float* y) const;
  // Where an entity sits in the newest snapshot (false = not in it).
  bool latest(std::uint64_t id, float* x, float* y) const;
  // Where the world is drawn now (after pose()).
  bool drawn(std::uint64_t id, float* x, float* y) const;

  const RosterMsg& roster() const { return roster_; }
  const ClientStats& stats() const { return stats_; }
  // Toasts for this rider's HUD, oldest first (expired ones dropped).
  std::vector<ClientToast> toasts() const;
  // Join / leave / Telegraph lines since the last take.
  std::vector<std::string> take_events();
  double clock() const { return clock_; }

  void set_link_sim(const net::LinkSim& sim);
  net::LinkSim link_sim() const;

 private:
  struct Snap {
    std::uint64_t tick = 0;
    std::vector<runtime::NetActor> actors;
  };

  void handle(const net::TransportEvent& ev);
  void on_welcome(const WelcomeMsg& w);
  void on_snapshot(SnapshotMsg&& s);
  void reconcile(const Snap& s, std::uint32_t ack);
  void send_input();
  void measure(double real_seconds);
  std::uint32_t now_ms() const;
  const runtime::NetActor* row(const Snap& s, std::uint64_t id) const;
  void event(std::string line);

  ClientConfig config_;
  std::unique_ptr<net::Transport> transport_;
  net::SimTransport* sim_ = nullptr;
  net::PeerId server_ = net::kNoPeer;
  ClientState state_ = ClientState::Idle;
  std::string status_;
  std::string project_name_;
  runtime::World world_;
  bool world_ready_ = false;
  int slot_ = -1;
  std::uint64_t rider_ = 0;

  double clock_ = 0.0;       // seconds since connect
  double accumulator_ = 0.0;
  std::uint32_t seq_ = 0;
  std::deque<InputCmd> unacked_;
  runtime::PlayerInput last_input_;
  std::deque<Snap> snaps_;   // ascending tick
  std::uint32_t acked_ = 0;
  float pred_x_ = 0.0f;
  float pred_y_ = 0.0f;
  float err_x_ = 0.0f;       // visual smoothing of corrections
  float err_y_ = 0.0f;
  bool have_pred_ = false;
  double tick_offset_ = 0.0; // server tick - client clock ticks
  bool have_offset_ = false;

  RosterMsg roster_;
  std::vector<ClientToast> toasts_;
  std::vector<std::string> events_;
  ClientStats stats_;
  double measure_window_ = 0.0;
  std::uint64_t measure_snaps_ = 0;
  std::uint64_t bytes_in_mark_ = 0;
  std::uint64_t bytes_out_mark_ = 0;
  double start_wall_ = 0.0;
};

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
