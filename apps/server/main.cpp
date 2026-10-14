// ts_server: the authoritative TombStone multiplayer server. Loads
// <project>/scene.json into a runtime::World, runs it at a fixed 60 Hz and
// seats up to four riders over UDP (ENet). Clients get snapshots
// (unreliable) and events (reliable: joins, leaves, toasts, Telegraph).
//
//   ts_server --project <dir> [--port N]     open for business
//   ts_server --smoke [--project <dir>]      headless loopback check

#include "mp/GameServer.h"
#include "mp/PosseSmoke.h"
#include "net/Net.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace ts::tombstone;
namespace fs = std::filesystem;

constexpr const char* kUsage =
    "Usage: ts_server --project <dir> [--port N] [--snapshot-every N]\n"
    "                 [--lag MS] [--jitter MS] [--loss PCT] [--seconds S]\n"
    "       ts_server --smoke [--project <dir>] [--riders N --lag MS --jitter MS --loss PCT]\n"
    "  --project <dir>       TombStone project folder (holds scene.json)\n"
    "  --port N              UDP port (default 24642, 0 = any free port)\n"
    "  --snapshot-every N    ticks between snapshots (default 2 = 30 Hz)\n"
    "  --lag / --jitter MS   simulated one-way latency / jitter on the server link\n"
    "  --loss PCT            simulated unreliable packet loss, 0-100\n"
    "  --seconds S           close after S seconds (testing)\n"
    "  --smoke               headless check: loopback server + 2 and 4 clients under\n"
    "                        simulated lag, convergence, toasts, spawns, join / leave\n"
    "  --riders N            with --smoke: one run of N riders on the --lag line\n";

std::atomic<bool> g_quit{false};

extern "C" void on_signal(int) { g_quit = true; }

struct Options {
  std::string project;
  int port = net::kDefaultPort;
  int snapshot_every = 2;
  net::LinkSim sim;
  double seconds = -1.0;
  bool smoke = false;
  int riders = 0;  // --smoke: one custom run with this many riders
  bool help = false;
};

bool parse_args(int argc, char** argv, Options* o, std::string* err) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--project" && has) {
      o->project = argv[++i];
    } else if (a == "--port" && has) {
      o->port = std::atoi(argv[++i]);
      if (o->port < 0 || o->port > 65535) {
        *err = "--port must be 0..65535";
        return false;
      }
    } else if (a == "--snapshot-every" && has) {
      o->snapshot_every = std::atoi(argv[++i]);
    } else if (a == "--lag" && has) {
      o->sim.latency_ms = std::atoi(argv[++i]);
    } else if (a == "--jitter" && has) {
      o->sim.jitter_ms = std::atoi(argv[++i]);
    } else if (a == "--loss" && has) {
      o->sim.loss = static_cast<float>(std::atof(argv[++i]) / 100.0);
    } else if (a == "--seconds" && has) {
      o->seconds = std::atof(argv[++i]);
    } else if (a == "--riders" && has) {
      o->riders = std::atoi(argv[++i]);
    } else if (a == "--smoke") {
      o->smoke = true;
    } else if (a == "--help" || a == "-h") {
      o->help = true;
    } else {
      *err = "Unknown or incomplete argument: " + a;
      return false;
    }
  }
  return true;
}

int run_smoke(const Options& opt) {
  if (opt.riders > 0) {
    mp::PosseSmokeOptions one;
    one.riders = opt.riders;
    one.sim = opt.sim;
    one.project_dir = opt.project;
    const mp::PosseSmokeResult r = mp::run_posse_smoke(one);
    if (!r.ok) {
      std::cerr << "[smoke] ts_server FAILED: " << r.failure << '\n';
      return 1;
    }
    std::cout << "[smoke] posse " << r.summary << "\n[smoke] ts_server OK\n";
    return 0;
  }
  // Two riders on a rough line, then a full posse of four on a fair one.
  mp::PosseSmokeOptions two;
  two.riders = 2;
  two.sim = net::LinkSim{90, 25, 0.10f};
  two.project_dir = opt.project;
  const mp::PosseSmokeResult a = mp::run_posse_smoke(two);
  if (!a.ok) {
    std::cerr << "[smoke] ts_server FAILED (2 riders): " << a.failure << '\n';
    return 1;
  }
  std::cout << "[smoke] posse " << a.summary << '\n';
  mp::PosseSmokeOptions four;
  four.riders = 4;
  four.sim = net::LinkSim{40, 10, 0.05f};
  four.project_dir = opt.project;
  const mp::PosseSmokeResult b = mp::run_posse_smoke(four);
  if (!b.ok) {
    std::cerr << "[smoke] ts_server FAILED (4 riders): " << b.failure << '\n';
    return 1;
  }
  std::cout << "[smoke] posse " << b.summary << '\n';
  std::cout << "[smoke] ts_server OK\n";
  return 0;
}

int run_server(const Options& opt) {
  mp::GameServer server;
  mp::ServerConfig cfg;
  cfg.project_dir = opt.project;
  cfg.port = static_cast<std::uint16_t>(opt.port);
  cfg.snapshot_every = opt.snapshot_every;
  cfg.sim = opt.sim;
  std::string err;
  if (!server.start(std::move(cfg), &err)) {
    std::cerr << "[ts_server] " << err << '\n';
    return 1;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  auto last = t0;
  auto report_at = t0;
  while (!g_quit) {
    const auto now = Clock::now();
    server.update(std::chrono::duration<double>(now - last).count());
    last = now;
    for (const std::string& line : server.take_log()) {
      std::cout << "[ts_server] " << line << '\n';
    }
    for (const runtime::LogEntry& e : server.take_world_logs()) {
      std::cout << "[telegraph] " << runtime::format_log(e, true) << '\n';
    }
    if (now - report_at >= std::chrono::seconds(10)) {
      report_at = now;
      const mp::ServerStats& s = server.stats();
      std::cout << "[ts_server] tick " << server.tick() << ", " << server.rider_count()
                << " rider(s), " << s.snapshot_hz << " snapshots/s each, "
                << s.kbps_out << " KB/s out\n";
    }
    std::cout.flush();
    if (opt.seconds >= 0.0 && std::chrono::duration<double>(now - t0).count() >= opt.seconds) {
      break;
    }
    // Sleep to just short of the next tick (1 ms granularity is plenty).
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cout << "[ts_server] closing up after " << server.tick() << " ticks. So long.\n";
  server.stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string err;
  if (!parse_args(argc, argv, &opt, &err)) {
    std::cerr << err << '\n' << kUsage;
    return 2;
  }
  if (opt.help) {
    std::cout << kUsage;
    return 0;
  }
  std::cout << "TombStone Server\n";
  if (opt.smoke) return run_smoke(opt);
  if (opt.project.empty()) {
    std::cerr << "ts_server needs a project.\n" << kUsage;
    return 2;
  }
  if (!fs::is_directory(opt.project)) {
    std::cerr << "No project folder at " << opt.project << '\n';
    return 2;
  }
  return run_server(opt);
}
