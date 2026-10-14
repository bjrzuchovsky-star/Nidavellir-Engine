#pragma once

// Headless multiplayer check shared by ts_server --smoke, ts_client --smoke
// and ts_admin --smoke: a loopback GameServer plus 2-4 GameClients behind a
// lag simulator ride for a while, then must agree with the server to the
// hundredth of a pixel (prediction + reconciliation for each rider's own
// horse, interpolation for everybody else's); a toast and a spawned /
// destroyed entity must reach every client; a fifth rider is turned away;
// a rider leaving frees the seat and everybody hears about it.

#include "net/SimTransport.h"

#include <functional>
#include <string>

namespace ts {
namespace tombstone {
namespace mp {

class GameClient;

struct PosseSmokeOptions {
  int riders = 4;
  net::LinkSim sim{40, 10, 0.05f};
  int ride_ticks = 180;   // bots ride this long, then everybody reins in
  double settle_seconds = 4.0;
  std::string project_dir;  // "" = write the posse test project to a temp dir
  // Called once all riders have settled (ts_client checks its draw list).
  std::function<std::string(const GameClient& client)> inspect;
};

struct PosseSmokeResult {
  bool ok = false;
  std::string failure;
  std::string summary;  // one line for the [smoke] report
};

PosseSmokeResult run_posse_smoke(const PosseSmokeOptions& options);

// A small four-rider project: a rider, four SpawnPoints, a wall, a bell that
// toasts every second and rolls a spawned tumbleweed across town.
bool write_posse_test_project(const std::string& dir, std::string* error_out = nullptr);

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
