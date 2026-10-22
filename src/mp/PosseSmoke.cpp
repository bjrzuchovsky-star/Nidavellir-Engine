#include "mp/PosseSmoke.h"

#include "mp/LocalPosse.h"
#include "scene/SceneJson.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace ts {
namespace tombstone {
namespace mp {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kBellScript = R"lua(-- Posse smoke bell: rings a toast every second for the whole posse, and
-- rolls a spawned tumbleweed across town every two seconds (buried one
-- second later), so snapshots carry spawns and destroys.
props = {}
local weed = nil

function on_tick(dt)
  local t = tick()
  if t % 60 == 30 then
    toast("The bell rings at tick " .. t, 0.5)
  end
  if t % 120 == 0 then
    weed = spawn("Tumbleweed", 40, 520)
  end
  if weed and weed:alive() then
    weed:move_by(1, 0)
  end
  if t % 120 == 60 and weed then
    weed:destroy()
    weed = nil
  end
end
)lua";

Entity2D box(std::uint64_t id, const char* name, float x, float y, float w, float h) {
  Entity2D e;
  e.id = id;
  e.name = name;
  e.x = x;
  e.y = y;
  e.w = w;
  e.h = h;
  return e;
}

double seconds_since(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

}  // namespace

bool write_posse_test_project(const std::string& dir, std::string* error_out) {
  std::error_code ec;
  fs::create_directories(fs::path(dir) / "scripts", ec);
  if (ec) {
    if (error_out) *error_out = "could not create " + dir + ": " + ec.message();
    return false;
  }
  scene_json::SceneDoc doc;
  Entity2D rider = box(1, "Rider", 200, 200, 32, 48);
  rider.color[0] = 0.85f;
  rider.color[1] = 0.55f;
  rider.color[2] = 0.25f;
  rider.player = PlayerControllerData{};
  rider.player->slot = 0;
  rider.player->speed = 160.0f;
  rider.collider = default_collider(rider);
  rider.layer = 5;
  doc.entities.push_back(rider);
  for (int s = 0; s < 4; ++s) {
    Entity2D sp = box(static_cast<std::uint64_t>(2 + s), "Spawn", 200.0f + 90.0f * s, 200, 16, 16);
    sp.name = "Spawn P" + std::to_string(s + 1);
    sp.spawn = SpawnPointData{s};
    sp.color[3] = 0.4f;
    doc.entities.push_back(sp);
  }
  Entity2D wall = box(6, "Wall", 100, 60, 16, 420);
  wall.color[0] = 0.45f;
  wall.color[1] = 0.3f;
  wall.color[2] = 0.2f;
  wall.collider = default_collider(wall);
  doc.entities.push_back(wall);
  Entity2D bell = box(7, "Bell", 640, 120, 24, 24);
  bell.script = ScriptData{};
  bell.script->path = "scripts/bell.lua";
  doc.entities.push_back(bell);
  Entity2D weed = box(8, "Tumbleweed", 900, 900, 16, 16);
  weed.color[0] = 0.7f;
  weed.color[1] = 0.6f;
  weed.color[2] = 0.35f;
  doc.entities.push_back(weed);
  std::string err;
  if (!scene_json::save_file(scene_json::scene_path_for_project(dir), doc, &err)) {
    if (error_out) *error_out = err;
    return false;
  }
  std::ofstream(fs::path(dir) / "scripts" / "bell.lua", std::ios::binary) << kBellScript;
  std::ofstream(fs::path(dir) / "project.json", std::ios::binary)
      << "{\n  \"name\": \"Posse Smoke\"\n}\n";
  return true;
}

PosseSmokeResult run_posse_smoke(const PosseSmokeOptions& opt) {
  PosseSmokeResult res;
  auto fail = [&](const std::string& why) {
    res.ok = false;
    res.failure = why;
    return res;
  };
  const int n = std::clamp(opt.riders, 1, kMaxPosse);
  std::string dir = opt.project_dir;
  if (dir.empty()) {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) /
                         ("tombstone_posse_smoke_" +
                          std::to_string(Clock::now().time_since_epoch().count() % 1000000007));
    dir = tmp.string();
    std::string err;
    if (!write_posse_test_project(dir, &err)) return fail(err);
  }
  struct Cleanup {
    std::string dir;
    bool owned;
    ~Cleanup() {
      std::error_code ec;
      if (owned) fs::remove_all(dir, ec);
    }
  } cleanup{dir, opt.project_dir.empty()};

