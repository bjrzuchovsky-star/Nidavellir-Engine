#pragma once

// Request / reply over a team session, callable from any thread
// (docs/team-claims.md, 4). The editor's file worker asks the session host
// for blobs ("blob_has" / "blob_put" / "blob_get") through this; the
// TeamClient's poll() (main thread) puts the requests on the wire and hands
// the replies back. A closed session wakes every waiter with an error.

#include "scene/JsonTree.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace team {

class TeamRpc {
 public:
  // Any thread: send `req` (its "t" set by the caller), wait for the reply.
  // False (and *error) on timeout, a closed session or {"ok": false}.
  bool call(json::Value req, json::Value* reply, double timeout_s, std::string* error);

  // Main thread (TeamClient).
  void open();
  void close(const std::string& why);
  bool is_open() const;
  std::vector<json::Value> take_outgoing();
  void deliver(const json::Value& reply);  // matched by "rid"

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool open_ = false;
  std::string why_ = "Not riding with a posse.";
  std::uint64_t next_rid_ = 1;
  std::uint64_t epoch_ = 0;  // bumped by close(): stale waiters give up
  std::deque<json::Value> out_;
  std::map<std::uint64_t, json::Value> replies_;
};

}  // namespace team
}  // namespace tombstone
}  // namespace ts
