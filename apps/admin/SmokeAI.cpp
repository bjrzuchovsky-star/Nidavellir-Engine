#include "SmokeAI.h"

#include "ai/BehaviorTree.h"
#include "ai/GridPath.h"
#include "ai/NavMesh.h"
#include "ai/Perception.h"
#include "ai/StateMachine.h"
#include "ai/Steering.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace ai = ts::ai;

int run_ai_world_smoke();  // SmokeAIWorld.cpp

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] ai FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

struct Rng {
  std::uint64_t s;
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<std::uint32_t>(s >> 11);
  }
  float f() { return static_cast<float>(next() % 100000u) / 100000.0f; }
};

struct Grid {
  int w = 0, h = 0;
  std::vector<std::uint8_t> solid;
  bool walk(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h && !solid[static_cast<std::size_t>(y * w + x)]; }
  ai::WalkFn fn() const {
    return [this](int x, int y) { return walk(x, y); };
  }
};

Grid random_grid(int w, int h, float density, std::uint64_t seed) {
  Grid g;
  g.w = w;
  g.h = h;
  g.solid.assign(static_cast<std::size_t>(w * h), 0);
  Rng r{seed};
  for (auto& c : g.solid) c = r.f() < density ? 1 : 0;
  return g;
}

// Every step 8-adjacent, walkable, no corner cutting.
bool valid_path(const Grid& g, const std::vector<ai::Cell>& cells) {
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (!g.walk(cells[i].x, cells[i].y)) return false;
    if (i == 0) continue;
    const int dx = cells[i].x - cells[i - 1].x, dy = cells[i].y - cells[i - 1].y;
    if (std::abs(dx) > 1 || std::abs(dy) > 1 || (dx == 0 && dy == 0)) return false;
    if (dx && dy && (!g.walk(cells[i - 1].x + dx, cells[i - 1].y) || !g.walk(cells[i - 1].x, cells[i - 1].y + dy))) {
      return false;
    }
  }
  return true;
}

