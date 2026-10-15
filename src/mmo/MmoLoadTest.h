#pragma once

// Town Hall load test (ts_mmo): a swarm of simulated riders. Each bot is a
// real MmoClient on its own ENet socket: it creates (or logs into) its
// account, wanders the zone, talks on say / zone now and then, picks up
// what it passes, and a share of them drop the line once and resume.
//
//   ts_server --mmo-loadtest 200 [--connect host:port] [--seconds 30]
//   ts_admin  --smoke-mmo (a short run against an in-process server)
//   the editor's Town Hall panel (bots against the local server)

#include "mmo/MmoClient.h"
#include "mmo/MmoServer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mmo {

struct BotOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = kMmoDefaultPort;
  std::string prefix = "bot";
  std::string password = "ride-or-die";
  double chat_every = 4.0;      // seconds between a bot's lines (average)
  double drop_fraction = 0.1;   // share of bots that drop the line once
  double drop_after = 2.0;      // seconds in the world before dropping
  std::uint32_t seed = 7;
};

class BotSwarm {
 public:
  explicit BotSwarm(BotOptions opt = {});
  ~BotSwarm();
  // Start n more bots (staggered: a few per update).
  void add(int n);
  void update(double dt);
  void quit_all();
  int size() const { return static_cast<int>(bots_.size()); }
  int in_world() const;
  int failed() const;
  std::uint64_t resumes() const;
  std::uint64_t chat_heard() const;
  std::uint64_t corrections() const;
  double avg_visible() const;
  int max_visible() const;
  std::uint64_t bytes_in() const;
  std::vector<std::string> failures() const;
  const MmoClient* bot(int i) const { return bots_[static_cast<std::size_t>(i)]->client.get(); }

 private:
  struct Bot {
    std::unique_ptr<MmoClient> client;
    std::string user;
    bool started = false;
    bool will_drop = false;
    bool dropped = false;
    double drop_at = 0.0;
    double in_world_for = 0.0;
    double next_chat = 0.0;
    float tx = 0.0f, ty = 0.0f;
    std::uint64_t heard = 0;
  };
  BotOptions opt_;
  std::vector<std::unique_ptr<Bot>> bots_;
  std::mt19937 rng_;
  int next_id_ = 1;
};

struct LoadTestOptions {
  int bots = 64;
  double seconds = 6.0;
  // Empty: run an in-process server on project_dir (a sample world is
  // written there when it has no mmo.json) with database db_path.
  std::string connect;
  std::string project_dir;
  std::string db_path;
  std::uint32_t argon2_kib = 64;  // in-process only: cheap hashes for bots
  BotOptions bot;
  std::function<void(const std::string&)> progress;
};

struct LoadTestResult {
  bool ok = false;
  std::string failure;
  int bots = 0;
  int in_world = 0;
  int peak_in_world = 0;
  std::uint64_t resumes = 0;
  std::uint64_t chat_heard = 0;
  std::uint64_t corrections = 0;
  double avg_visible = 0.0;
  int max_visible = 0;
  int world_entities = 0;  // in-process only
  double tick_ms_avg = 0.0, tick_ms_max = 0.0;
  double kbps_out = 0.0;
  std::uint64_t cheat_kicks = 0;
  std::string summary;
};

LoadTestResult run_load_test(const LoadTestOptions& opt);

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