  LocalPosse posse;
  PosseConfig pc;
  pc.project_dir = dir;
  pc.riders = n;
  pc.sim = opt.sim;
  std::string err;
  if (!posse.start(std::move(pc), &err)) return fail("posse would not start: " + err);
  posse.set_bots(true);

  auto last = Clock::now();
  bool ride = true;
  std::uint64_t ridden = 0;
  // Pump everyone for real time; `ride` = bots ride, else sit still.
  auto pump = [&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    const std::uint64_t before = posse.bot_tick();
    if (ride) {
      posse.set_bots(true);
      posse.update(dt, bot_input(0, posse.bot_tick()));
    } else {
      posse.set_bots(false);
      posse.update(dt, runtime::PlayerInput{});
    }
    if (ride) ridden += posse.bot_tick() - before;
  };
  auto wait_for = [&](double seconds, const std::function<bool()>& done) {
    const auto t0 = Clock::now();
    while (seconds_since(t0) < seconds) {
      pump();
      if (done()) return true;
    }
    return done();
  };
  auto all_joined = [&]() {
    if (posse.server().rider_count() != static_cast<int>(posse.clients().size())) return false;
    for (const auto& c : posse.clients()) {
      if (!c->joined() || !c->has_snapshot()) return false;
    }
    return true;
  };

  // 1. Everybody rides in and gets a seat of their own.
  if (!wait_for(6.0, all_joined)) return fail("not every rider joined within 6 s");
  std::set<int> slots;
  std::set<std::uint64_t> riders_seen;
  for (const auto& c : posse.clients()) {
    slots.insert(c->slot());
    riders_seen.insert(c->rider());
  }
  if (static_cast<int>(slots.size()) != n || static_cast<int>(riders_seen.size()) != n) {
    return fail("riders should get distinct slots and player entities");
  }
  std::map<std::uint64_t, std::pair<float, float>> start;
  for (std::uint64_t id : riders_seen) {
    const runtime::Actor* a = posse.server().world().find(id);
    if (!a || a->hidden) return fail("a seated rider is missing or hidden on the server");
    start[id] = {a->data.x, a->data.y};
  }

  // 2. Ride under lag: watch prediction lead, toasts and spawns arrive.
  float max_lead = 0.0f;
  std::vector<bool> toasted(posse.clients().size(), false);
  std::vector<bool> saw_weed(posse.clients().size(), false);
  const std::uint64_t ride_until = posse.bot_tick() + static_cast<std::uint64_t>(opt.ride_ticks);
  while (posse.bot_tick() < ride_until) {
    pump();
    for (std::size_t i = 0; i < posse.clients().size(); ++i) {
      const GameClient& c = *posse.clients()[i];
      float sx = 0, sy = 0;
      if (c.server_rider(&sx, &sy)) {
        max_lead = std::max(max_lead, std::hypot(c.predicted_x() - sx, c.predicted_y() - sy));
      }
      toasted[i] = toasted[i] || !c.toasts().empty();
      for (const runtime::Actor& a : c.world().actors()) {
        if (a.spawned && a.data.name == "Tumbleweed") saw_weed[i] = true;
      }
    }
  }
  if (posse.server().rider_count() != n) return fail("a rider dropped during the ride");
  int moved = 0;
  for (const auto& [id, p] : start) {
    const runtime::Actor* a = posse.server().world().find(id);
    if (a && std::hypot(a->data.x - p.first, a->data.y - p.second) > 1.0f) ++moved;
  }
  if (moved == 0) return fail("nobody moved on the server: input never arrived");

