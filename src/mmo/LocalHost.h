#pragma once

// The editor's Town Hall host (ts_mmo): an MmoServer and an optional bot
// swarm on their own thread, so the persistent world keeps time while the
// editor draws. Everything the panel reads goes through with(), under the
// host's lock.

#include "mmo/MmoLoadTest.h"
#include "mmo/MmoServer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ts {
namespace tombstone {
namespace mmo {

class LocalHost {
 public:
  LocalHost() = default;
  ~LocalHost();
  LocalHost(const LocalHost&) = delete;
  LocalHost& operator=(const LocalHost&) = delete;

  bool start(MmoServerConfig cfg, std::string* error);
  void stop();
  bool running() const { return running_; }
  // Bots against the local server (0 = none). Extra bots join, fewer quit.
  void set_bots(int n);
  int bots() const { return want_bots_; }
  void with(const std::function<void(MmoServer& server, BotSwarm* bots)>& fn);
  std::string admin(const std::string& line);

 private:
  void run();
  std::mutex mu_;
  std::unique_ptr<MmoServer> server_;
  std::unique_ptr<BotSwarm> swarm_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> quit_{false};
  int want_bots_ = 0;
  int swarm_gen_ = 0;
};

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