int grid_paths() {
  int checked = 0, unreachable = 0;
  std::size_t astar_exp = 0, jps_exp = 0;
  for (int trial = 0; trial < 60; ++trial) {
    const Grid g = random_grid(48, 40, 0.28f, 0x1000 + static_cast<std::uint64_t>(trial));
    Rng r{0x77 + static_cast<std::uint64_t>(trial)};
    ai::Cell s{static_cast<int>(r.next() % 48), static_cast<int>(r.next() % 40)};
    ai::Cell e{static_cast<int>(r.next() % 48), static_cast<int>(r.next() % 40)};
    Grid gg = g;
    gg.solid[static_cast<std::size_t>(s.y * 48 + s.x)] = 0;
    gg.solid[static_cast<std::size_t>(e.y * 48 + e.x)] = 0;
    ai::GridOptions o;
    o.bounds = {0, 0, 48, 40};
    const ai::GridPath ref = ai::dijkstra(gg.fn(), s, e, o);
    const ai::GridPath a = ai::astar(gg.fn(), s, e, o);
    const ai::GridPath j = ai::jps(gg.fn(), s, e, o);
    if (ref.found != a.found || ref.found != j.found) {
      return fail("trial " + std::to_string(trial) + ": reachability disagrees (dijkstra " +
                  std::to_string(ref.found) + ", A* " + std::to_string(a.found) + ", JPS " + std::to_string(j.found) +
                  ")");
    }
    if (!ref.found) {
      ++unreachable;
      continue;
    }
    if (!near(a.cost, ref.cost) || !near(j.cost, ref.cost) || !near(ai::path_length(j.cells), ref.cost) ||
        !near(ai::path_length(a.cells), a.cost)) {
      char b[160];
      std::snprintf(b, sizeof(b), "trial %d: costs dijkstra %.3f A* %.3f JPS %.3f (JPS cells %.3f)", trial, ref.cost,
                    a.cost, j.cost, ai::path_length(j.cells));
      return fail(b);
    }
    if (!valid_path(gg, a.cells) || !valid_path(gg, j.cells) || !(a.cells.front() == s) || !(j.cells.back() == e)) {
      return fail("trial " + std::to_string(trial) + ": illegal step (corner cut / wall)");
    }
    astar_exp += a.expanded;
    jps_exp += j.expanded;
    ++checked;
  }
  if (checked < 30) return fail("too few reachable trials");
  // Weighted cells: A* matches Dijkstra with costs too, and detours mud.
  {
    Grid g = random_grid(30, 30, 0.0f, 3);
    ai::GridOptions o;
    o.bounds = {0, 0, 30, 30};
    o.cost = [](int x, int y) { return (x >= 10 && x <= 19 && y >= 5 && y <= 24) ? 6.0f : 1.0f; };
    const ai::GridPath ref = ai::dijkstra(g.fn(), {2, 15}, {27, 15}, o);
    const ai::GridPath a = ai::astar(g.fn(), {2, 15}, {27, 15}, o);
    if (!a.found || !near(a.cost, ref.cost) || !(a.cost < 25.0f * 6.0f)) return fail("weighted A*");
    bool mud = false;
    for (const ai::Cell& c : a.cells) mud = mud || (c.x >= 10 && c.x <= 19 && c.y >= 5 && c.y <= 24);
    if (mud) return fail("weighted A* walked through the mud");
  }
  // Smoothing keeps the line on open ground.
  {
    Grid g = random_grid(20, 20, 0.0f, 1);
    for (int y = 0; y < 15; ++y) g.solid[static_cast<std::size_t>(y * 20 + 10)] = 1;
    ai::GridOptions o;
    o.bounds = {0, 0, 20, 20};
    const ai::GridPath p = ai::astar(g.fn(), {2, 2}, {17, 2}, o);
    const std::vector<ai::Cell> sm = ai::smooth_path(g.fn(), p.cells);
    if (sm.size() < 3 || sm.size() >= p.cells.size()) return fail("smooth_path");
    for (std::size_t i = 1; i < sm.size(); ++i) {
      if (!ai::line_walkable(g.fn(), sm[i - 1], sm[i])) return fail("smoothed segment crosses the wall");
    }
  }
  // HPA*: a 512 x 512 world with walls every 32 cells (gaps), lazily built.
  {
    Grid g;
    g.w = g.h = 512;
    g.solid.assign(static_cast<std::size_t>(512 * 512), 0);
    Rng r{99};
    for (int x = 31; x < 512; x += 32) {
      const int gap = static_cast<int>(r.next() % 480) + 16;
      for (int y = 0; y < 512; ++y) {
        if (std::abs(y - gap) > 2) g.solid[static_cast<std::size_t>(y * 512 + x)] = 1;
      }
    }
    for (int i = 0; i < 4000; ++i) g.solid[static_cast<std::size_t>(r.next() % (512u * 512u))] = 1;
    const ai::Cell s{3, 250}, e{500, 260};
    g.solid[static_cast<std::size_t>(s.y * 512 + s.x)] = 0;
    g.solid[static_cast<std::size_t>(e.y * 512 + e.x)] = 0;
    ai::HierGrid hg(g.fn(), 16);
    const auto t0 = std::chrono::steady_clock::now();
    const ai::GridPath hp = hg.find(s, e);
    const double hms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ai::GridOptions o;
    o.bounds = {0, 0, 512, 512};
    const auto t1 = std::chrono::steady_clock::now();
    const ai::GridPath opt = ai::jps(g.fn(), s, e, o);
    const double jms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    if (!opt.found || !hp.found) return fail("HPA*: path through the walls");
    if (!valid_path(g, hp.cells) || !(hp.cells.front() == s) || !(hp.cells.back() == e)) {
      return fail("HPA*: illegal path");
    }
    const float ratio = hp.cost / opt.cost;
    if (ratio > 1.25f) return fail("HPA*: " + std::to_string(ratio) + "x the optimal length");
    // A second query reuses the clusters (cheap), an edit invalidates one.
    const std::size_t built = hg.clusters_built();
    const ai::GridPath again = hg.find({5, 100}, {490, 400});
    g.solid[static_cast<std::size_t>(260 * 512 + 250)] = 1;
    hg.invalidate(250, 260);
    const ai::GridPath after = hg.find(s, e);
    if (!again.found || !after.found || !valid_path(g, after.cells)) return fail("HPA*: reuse / invalidate");
    std::printf("[smoke] ai grid OK (%d random maps: A* = JPS = Dijkstra cost, JPS %zu vs A* %zu expansions; "
                "HPA* 512^2 %.2fx optimal in %.1f ms (JPS %.1f ms), %zu clusters)\n",
                checked, jps_exp, astar_exp, ratio, hms, jms, built);
    (void)unreachable;
  }
  // Flow field: distances equal Dijkstra, following arrows reaches the goal.
  {
    const Grid g = random_grid(40, 40, 0.25f, 4242);
    Grid gg = g;
    gg.solid[static_cast<std::size_t>(20 * 40 + 20)] = 0;
    ai::FlowField ff;
    ff.build(gg.fn(), {0, 0, 40, 40}, {{20, 20}});
    ai::GridOptions o;
    o.bounds = {0, 0, 40, 40};
    int followed = 0;
    for (int y = 0; y < 40; y += 3) {
      for (int x = 0; x < 40; x += 3) {
        if (!gg.walk(x, y)) continue;
        const ai::GridPath ref = ai::dijkstra(gg.fn(), {x, y}, {20, 20}, o);
        if (ref.found != ff.reachable(x, y)) return fail("flow field reachability");
        if (!ref.found) continue;
        if (!near(ff.distance(x, y), ref.cost)) return fail("flow field distance");
        int cx = x, cy = y;
        for (int k = 0; k < 400 && !(cx == 20 && cy == 20); ++k) {
          float dx, dy;
          ff.direction(cx, cy, &dx, &dy);
          cx += (dx > 0.1f) - (dx < -0.1f);
          cy += (dy > 0.1f) - (dy < -0.1f);
          if (!gg.walk(cx, cy)) return fail("flow field walks into a wall");
        }
        if (!(cx == 20 && cy == 20)) return fail("flow field never arrives");
        ++followed;
      }
    }
    std::printf("[smoke] ai flow field OK (%zu cells reached, %d starts followed home)\n", ff.reached(), followed);
  }
  return 0;
}