  // 3. Rein in; every client must converge on the server's world.
  ride = false;
  float worst = 0.0f;
  auto converged = [&]() {
    worst = 0.0f;
    const runtime::World& sw = posse.server().world();
    for (const auto& c : posse.clients()) {
      const runtime::Actor* own = sw.find(c->rider());
      if (!own) return false;
      worst = std::max(worst, std::hypot(c->predicted_x() - own->data.x,
                                         c->predicted_y() - own->data.y));
      for (const auto& o : posse.clients()) {
        const runtime::Actor* sa = sw.find(o->rider());
        float x = 0, y = 0;
        if (!sa || !c->drawn(o->rider(), &x, &y)) return false;
        worst = std::max(worst, std::hypot(x - sa->data.x, y - sa->data.y));
      }
    }
    return worst <= 0.01f;
  };
  const auto settle_t0 = Clock::now();
  if (!wait_for(opt.settle_seconds, converged)) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "clients did not converge within %.1f s (worst %.3f px)",
                  opt.settle_seconds, static_cast<double>(worst));
    return fail(buf);
  }
  const double settle_s = seconds_since(settle_t0);
  for (std::size_t i = 0; i < posse.clients().size() && cleanup.owned; ++i) {
    if (!toasted[i]) return fail("rider " + std::to_string(i + 1) + " never saw the bell's toast");
    if (!saw_weed[i]) return fail("rider " + std::to_string(i + 1) + " never saw the spawned tumbleweed");
  }
  float max_corr = 0.0f;
  std::uint64_t corrections = 0;
  double rtt = 0.0;
  for (const auto& c : posse.clients()) {
    max_corr = std::max(max_corr, c->stats().max_correction_px);
    corrections += c->stats().corrections;
    rtt = std::max(rtt, c->stats().rtt_ms);
  }
  std::string inspected;
  if (opt.inspect) {
    inspected = opt.inspect(*posse.clients()[0]);
    if (!inspected.empty() && inspected[0] == '!') return fail(inspected.substr(1));
  }

  // 4. A fifth rider is turned away when the posse is full.
  std::string full_note = "no fifth rider test (posse of " + std::to_string(n) + ")";
  if (n == kMaxPosse) {
    GameClient fifth;
    ClientConfig cc;
    cc.port = posse.port();
    cc.name = "Latecomer";
    if (!fifth.connect(std::move(cc), &err)) return fail("fifth rider could not dial: " + err);
    const bool turned = wait_for(4.0, [&] {
      fifth.update(0.002, runtime::PlayerInput{});
      return fifth.state() == ClientState::Rejected;
    });
    if (!turned) return fail("a fifth rider should be turned away (posse full)");
    full_note = "fifth rider turned away (\"" + fifth.status() + "\")";
    fifth.disconnect();
  }

  // 5. The last rider leaves: seat freed, rider hidden, everyone told.
  std::string leave_note = "single rider: no leave test";
  if (n >= 2) {
    const int gone_slot = posse.clients().back()->slot();
    const std::uint64_t gone_rider = posse.clients().back()->rider();
    for (const auto& c : posse.clients()) c->take_events();
    posse.remove_rider();
    // Wait on the condition, with a timeout past ENet's longest peer
    // timeout (10 s), so a slow runner can't fail on timing alone.
    const bool left = wait_for(12.0, [&] {
      if (posse.server().rider_count() != n - 1) return false;
      for (const auto& c : posse.clients()) {
        if (c->roster().riders.size() != static_cast<std::size_t>(n - 1)) return false;
        const runtime::Actor* a = c->world().find(gone_rider);
        if (a && !a->hidden) return false;
      }
      return true;
    });
    if (!left) return fail("a leaving rider should free the seat and vanish for the others");
    bool heard = true;
    for (const auto& c : posse.clients()) {
      bool h = false;
      for (const std::string& e : c->take_events()) h |= e.find("rode out") != std::string::npos;
      heard = heard && h;
    }
    if (!heard) return fail("the posse should hear that a rider rode out");
    // Somebody new rides in and gets the freed seat.
    if (!posse.add_rider(&err)) return fail("rejoin: " + err);
    if (!wait_for(6.0, all_joined)) return fail("a new rider could not take the freed seat");
    if (posse.clients().back()->slot() != gone_slot) {
      return fail("the new rider should get the freed slot P" + std::to_string(gone_slot + 1));
    }
    leave_note = "P" + std::to_string(gone_slot + 1) + " left (seat freed, posse told) and " +
                 posse.clients().back()->name() + " took it";
  }
  const ServerStats st = posse.server().stats();
  posse.stop();

  char buf[640];
  std::snprintf(buf, sizeof(buf),
                "%d riders under %d ms +-%d ms, %.0f%% loss: %llu ticks ridden, converged to "
                "<=0.01 px %.2f s after reining in, prediction lead up to %.1f px, %llu "
                "reconciliations (max %.1f px), RTT %.0f ms, %llu snapshots of %zu bytes, %s, "
                "%s, %s%s%s",
                n, opt.sim.latency_ms, opt.sim.jitter_ms, static_cast<double>(opt.sim.loss) * 100.0,
                static_cast<unsigned long long>(ridden), settle_s, static_cast<double>(max_lead),
                static_cast<unsigned long long>(corrections), static_cast<double>(max_corr), rtt,
                static_cast<unsigned long long>(st.snapshots_sent), st.last_snapshot_bytes,
                cleanup.owned ? "toasts + spawned tumbleweed on every client"
                              : "own project (bell checks skipped)",
                full_note.c_str(), leave_note.c_str(), inspected.empty() ? "" : ", ",
                inspected.c_str());
  res.ok = true;
  res.summary = buf;
  return res;
}

}  // namespace mp
}  // namespace tombstone
}  // namespace ts
