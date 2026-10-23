#pragma once

// Live Team Claims session host (docs/team-claims.md, section 3).
//
// One TeamServer holds any number of sessions, one per claim. It is
// single-threaded: call poll() from one thread (ts_vault runs it on its own
// thread, the editor's "Host" calls it every frame). Wire: net::Transport
// (ENet), one compact JSON object per message. Reliable channel for
// hello / ops / locks, unreliable for presence.

#include "net/Transport.h"
#include "team/TeamDoc.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace team {

struct ServerOptions {
  std::uint16_t port = 0;          // UDP; 0 = any free port (see port())
  std::size_t max_peers = 64;
  // Only this claim may open a session ("" = any; ts_vault hosts many).
  std::string only_claim;
  // Auth, checked in this order: a passcode (LAN host), else the key
  // callback (ts_vault: VaultStore::check_key), else everyone may join.
  std::string passcode;
  std::function<bool(const std::string& key, std::string* name, bool* can_write)> check_key;
  std::string engine = "0.0.0";    // this build's version (informational)
  double lock_timeout_s = 15.0;    // silent members lose their locks
  // A lock nobody has used (edited, dragged) for this long may be taken
  // over by another member ("take"); 0 = never.
  double lock_idle_s = 15.0;
  double member_timeout_s = 30.0;  // and are dropped after this
  std::size_t compact_every = 256; // commits kept after the snapshot
  std::uint64_t id_block = 1ull << 20;
  std::function<void(const std::string&)> log;
  // Persistent sessions: each claim's session (docs, seq, id grants,
  // riders' rejoin tokens) is written to <state_dir>/<claim>.team.json a
  // moment after it changes and on stop(), and loaded again by start().
  // "" = memory only. ts_vault: <vault root>/team; editor host:
  // <project>/.tombstone/team/host.
  std::string state_dir;
  double save_delay_s = 1.0;
  // Tests: wrap the transport (net::SimTransport for latency).
  std::function<std::unique_ptr<net::Transport>(std::unique_ptr<net::Transport>)> wrap_transport;
};

struct MemberInfo {
  std::uint32_t id = 0;
  std::string name;
  std::string colour;   // "#rrggbb"
  bool can_write = true;
  std::string scene;
  Value presence;       // last "pres" (cursor, sel, cam)
};

struct SessionInfo {
  std::string claim;
  std::string claim_name;
  std::string engine;
  int scene2d = 0;
  int scene3d = 0;
  std::string dim;
  std::uint64_t seq = 0;
  std::uint64_t snap_seq = 0;
  std::vector<MemberInfo> members;
  std::map<std::string, std::uint32_t> locks;
};

class TeamServer {
 public:
  TeamServer();
  ~TeamServer();
  TeamServer(const TeamServer&) = delete;
  TeamServer& operator=(const TeamServer&) = delete;

  bool start(const ServerOptions& opt, std::string* error);
  void stop();
  bool running() const { return transport_ != nullptr; }
  std::uint16_t port() const;
  // Service the socket: handle everything that arrived, expire locks.
  void poll();

  std::vector<SessionInfo> sessions() const;
  // The authoritative docs of a claim's session (nullptr when none).
  const Docs* docs(const std::string& claim) const;
  // JSON for GET /api/v1/team (no docs, no keys).
  Value info_json() const;
  std::uint64_t commits() const { return total_commits_; }
  std::uint64_t saves() const { return total_saves_; }
  // Write every changed session now (stop() does this too).
  void save_all();
  std::uint64_t rejects() const { return total_rejects_; }

  // Palette the members are coloured from (Dusk).
  static const std::vector<std::string>& palette();

 private:
  struct Member {
    std::uint32_t id = 0;
    net::PeerId peer = net::kNoPeer;
    std::string claim;
    std::string name;
    std::string colour;
    bool can_write = true;
    bool joined = false;
    std::string scene;
    Value presence;
    double last_seen = 0.0;
  };
  struct Session {
    std::string claim, claim_name, engine, dim;
    int scene2d = 0, scene3d = 0;
    Docs docs;            // current
    Docs snap_docs;       // at snap_seq
    std::uint64_t snap_seq = 0;
    std::vector<Batch> tail;  // commits after snap_seq
    std::uint64_t seq = 0;
    std::map<std::string, std::uint32_t> locks;  // key -> member id
    std::map<std::string, double> lock_used;     // key -> last grant / edit / drag
    std::uint64_t next_grant = 0;
    int colour_next = 0;
    // Riders who may come back as themselves: token -> who they were.
    struct Rider {
      std::uint32_t id = 0;
      std::string name, colour;
      double last = 0.0;  // ordering only (most recent kept)
    };
    std::map<std::string, Rider> riders;
    bool dirty = false;
    double dirty_at = 0.0;
  };
  void mark_dirty(Session& s);
  void save_session(Session& s);
  void load_sessions();
  std::string new_token();

  double now() const;
  void log(const std::string& s) const;
  void send(net::PeerId peer, const Value& msg, bool reliable = true);
  void broadcast(const Session& s, const Value& msg, std::uint32_t except = 0,
                 bool reliable = true);
  void on_message(Member& m, const Value& msg, bool reliable);
  void on_hello(Member& m, const Value& msg);
  void on_op(Member& m, const Value& msg);
  void on_lock(Member& m, const Value& msg);
  void on_take(Member& m, const Value& msg);
  void touch_lock(Session& s, const std::string& key) { s.lock_used[key] = now(); }
  void on_presence(Member& m, const Value& msg);
  void on_preview(Member& m, const Value& msg, bool end);
  void on_gone(net::PeerId peer);
  void release_locks(Session& s, std::uint32_t member, bool notify);
  void send_locks(const Session& s);
  void refuse(Member& m, const std::string& reason);
  Value member_json(const Member& m) const;
  Member* member_by_peer(net::PeerId peer);

  ServerOptions opt_;
  std::unique_ptr<net::Transport> transport_;
  std::map<net::PeerId, Member> members_;
  std::map<std::string, Session> sessions_;
  std::uint32_t next_member_ = 1;
  std::uint64_t total_commits_ = 0;
  std::uint64_t total_rejects_ = 0;
  std::uint64_t total_saves_ = 0;
  std::uint64_t token_salt_ = 0;
  std::chrono::steady_clock::time_point t0_;
};

// One message on the wire and back.
std::vector<std::uint8_t> encode(const Value& msg);
bool decode(const std::vector<std::uint8_t>& data, Value* out);

}  // namespace team
}  // namespace tombstone
}  // namespace ts