int navmesh() {
  // 20 x 20 ground, a wall across the middle with a gap at one end, and a
  // raised platform (1.5 m, unreachable by walking) joined by a link.
  ai::NavGeometry geo;
  geo.add_plane({0, 0, 0}, 10.0f, 10.0f);
  geo.add_box({-1.5f, 1.0f, 0.0f}, {8.5f, 1.0f, 0.25f});  // wall x -10..7, gap 7..10
  geo.add_box({0.0f, 0.75f, -7.0f}, {2.0f, 0.75f, 2.0f});  // platform top at y 1.5
  ai::NavMesh mesh;
  std::string err;
  if (!mesh.build(geo, ai::NavBuildConfig{}, {}, &err)) return fail("navmesh build: " + err);
  const ai::NavPath p = mesh.find_path({-6, 0, 5}, {-6, 0, -4});
  if (!p.found || p.partial || p.points.size() < 3) return fail("navmesh path around the wall");
  if (!(p.length() > 20.0f)) return fail("navmesh path too short (" + std::to_string(p.length()) + " m): went through the wall?");
  for (const ai::V3& q : p.points) {
    if (std::fabs(q.z) < 0.6f && q.x < 6.5f) return fail("navmesh path crosses the wall");
  }
  ai::V3 hit;
  if (mesh.raycast({-6, 0, 5}, {-6, 0, -4}, &hit) || !(hit.z > 0.0f)) return fail("navmesh raycast should hit the wall");
  // Platform: no walking route up; a link from the ground makes one.
  const ai::NavPath up = mesh.find_path({0, 0, -3}, {0, 1.5f, -7});
  if (up.found && !up.partial) return fail("platform should be unreachable without a link");
  ai::OffMeshLink link;
  link.a = {0.0f, 0.0f, -4.4f};
  link.b = {0.0f, 1.5f, -5.6f};
  ai::NavMesh linked;
  if (!linked.build(geo, ai::NavBuildConfig{}, {link}, &err)) return fail("navmesh build with link: " + err);
  const ai::NavPath up2 = linked.find_path({0, 0, -2}, {0, 1.5f, -7});
  const bool via = std::any_of(up2.offmesh.begin(), up2.offmesh.end(), [](std::uint8_t f) { return f != 0; });
  if (!up2.found || up2.partial || !via) return fail("off-mesh link path");
  std::uint64_t seed = 7;
  ai::V3 rp;
  if (!linked.random_point({0, 0, 5}, 3.0f, &seed, &rp) || ai::distance_xz(rp, {0, 0, 5}) > 3.5f) {
    return fail("random point");
  }
  std::printf("[smoke] ai navmesh OK (%d polys, %d detail tris in %.1f ms; %.1f m around the wall, %zu corners; "
              "platform via off-mesh link)\n",
              mesh.stats().polys, mesh.stats().detail_tris, mesh.stats().build_ms, p.length(), p.points.size());

  // Crowd: two files of 6 cross through each other's lane.
  ai::NavGeometry open;
  open.add_plane({0, 0, 0}, 12.0f, 12.0f);
  ai::NavMesh field;
  if (!field.build(open, ai::NavBuildConfig{}, {}, &err)) return fail("crowd mesh: " + err);
  ai::NavCrowd crowd(field, 32, 0.6f);
  std::vector<int> ids;
  std::vector<ai::V3> goals;
  for (int i = 0; i < 6; ++i) {
    const float z = -2.5f + static_cast<float>(i);
    ai::CrowdAgentParams prm;
    prm.radius = 0.35f;
    ids.push_back(crowd.add({-8.0f, 0, z}, prm));
    goals.push_back({8.0f, 0, z});
    ids.push_back(crowd.add({8.0f, 0, z + 0.2f}, prm));
    goals.push_back({-8.0f, 0, z + 0.2f});
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] < 0 || !crowd.set_target(ids[i], goals[i])) return fail("crowd add / target");
  }
  float worst = 1e9f;
  int arrived_at = -1;
  for (int step = 0; step < 60 * 20; ++step) {
    crowd.update(1.0f / 60.0f);
    worst = std::min(worst, crowd.min_clearance());
    bool all = true;
    for (int id : ids) all = all && crowd.arrived(id, 0.6f);
    if (all) {
      arrived_at = step;
      break;
    }
  }
  if (arrived_at < 0) return fail("crowd agents did not all arrive");
  if (worst < -0.08f) return fail("crowd agents overlapped by " + std::to_string(-worst) + " m");
  // The 2D steering crowd resolves overlaps exactly.
  ai::SteerCrowd sc;
  for (int i = 0; i < 10; ++i) {
    ai::SteerAgent a;
    a.pos = {static_cast<float>(i % 2 ? 6 : -6), 0, static_cast<float>(i / 2) * 0.9f};
    a.radius = 0.4f;
    sc.add(a);
  }
  float sc_worst = 1e9f;
  for (int step = 0; step < 600; ++step) {
    for (std::size_t i = 0; i < sc.agents().size(); ++i) {
      const ai::V3 goal{i % 2 ? -6.0f : 6.0f, 0, static_cast<float>(i / 2) * 0.9f};
      sc.set_desired(i, ai::arrive(sc.agents()[i], goal, 1.0f));
    }
    sc.step(1.0f / 60.0f);
    sc_worst = std::min(sc_worst, sc.min_clearance());
  }
  if (sc_worst < -1e-3f) return fail("steering crowd overlap " + std::to_string(sc_worst));
  std::printf("[smoke] ai crowd OK (12 Detour agents crossed in %.1f s, worst gap %.3f m; 10 steering agents, "
              "worst gap %.4f m)\n",
              static_cast<float>(arrived_at) / 60.0f, worst, sc_worst);
  return 0;
}

