#pragma once

// Live Team Claims client (docs/team-claims.md, 3.3 - 3.6). Headless: the
// editor's team binding (src/editor/team) feeds it the op batches the undo
// system commits and applies what it hands back. Single-threaded: poll()
// from the editor's frame.

#include "net/Transport.h"
#include "team/TeamDoc.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace team {

struct JoinOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string claim;       // project id
  std::string claim_name;  // shown to the others
  std::string user;        // display name ("" = the API key's name)
  std::string key;         // Vault API key (ts_vault sessions)
  std::string pass;        // passcode (editor-hosted LAN sessions)
  std::string engine = "0.0.0";
  int scene2d = 0;         // scene formats this editor writes
  int scene3d = 0;
  std::string dim;         // "2d" / "3d" / "vr"
  std::string scene;       // the scene this editor has open
  Docs seed;               // used only if the session is new
  std::string rejoin;      // token from an earlier welcome: ride back in as yourself
  std::function<std::unique_ptr<net::Transport>(std::unique_ptr<net::Transport>)> wrap_transport;
};

struct RemoteMember {
  std::uint32_t id = 0;
  std::string name;
  std::string colour;
  bool can_write = true;
  std::string scene;
  Value presence;  // {"scene", "space", "cursor": [x,y,z], "sel": [...]}
  bool has_presence = false;
};

// Mid-drag preview from another member (docs/team-claims.md, 3.7):
// transient entity puts while they drag, then an end when they let go.
struct PreviewEvent {
  std::uint32_t member = 0;
  std::string scene;
  std::uint64_t n = 0;               // the sender's preview counter
  bool end = false;                  // drag over: drop the preview
  std::vector<Op> ops;               // puts (preview)
  std::vector<std::uint64_t> ids;    // entities (end)
};

class TeamClient {
 public:
  enum class State { Idle, Connecting, Joined, Refused, Closed };

  TeamClient();
  ~TeamClient();
  TeamClient(const TeamClient&) = delete;
  TeamClient& operator=(const TeamClient&) = delete;

  bool join(const JoinOptions& opt, std::string* error);
  void leave();     // graceful goodbye
  void drop_now();  // vanish without a goodbye (tests: a crashed editor)
  void poll();

  State state() const { return state_; }
  bool joined() const { return state_ == State::Joined; }
  const std::string& error() const { return error_; }
  const std::string& warning() const { return warn_; }
  std::uint32_t me() const { return me_; }
  const std::string& my_name() const { return my_name_; }
  const std::string& my_colour() const { return my_colour_; }
  bool can_write() const { return can_write_; }
  std::uint64_t id_lo() const { return id_lo_; }
  std::uint64_t id_hi() const { return id_hi_; }
  const JoinOptions& options() const { return opt_; }
  const Value& session() const { return session_; }
  // From the welcome: hand back as JoinOptions::rejoin next time.
  const std::string& rejoin_token() const { return rejoin_token_; }
  bool rode_back() const { return rode_back_; }  // the server knew our token

  // Server state up to seq() (what everyone converges to).
  const Docs& confirmed() const { return confirmed_; }
  std::uint64_t seq() const { return seq_; }
  // Once after joining: the docs the editor should load (session state).
  bool take_welcome(Docs* out);

  // A local undo step's ops (already applied in the editor). Returns cid.
  std::uint64_t submit(const std::string& scene, const std::string& label, std::vector<Op> ops);
  // Ops the editor must apply now (remote edits, rejected local edits
  // rolled back), in order.
  std::vector<std::pair<std::string, Op>> take_apply();
  bool idle() const { return pending_.empty(); }
  std::uint64_t bytes_sent() const { return bytes_sent_; }  // everything we sent (encoded)
  std::size_t pending() const { return pending_.size(); }

  // Locks: the full set this editor wants (selection / drag / Inspector).
  void want_locks(std::vector<std::string> keys);
  std::uint32_t lock_owner(const std::string& key) const;
  // Take over locks someone else has left idle (15 s by default). The
  // server says no (a note) while the holder is still using them.
  void take_locks(const std::vector<std::string>& keys);
  const std::map<std::string, std::uint32_t>& locks() const { return locks_; }
  // Unreliable presence ({"scene", "space", "cursor", "sel"}).
  void send_presence(Value presence);
  // Mid-drag preview: puts of entities this editor holds while a drag or
  // Inspector edit is open (unreliable, ~20 Hz), then the end (reliable).
  // Returns the preview number used.
  std::uint64_t send_preview(const std::string& scene, const std::vector<Op>& puts);
  void end_preview(const std::string& scene, const std::vector<std::uint64_t>& ids);
  std::vector<PreviewEvent> take_previews();

  const std::map<std::uint32_t, RemoteMember>& members() const { return members_; }
  const RemoteMember* member(std::uint32_t id) const;
  // Things worth a toast ("Mia rode in", "Edit undone: locked by Mia").
  std::vector<std::string> take_notes();

  std::uint64_t commits_seen() const { return commits_seen_; }
  std::uint64_t rejected() const { return rejected_; }

 private:
  void send(const Value& msg, bool reliable = true);
  void on_message(const Value& msg);
  void on_commit(const Batch& b);
  void on_reject(std::uint64_t cid, const std::string& reason);
  double now() const;

  JoinOptions opt_;
  std::unique_ptr<net::Transport> transport_;
  net::PeerId server_ = net::kNoPeer;
  State state_ = State::Idle;
  std::string error_, warn_;
  std::uint32_t me_ = 0;
  std::string my_name_, my_colour_;
  bool can_write_ = true;
  std::uint64_t id_lo_ = 0, id_hi_ = 0;
  Value session_;
  std::string rejoin_token_;
  bool rode_back_ = false;
  Docs confirmed_;
  std::uint64_t seq_ = 0;
  bool welcome_pending_ = false;
  std::deque<Batch> pending_;
  std::uint64_t next_cid_ = 1;
  std::vector<std::pair<std::string, Op>> apply_;
  std::vector<std::string> want_;
  bool want_sent_ = false;
  std::uint64_t bytes_sent_ = 0;
  std::map<std::string, std::uint32_t> locks_;
  std::map<std::uint32_t, RemoteMember> members_;
  std::vector<std::string> notes_;
  std::vector<PreviewEvent> previews_;
  std::uint64_t preview_n_ = 0;
  std::uint64_t commits_seen_ = 0, rejected_ = 0;
  double connect_started_ = 0.0;
  std::chrono::steady_clock::time_point t0_;
};

}  // namespace team
}  // namespace tombstone
}  // namespace ts
