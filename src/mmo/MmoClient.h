#pragma once

// Town Hall client (ts_mmo): headless, owned by one thread. Logs in (or
// creates an account), sends intent every client tick with client-side
// prediction (step_rider on the zone geometry the server sent; replay of
// unacknowledged moves on every snapshot), keeps what interest management
// lets it see, chat, inventory, and resumes the session by itself when the
// line drops (the server keeps the rider link-dead meanwhile).
//
// Game hosts draw from entities() / position(); bots (MmoLoadTest) and the
// smokes drive it directly.

#include "mmo/MmoProtocol.h"
#include "net/Transport.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace mmo {

struct ClientEntity {
  std::uint64_t id = 0;
  EntityKind kind = EntityKind::Actor;
  std::string name;
  float x = 0.0f, y = 0.0f;
};

struct ClientStats {
  std::uint64_t snapshots = 0;
  std::uint64_t corrections = 0;
  std::uint64_t appears = 0;
  std::uint64_t vanishes = 0;
  std::uint64_t resumes = 0;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint32_t rtt_ms = 0;
};

class MmoClient {
 public:
  enum class State { Idle, Connecting, LoggingIn, InWorld, Reconnecting, Failed, Closed };

  MmoClient();
  ~MmoClient();
  MmoClient(const MmoClient&) = delete;
  MmoClient& operator=(const MmoClient&) = delete;

  // Testing: transport instead of ENet (made fresh per connection).
  void set_transport_factory(std::function<std::unique_ptr<net::Transport>()> make) { make_ = std::move(make); }

  bool login(const std::string& host, std::uint16_t port, const std::string& user, const std::string& password);
  bool create(const std::string& host, std::uint16_t port, const std::string& user, const std::string& password,
              const std::string& character);
  bool resume(const std::string& host, std::uint16_t port, const std::string& token);
  // Polite goodbye (the server saves and forgets the rider at once).
  void quit();
  // Testing: lose the connection without a word (the server sees a drop).
  void drop_line();

  // Service the network and run client ticks for real elapsed seconds.
  void update(double real_seconds);

  void set_move(float mx, float my) { mx_ = mx; my_ = my; }
  void say(const std::string& channel, const std::string& text);
  void interact(std::uint64_t entity);
  // Testing (anti-cheat smoke): the claim sent with the next move is off by
  // (dx, dy), and extra moves per tick flood the server.
  void cheat_offset(float dx, float dy) { cheat_dx_ = dx; cheat_dy_ = dy; }
  void cheat_flood(int extra_moves_per_tick) { flood_ = extra_moves_per_tick; }

  State state() const { return state_; }
  bool in_world() const { return state_ == State::InWorld; }
  const std::string& error() const { return error_; }
  std::uint32_t disconnect_reason() const { return reason_; }
  const LoginOkMsg& session() const { return session_; }
  const std::string& token() const { return session_.token; }
  const ZoneGeometry& zone() const { return zone_; }
  float x() const { return x_; }
  float y() const { return y_; }
  const std::unordered_map<std::uint64_t, ClientEntity>& entities() const { return ents_; }
  const std::map<std::string, int>& inventory() const { return inventory_; }
  std::deque<ChatMsg>& inbox() { return inbox_; }
  const ClientStats& stats() const { return stats_; }
  void set_auto_resume(bool on) { auto_resume_ = on; }

 private:
  bool open(const std::string& host, std::uint16_t port, LoginMsg msg);
  void handle(const net::TransportEvent& ev);
  void client_tick(double dt);
  void send(net::Delivery d, const std::vector<std::uint8_t>& msg);
  double now_ms() const;

  std::function<std::unique_ptr<net::Transport>()> make_;
  std::unique_ptr<net::Transport> transport_;
  net::PeerId server_ = net::kNoPeer;
  State state_ = State::Idle;
  std::string host_;
  std::uint16_t port_ = 0;
  LoginMsg pending_login_;
  std::string error_;
  std::uint32_t reason_ = 0;
  LoginOkMsg session_;
  ZoneGeometry zone_;
  float x_ = 0.0f, y_ = 0.0f;
  float mx_ = 0.0f, my_ = 0.0f;
  std::uint32_t seq_ = 0;
  std::uint64_t last_snap_tick_ = 0;
  std::deque<MoveCmd> unacked_;
  double accumulator_ = 0.0;
  double clock_ = 0.0;
  double reconnect_at_ = 0.0;
  int reconnect_tries_ = 0;
  bool auto_resume_ = true;
  bool quitting_ = false;
  float cheat_dx_ = 0.0f, cheat_dy_ = 0.0f;
  int flood_ = 0;
  std::unordered_map<std::uint64_t, ClientEntity> ents_;
  std::map<std::string, int> inventory_;
  std::deque<ChatMsg> inbox_;
  ClientStats stats_;
};

const char* to_string(MmoClient::State s);

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