// --- Behaviour trees ---------------------------------------------------------------------

struct StubHost : ai::BtHost {
  std::vector<std::string> log_lines;
  std::vector<std::string> halted;
  int move_ticks = 3;
  bool see = false;
  ai::BtStatus action(ai::BtContext& c) override {
    const std::string& t = c.node->type;
    if (t == "move_to" || t == "wander") {
      if (c.entering) c.counter = 0;
      return ++c.counter >= move_ticks ? ai::BtStatus::Success : ai::BtStatus::Running;
    }
    if (t == "attack") {
      c.bb->set_number("hits", c.bb->number("hits") + 1);
      return ai::BtStatus::Success;
    }
    return ai::BtStatus::Failure;
  }
  bool condition(ai::BtContext& c) override { return c.node->type == "can_see" ? see : false; }
  void halt(ai::BtContext& c) override { halted.push_back(c.node->label()); }
  void log(ai::BtContext&, const std::string& text) override { log_lines.push_back(text); }
};

std::shared_ptr<ai::BtTree> tree(const std::string& text) {
  auto t = std::make_shared<ai::BtTree>();
  std::string err;
  if (!t->parse(text, &err)) {
    std::cerr << "bt parse: " << err << '\n';
    return nullptr;
  }
  return t;
}

int behaviour_trees() {
  // Semantics.
  {
    auto t = tree(R"({"root": {"type": "sequence", "children": [
        {"type": "set", "key": "a", "value": 1},
        {"type": "wait", "seconds": 0.05},
        {"type": "check", "key": "a", "op": "==", "value": 1},
        {"type": "log", "text": "done"}]}})");
    if (!t) return fail("bt parse");
    StubHost h;
    ai::BtInstance bt(t, 1);
    if (bt.tick(0.02f, h) != ai::BtStatus::Running || bt.tick(0.02f, h) != ai::BtStatus::Running ||
        bt.tick(0.02f, h) != ai::BtStatus::Success || h.log_lines.size() != 1) {
      return fail("bt sequence + wait");
    }
  }
  {
    // Reactive selector: seeing the target interrupts wandering (halted).
    auto t = tree(R"({"root": {"type": "selector", "reactive": true, "children": [
        {"type": "sequence", "name": "fight", "children": [{"type": "can_see"}, {"type": "attack"}]},
        {"type": "wander", "name": "roam"}]}})");
    StubHost h;
    h.move_ticks = 100;
    ai::BtInstance bt(t, 1);
    bt.tick(0.1f, h);
    bt.tick(0.1f, h);
    if (bt.running_path() != "selector > roam") return fail("bt running path: " + bt.running_path());
    h.see = true;
    if (bt.tick(0.1f, h) != ai::BtStatus::Success || bt.blackboard().number("hits") != 1.0 ||
        std::find(h.halted.begin(), h.halted.end(), "roam") == h.halted.end()) {
      return fail("bt reactive selector interrupt");
    }
  }
  {
    // Decorators: repeat 3, timeout, cooldown, inverter, retry, parallel.
    auto t = tree(R"({"root": {"type": "sequence", "children": [
        {"type": "repeat", "times": 3, "child": {"type": "inc", "key": "n"}},
        {"type": "inverter", "child": {"type": "fail"}},
        {"type": "parallel", "success": 1, "children": [{"type": "wait", "seconds": 0.3}, {"type": "move_to"}]}]}})");
    StubHost h;
    ai::BtInstance bt(t, 1);
    ai::BtStatus st = ai::BtStatus::Running;
    int ticks = 0;
    while (st == ai::BtStatus::Running && ticks < 50) {
      st = bt.tick(0.1f, h);
      ++ticks;
    }
    if (st != ai::BtStatus::Success || bt.blackboard().number("n") != 3.0) {
      return fail("bt repeat / inverter / parallel (n=" + std::to_string(bt.blackboard().number("n")) + ")");
    }
    auto t2 = tree(R"({"root": {"type": "selector", "children": [
        {"type": "timeout", "seconds": 0.25, "child": {"type": "running", "name": "stuck"}},
        {"type": "cooldown", "seconds": 1.0, "child": {"type": "inc", "key": "fired"}}]}})");
    StubHost h2;
    ai::BtInstance b2(t2, 1);
    for (int i = 0; i < 40; ++i) b2.tick(0.1f, h2);
    // Each cycle: 3 ticks running + timeout -> cooldown fires (once per s).
    const double fired = b2.blackboard().number("fired");
    if (fired < 2.0 || fired > 5.0) return fail("bt timeout / cooldown fired " + std::to_string(fired));
  }
  {
    // Utility: the hungriest need wins, with hysteresis.
    auto t = tree(R"({"root": {"type": "utility", "hysteresis": 0.05, "children": [
        {"type": "set", "name": "eat", "key": "did", "value": "eat",
         "utility": {"considerations": [{"key": "hunger", "min": 0, "max": 100, "curve": "linear"}]}},
        {"type": "set", "name": "sleep", "key": "did", "value": "sleep",
         "utility": {"considerations": [{"key": "tired", "min": 0, "max": 100, "curve": "quadratic"}]}}]}})");
    StubHost h;
    ai::BtInstance bt(t, 1);
    bt.blackboard().set_number("hunger", 70);
    bt.blackboard().set_number("tired", 80);  // 0.64 < 0.7
    bt.tick(0.1f, h);
    if (bt.blackboard().text("did") != "eat") return fail("utility pick (eat)");
    bt.blackboard().set_number("tired", 90);  // 0.81 > 0.7
    bt.tick(0.1f, h);
    if (bt.blackboard().text("did") != "sleep") return fail("utility pick (sleep)");
    if (!near(ai::utility_curve("logistic", 0.5f, 0.5f), 0.5f) || ai::utility_curve("step", 0.4f, 0.5f) != 0.0f) {
      return fail("utility curves");
    }
  }
  // Determinism: random composites + chance + wait ranges, traced twice.
  const std::string guard = R"({"name": "patrol", "blackboard": {"alert": 0},
    "root": {"type": "repeat", "child": {"type": "selector", "children": [
      {"type": "sequence", "children": [{"type": "chance", "p": 0.3}, {"type": "inc", "key": "alert"},
                                         {"type": "wait", "seconds": 0.1, "max": 0.5}]},
      {"type": "random", "children": [
        {"type": "sequence", "name": "a", "children": [{"type": "move_to"}, {"type": "inc", "key": "a"}]},
        {"type": "sequence", "name": "b", "children": [{"type": "wander"}, {"type": "inc", "key": "b"}]},
        {"type": "sequence", "name": "c", "children": [{"type": "wait", "seconds": 0.2}, {"type": "inc", "key": "c"}]}]}]}}})";
  auto t = tree(guard);
  if (!t || !t->lint().empty()) return fail("bt guard tree lint");
  auto run = [&](std::uint64_t seed) {
    StubHost h;
    ai::BtInstance bt(t, seed);
    bt.set_tracing(true);
    for (int i = 0; i < 400; ++i) bt.tick(1.0f / 30.0f, h);
    return std::make_pair(bt.trace(), bt.blackboard());
  };
  const auto r1 = run(1849), r2 = run(1849), r3 = run(77);
  if (r1.first.size() < 400 || !(r1.first == r2.first) || !(r1.second == r2.second)) return fail("bt not deterministic");
  if (r1.first == r3.first) return fail("bt seed has no effect");
  // Round trip.
  ai::BtTree back;
  std::string err;
  if (!back.parse(t->write(), &err) || back.write() != t->write() || back.nodes.size() != t->nodes.size()) {
    return fail("bt json round trip: " + err);
  }
  // Editing helpers.
  ai::BtTree ed = *t;
  const int n = ed.add(1, "log");
  ed.nodes[static_cast<std::size_t>(n)].params.set("text", ai::JsonValue::string("hi"));
  ed.remove(n);
  if (ed.write() != t->write()) return fail("bt add / remove");
  std::printf("[smoke] ai behaviour trees OK (sequence/selector/reactive interrupt/parallel/repeat/timeout/"
              "cooldown/utility; %zu traced node runs identical for one seed, differ for another; json round trip; "
              "alert %g a %g b %g c %g)\n",
              r1.first.size(), r1.second.number("alert"), r1.second.number("a"), r1.second.number("b"),
              r1.second.number("c"));
  return 0;
}

int perception_fsm() {
  ai::Perception p;
  p.config.sight_range = 10.0f;
  p.config.fov_deg = 90.0f;
  p.config.hearing_range = 8.0f;
  p.config.memory = 2.0f;
  const ai::V3 eye{0, 1.6f, 0}, fwd{0, 0, 1};
  // 1 ahead in the cone, 2 behind, 3 too far, 4 ahead but behind a wall,
  // 5 behind but within near_sense.
  std::vector<ai::Perception::Candidate> c = {
      {1, {1, 1, 5}}, {2, {0, 1, -5}}, {3, {0, 1, 15}}, {4, {-2, 1, 6}}, {5, {0, 1, -1}}};
  auto clear = [](const ai::V3&, const ai::V3& to) { return !(to.x < -1.0f); };  // wall on the left
  p.see(eye, fwd, c, clear, 0.0);
  if (!p.sees(1) || p.sees(2) || p.sees(3) || p.sees(4) || !p.sees(5)) return fail("sight cone / LOS / near sense");
  if (!ai::in_sight_cone(eye, fwd, {4.9f, 1.6f, 5.0f}, 10.0f, 90.0f) ||
      ai::in_sight_cone(eye, fwd, {5.2f, 1.6f, 5.0f}, 10.0f, 90.0f)) {
    return fail("cone edge (45 deg)");
  }
  // Hearing: a gunshot (loudness 2) at 12 m is heard, a step (0.5) at 6 m not.
  if (!p.hear(9, {12, 0, 0}, 2.0f, "gunshot", eye, 0.5) || p.hear(10, {6, 0, 0}, 0.5f, "step", eye, 0.5)) {
    return fail("hearing range x loudness");
  }
  const ai::Percept* h = p.last_heard(0.6, 1.0);
  if (!h || h->sound != "gunshot" || h->id != 9) return fail("last heard");
  const ai::Percept* best = p.best(eye);
  if (!best || best->id != 5) return fail("best percept (nearest visible)");
  p.see(eye, fwd, {}, clear, 1.0);
  p.forget(1.5);
  if (!p.find(1) || p.sees(1)) return fail("memory keeps unseen");
  p.forget(4.0);
  if (!p.known().empty()) return fail("memory forgets after config.memory");
  // FSM.
  ai::StateMachine fsm;
  int roars = 0;
  bool seen = false;
  fsm.add("idle", {}, [&](float) { return seen ? "chase" : ""; });
  fsm.add("chase", [&] { ++roars; }, [&](float) { return seen ? "" : "idle"; });
  fsm.start("idle");
  fsm.update(0.1f);
  seen = true;
  fsm.update(0.1f);
  fsm.update(0.1f);
  if (fsm.current() != "chase" || roars != 1 || !near(fsm.time_in_state(), 0.1f)) return fail("fsm");
  seen = false;
  fsm.update(0.1f);
  if (fsm.current() != "idle" || fsm.previous() != "chase" || fsm.changes() != 3) return fail("fsm back to idle");
  std::printf("[smoke] ai perception + fsm OK (cone 90 deg, LOS, near sense, hearing x loudness, memory; idle <-> chase)\n");
  return 0;
}

}  // namespace

int run_ai_smoke() {
  if (grid_paths() != 0) return 1;
  if (navmesh() != 0) return 1;
  if (behaviour_trees() != 0) return 1;
  if (perception_fsm() != 0) return 1;
  if (run_ai_world_smoke() != 0) return 1;
  std::printf("[smoke] ai OK\n");
  return 0;
}
