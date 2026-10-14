#include "SmokeShipyard.h"

#include "mp/GameClient.h"
#include "mp/GameServer.h"
#include "plugins/RuntimePlugins.h"
#include "plugins/shipyard/Sample.h"
#include "plugins/shipyard/ShipyardPlugin.h"
#include "runtime/Input.h"
#include "runtime/Log.h"
#include "runtime/PlaySession.h"
#include "runtime/World.h"

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace ts::tombstone;
using namespace ts::tombstone::shipyard;
namespace fs = std::filesystem;

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string& tag) {
    std::error_code ec;
    path = fs::temp_directory_path(ec) /
           ("tombstone_" + tag + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007));
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

int fail(const std::string& what) {
  std::cerr << "[smoke] shipyard FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b)); }

std::string hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%016" PRIx64, v);
  return b;
}

ShipSim bare_sim(bool crew = true) {
  ShipSim sim;
  SkySettings sky;
  sky.crew = crew;
  sky.reinforce = 0.0f;
  sim.begin(nullptr, "", PartLibrary::defaults(), sky);
  return sim;
}

int find_part(const Design& d, const char* id, int x, int y) {
  for (std::size_t i = 0; i < d.parts.size(); ++i) {
    if (d.parts[i].part == id && d.parts[i].x == x && d.parts[i].y == y) return static_cast<int>(i);
  }
  return -1;
}

// --- 1. Designs ----------------------------------------------------------------------------
int smoke_design() {
  const PartLibrary lib = PartLibrary::defaults();
  if (lib.parts.size() < 9) return fail("default parts.json should carry every kind");
  for (int k = 0; k < kPartKinds; ++k) {
    bool have = false;
    for (const PartDef& d : lib.parts) have |= static_cast<int>(d.kind) == k;
    if (!have) return fail(std::string("no default part of kind ") + part_kind_id(static_cast<PartKind>(k)));
  }
  for (const Design& d : {ShipSim::sloop_design(), ShipSim::marauder_design()}) {
    const Validation v = validate(lib, d);
    if (!v.ok) return fail(d.name + " should be airworthy: " + (v.errors.empty() ? "" : v.errors.front()));
    if (!v.warnings.empty()) return fail(d.name + " should have no warnings: " + v.warnings.front());
  }
  // Disconnect: knock the bunkhouse and gangway out of the sloop.
  Design cut = ShipSim::sloop_design();
  erase_at(lib, &cut, 2, 4);
  erase_at(lib, &cut, 4, 4);
  Validation v = validate(lib, cut);
  if (v.ok || v.loose.size() != 5) {
    return fail("a cut sloop should be grounded with 5 loose parts (got " + std::to_string(v.loose.size()) + ")");
  }
  // Steam budget.
  Design hungry = ShipSim::sloop_design();
  if (!place_part(lib, &hungry, "cannon", 7, 2, 0)) return fail("cannon at (7,2)");
  if (!place_part(lib, &hungry, "shield_vane", 6, 1, 0)) return fail("vane at (6,1)");
  v = validate(lib, hungry);
  if (v.ok || v.errors.empty() || v.errors.front().find("steam") == std::string::npos) {
    return fail("burning more steam than the boiler makes should ground her");
  }
  // Thrust coverage.
  Design drifter;
  place_part(lib, &drifter, "boiler", 0, 0, 0);
  place_part(lib, &drifter, "bunkhouse", 0, 2, 0);
  place_part(lib, &drifter, "trim_jet", 2, 0, 2);
  v = validate(lib, drifter);
  bool no_fwd = false;
  for (const std::string& e : v.errors) no_fwd |= e.find("forward") != std::string::npos;
  if (!no_fwd || v.warnings.empty()) return fail("an engine-less bow should be flagged (and turn/side warned)");
  // Editing rules.
  Design ed;
  std::string why;
  if (!place_part(lib, &ed, "boiler", 3, 3, 0)) return fail("place boiler");
  if (place_part(lib, &ed, "hull", 4, 4, 0, &why)) return fail("overlap accepted");
  if (place_part(lib, &ed, "steam_engine", kMaxGrid - 1, kMaxGrid - 1, 0, &why)) return fail("off-grid accepted");
  if (!place_part(lib, &ed, "steam_engine", 5, 3, 0)) return fail("engine beside boiler");
  if (!rotate_at(lib, &ed, 5, 4) || ed.parts[1].rot != 1) return fail("rotate engine in place");
  if (part_at(lib, ed, 6, 3) != 1) return fail("a turned 1x2 engine should cover (6,3)");
  if (!erase_at(lib, &ed, 6, 3) || ed.parts.size() != 1) return fail("erase");
  // Files.
  TempDir tmp("ship_design");
  const Design sloop = ShipSim::sloop_design();
  Design back;
  if (!parse_design(write_design(sloop), &back, &why) || !(back == sloop)) return fail("design round trip: " + why);
  const std::string path = (tmp.path / "shipyard" / "designs" / "sloop.ship.json").string();
  if (!save_design(path, sloop, &why) || !load_design(path, &back, &why) || !(back == sloop)) {
    return fail("design save / load: " + why);
  }
  if (list_designs(tmp.path.string()).size() != 1) return fail("list_designs should find the sloop");
  if (parse_design(R"({"format": "tombstone.ship", "version": 99, "parts": []})", &back, &why)) {
    return fail("a design from the future should be refused");
  }
  if (parse_design(R"({"format": "something.else"})", &back, &why)) return fail("non-ship json accepted");
  // parts.json: broken -> built-ins with a warning; edited -> read.
  PartLibrary lib2;
  std::vector<std::string> warns;
  fs::create_directories(tmp.path / "shipyard");
  { std::ofstream(tmp.path / "shipyard" / "parts.json") << "{ broken"; }
  if (!lib2.load(tmp.path.string(), &why, &warns) || lib2.parts.size() != lib.parts.size() || warns.empty()) {
    return fail("broken parts.json should fall back to the built-ins with a warning");
  }
  { std::ofstream(tmp.path / "shipyard" / "parts.json")
        << R"({"parts": [{"id": "plank", "kind": "hull", "mass": 3, "hp": 9}, {"id": "x", "kind": "nope"}]})"; }
  warns.clear();
  if (!lib2.load(tmp.path.string(), &why, &warns) || lib2.parts.size() != 1 || lib2.parts[0].hp != 9 || warns.size() != 1) {
    return fail("an edited parts.json should be read (bad kinds skipped)");
  }
  std::cout << "[smoke] shipyard designs OK (" << lib.parts.size() << " stock parts over all "
            << kPartKinds << " kinds; sloop + marauder airworthy; cut hull -> 5 loose parts; steam "
               "budget and missing forward thrust ground her; overlap / off-grid refused; rotate + "
               "erase; .ship.json round trip, future version refused; parts.json fallback + edit)\n";
  return 0;
}

// --- 2. Mass and thrust ----------------------------------------------------------------------
int smoke_physics() {
  const PartLibrary lib = PartLibrary::defaults();
  // Hand math: hull, hull, armor in a row.
  Design row;
  place_part(lib, &row, "hull", 0, 0, 0);
  place_part(lib, &row, "hull", 1, 0, 0);
  place_part(lib, &row, "armor", 2, 0, 0);
  const ShipStats s = compute_stats(lib, resolve(lib, row));
  const float m = 1.0f + 1.0f + 2.5f;
  const float cx = (8.0f * 1.0f + 24.0f * 1.0f + 40.0f * 2.5f) / m;
  float inertia = 0.0f;
  const float xs[3] = {8, 24, 40}, ms[3] = {1, 1, 2.5f};
  for (int i = 0; i < 3; ++i) inertia += ms[i] * ((xs[i] - cx) * (xs[i] - cx) + (256.0f + 256.0f) / 12.0f);
  if (!near(s.mass, m) || !near(s.cx, cx) || !near(s.cy, 8.0f) || !near(s.inertia, inertia)) {
    return fail("mass / centre / inertia of a 3-part row disagree with hand math");
  }
  // A tick of full ahead from rest: v = F / m * dt * (1 - damping * dt).
  ShipSim sim = bare_sim();
  const std::uint32_t id = sim.spawn(ShipSim::sloop_design(), 0, 0, Team::Posse, false, -1);
  const ShipStats ss = compute_stats(lib, sim.find(id)->poses());
  sim.set_helm(id, 0.0f, -1.0f, 0, false);
  sim.step_bare();
  const Ship* sh = sim.find(id);
  const float dt = static_cast<float>(runtime::kTickSeconds);
  const float want = -ss.thrust[kFwd] / ss.mass * dt * (1.0f - kLinearDamping * dt);
  if (!near(sh->vy, want, 1e-4f) || std::fabs(sh->vx) > 1e-4f) {
    return fail("full ahead: vy " + std::to_string(sh->vy) + " want " + std::to_string(want));
  }
  int engines_firing = 0;
  for (const LivePart& p : sh->parts) engines_firing += p.firing && lib.get(p.def)->kind == PartKind::Engine;
  if (engines_firing != 2) return fail("full ahead should light exactly the two paddle engines");
  // Turning both ways, strafing, astern.
  auto run = [&](float ix, float iy, int turn, int ticks) {
    ShipSim t = bare_sim();
    const std::uint32_t k = t.spawn(ShipSim::sloop_design(), 0, 0, Team::Posse, false, -1);
    for (int i = 0; i < ticks; ++i) {
      t.set_helm(k, ix, iy, turn, false);
      t.step_bare();
    }
    return *t.find(k);
  };
  const Ship cw = run(0, 0, 1, 30), ccw = run(0, 0, -1, 30);
  if (!(cw.spin > 0.05f && cw.angle > 0.0f) || !(ccw.spin < -0.05f && ccw.angle < 0.0f)) {
    return fail("E / Q should swing her clockwise / anticlockwise via trim jets");
  }
  const Ship right = run(1, 0, 0, 30), back = run(0, 1, 0, 30);
  if (!(right.vx > 1.0f) || !(back.vy > 1.0f)) return fail("strafe right / astern");
  // Damping: coast to a near stop.
  ShipSim coast = bare_sim();
  const std::uint32_t c = coast.spawn(ShipSim::sloop_design(), 0, 0, Team::Posse, false, -1);
  coast.find(c)->vx = 200.0f;
  for (int i = 0; i < 600; ++i) coast.step_bare();
  if (std::fabs(coast.find(c)->vx) > 1.0f) return fail("damping should bleed off a coasting ship");
  // Steam: lose the boiler and the engines go cold.
  ShipSim cold = bare_sim();
  const std::uint32_t k = cold.spawn(ShipSim::sloop_design(), 0, 0, Team::Posse, false, -1);
  const int boiler = find_part(ShipSim::sloop_design(), "boiler", 2, 2);
  const int split_before = static_cast<int>(cold.ships().size());
  cold.damage_part(k, boiler, 9999);
  cold.set_helm(k, 0, -1, 0, false);
  cold.step_bare();
  const Ship* ck = cold.find(k);
  if (!ck || std::fabs(ck->vy) > 1e-6f) return fail("with the boiler gone the engines should be cold");
  bool any_on = false;
  for (const LivePart& p : ck->parts) any_on |= p.powered && lib.get(p.def)->power < 0;
  if (any_on) return fail("nothing that burns steam should be powered without a boiler");
  (void)split_before;
  // Crew: no bunks, no hands.
  ShipSim crewless = bare_sim(true);
  Design nobunk = ShipSim::sloop_design();
  erase_at(lib, &nobunk, 2, 4);
  place_part(lib, &nobunk, "hull", 2, 4, 0);
  place_part(lib, &nobunk, "hull", 3, 4, 0);
  const std::uint32_t nb = crewless.spawn(nobunk, 0, 0, Team::Posse, false, -1);
  crewless.step_bare();
  int crewed_on = 0;
  for (const LivePart& p : crewless.find(nb)->parts) crewed_on += p.powered && lib.get(p.def)->crew < 0;
  ShipSim relaxed = bare_sim(false);
  const std::uint32_t nr = relaxed.spawn(nobunk, 0, 0, Team::Posse, false, -1);
  relaxed.step_bare();
  int relaxed_on = 0;
  for (const LivePart& p : relaxed.find(nr)->parts) relaxed_on += p.powered && lib.get(p.def)->crew < 0;
  if (crewed_on != 0 || relaxed_on == 0) return fail("crew rule: no bunks should idle crewed parts (unless crew is off)");
  std::cout << "[smoke] shipyard physics OK (row mass " << s.mass << " t, centre " << s.cx
            << " px, inertia " << s.inertia << "; one tick full ahead vy " << sh->vy
            << " px/s = F/m dt; two paddles lit; E/Q spin " << cw.spin << " / " << ccw.spin
            << " rad/s; strafe + astern; damping; boiler loss cools engines; crew rule)\n";
  return 0;
}

// --- 3. Damage and break-up ---------------------------------------------------------------
int smoke_damage() {
  const PartLibrary lib = PartLibrary::defaults();
  ShipSim sim = bare_sim();
  const Design sloop = ShipSim::sloop_design();
  const std::uint32_t id = sim.spawn(sloop, 100, 50, Team::Posse, false, -1, 0.4f);
  Ship* s = sim.find(id);
  s->vx = 30.0f;
  s->spin = 0.5f;
  const float mass0 = s->mass;
  // Where the trim jet at (1,4) sits in the world before the cut.
  float gx = 0, gy = 0, wx0 = 0, wy0 = 0;
  part_center(*lib.get(lib.find("trim_jet")), PartPose{lib.find("trim_jet"), 1, 4, 1}, &gx, &gy);
  s->to_world(gx, gy, &wx0, &wy0);
  const int bunk = find_part(sloop, "bunkhouse", 2, 4);
  const int gang = find_part(sloop, "gangway", 4, 4);
  if (sim.damage_part(id, bunk, 10)) return fail("10 damage should not sink a 50 hp bunkhouse");
  if (sim.find(id)->parts[static_cast<std::size_t>(bunk)].hp != 40) return fail("part hp after a hit");
  if (!sim.damage_part(id, bunk, 100)) return fail("bunkhouse should be destroyed");
  const int gang_now = gang - 1;  // bunk came out ahead of it
  if (!sim.damage_part(id, gang_now, 999)) return fail("gangway should be destroyed");
  if (sim.ships().size() != 4) {
    return fail("cutting bunk + gangway should leave the ship and 3 wrecks (got " + std::to_string(sim.ships().size()) + ")");
  }
  float mass = 0.0f;
  int wrecks = 0;
  bool jet_in_place = false;
  for (const Ship& t : sim.ships()) {
    mass += t.mass;
    if (t.id != id) {
      ++wrecks;
      if (t.team != Team::Derelict || t.ai) return fail("wrecks should be derelict and captainless");
    }
    for (const LivePart& p : t.parts) {
      if (p.x == 1 && p.y == 4) {
        float wx = 0, wy = 0;
        part_center(*lib.get(p.def), PartPose{p.def, p.x, p.y, p.rot}, &gx, &gy);
        t.to_world(gx, gy, &wx, &wy);
        jet_in_place = std::fabs(wx - wx0) < 1e-2f && std::fabs(wy - wy0) < 1e-2f;
      }
    }
  }
  const float lost = lib.get(lib.find("bunkhouse"))->mass + lib.get(lib.find("gangway"))->mass;
  if (!near(mass, mass0 - lost)) return fail("mass should be conserved across the split (minus wreckage shot away)");
  if (!jet_in_place) return fail("a wreck's parts should stay where they were at the break");
  if (sim.splits() != 3 || sim.parts_destroyed() != 2) return fail("split / destroyed counters");
  // Wrecks drift (they kept the hull's velocity + spin).
  std::vector<std::pair<float, float>> at;
  for (const Ship& t : sim.ships()) at.emplace_back(t.x, t.y);
  for (int i = 0; i < 30; ++i) sim.step_bare();
  int drifted = 0;
  for (std::size_t i = 0; i < sim.ships().size(); ++i) {
    drifted += std::hypot(sim.ships()[i].x - at[i].first, sim.ships()[i].y - at[i].second) > 1.0f;
  }
  if (drifted != 4) return fail("every piece should drift after the break");
  // Shields soak shot first, then guns chew parts: a pirate duel.
  ShipSim duel = bare_sim();
  const std::uint32_t posse = duel.spawn(ShipSim::sloop_design(), 0, 0, Team::Posse, true, -1);
  const std::uint32_t pirate = duel.spawn(ShipSim::marauder_design(), 0, -300, Team::Pirates, true, -1, 3.1f);
  const float shield0 = duel.find(posse)->shield;
  bool shield_hit = false;
  for (int i = 0; i < 60 * 40 && duel.parts_destroyed() == 0; ++i) {
    duel.step_bare();
    if (const Ship* p = duel.find(posse)) shield_hit |= p->shield < shield0 - 1.0f;
  }
  if (duel.shots_fired() == 0) return fail("neither captain fired in 40 s");
  if (!shield_hit) return fail("the sloop's shield should soak shot");
  if (duel.parts_destroyed() == 0) return fail("40 s of gunnery destroyed nothing");
  // Remove every part: the ship is gone.
  ShipSim sink = bare_sim();
  const std::uint32_t z = sink.spawn(ShipSim::sloop_design(), 0, 0, Team::Pirates, false, -1);
  while (sink.find(z) && !sink.find(z)->parts.empty()) {
    sink.damage_part(z, 0, 100000);
    // Breaks may make wrecks; keep chewing on the hull that keeps the id.
  }
  if (sink.find(z)) return fail("a ship with no parts should be gone");
  (void)pirate;
  std::cout << "[smoke] shipyard damage OK (hp per part, bunk + gangway shot away -> hull + 3 derelict "
               "wrecks, mass conserved, parts stay put at the break, wrecks drift; AI duel: "
            << duel.shots_fired() << " shot(s), shield soaked, " << duel.parts_destroyed()
            << " part(s) destroyed; a gutted ship sinks)\n";
  return 0;
}

// --- 4. Determinism -------------------------------------------------------------------------
std::uint64_t battle(float nudge) {
  ShipSim sim = bare_sim();
  sim.spawn(ShipSim::sloop_design(), 0, 0, Team::Posse, true, -1);
  sim.spawn(ShipSim::sloop_design(), 120 + nudge, 40, Team::Posse, true, -1);
  sim.spawn(ShipSim::marauder_design(), 400, -350, Team::Pirates, true, -1);
  sim.spawn(ShipSim::marauder_design(), -420, -300, Team::Pirates, true, -1, 1.0f);
  for (int i = 0; i < 60 * 30; ++i) sim.step_bare();
  return sim.hash() ^ (sim.parts_destroyed() << 48);
}

int smoke_determinism() {
  const std::uint64_t a = battle(0.0f), b = battle(0.0f), c = battle(0.5f);
  if (a != b) return fail("the same 30 s battle hashed differently (" + hex(a) + " vs " + hex(b) + ")");
  if (a == c) return fail("nudging a ship half a pixel changed nothing");
  std::cout << "[smoke] shipyard determinism OK (2v2 AI battle, 1800 ticks: " << hex(a)
            << " twice; a 0.5 px nudge gives " << hex(c) << ")\n";
  return 0;
}

// --- 5. Sample ride + Lua -----------------------------------------------------------------
int smoke_ride() {
  TempDir tmp("sky_pirates");
  std::string err;
  if (!write_sky_pirates_sample(tmp.path.string(), kSampleName, &err)) return fail(err);
  plugins::RuntimePlugins rp;
  std::string load_err;
  rp.open(tmp.path.string(), "smoke", [&](const plugin::Diagnostic& d) {
    if (d.level == runtime::LogLevel::Error) load_err += d.text + "; ";
  });
  auto* plug = dynamic_cast<ShipyardPlugin*>(rp.manager().get("ts.shipyard"));
  if (!plug) return fail("ts.shipyard did not load from the sample's project.json " + load_err);
  runtime::PlaySession session;
  rp.attach(session.world());
  if (!session.start_project(tmp.path.string(), &err)) return fail("start: " + err);
  rp.begin(session.world());
  runtime::World& w = session.world();
  ShipSim& sim = plug->sim();
  if (!sim.warnings().empty()) return fail("sample warnings: " + sim.warnings().front());
  auto step = [&](float mx, float my, std::uint32_t buttons, int n) {
    runtime::InputFrame f;
    f.slot(0).connected = true;
    f.slot(0).move_x = mx;
    f.slot(0).move_y = my;
    f.slot(0).buttons = buttons;
    for (int i = 0; i < n; ++i) w.step(f);
  };
  step(0, 0, 0, 2);
  const std::uint32_t mine = sim.ship_of(0);
  if (!mine || sim.ships().size() != 3) return fail("the sample should fly the sloop and two marauders");
  const Ship* s = sim.find(mine);
  if (s->name.find("Dust Devil") == std::string::npos) return fail("the sample flagship should be the sloop");
  step(0, -1, 0, 60);  // W
  s = sim.find(mine);
  const runtime::Actor* cap = w.player(0);
  if (!s || s->vy > -20.0f) return fail("W should drive the sloop ahead");
  if (!cap || !cap->hidden || std::fabs(cap->data.center_x() - s->x) > 0.01f ||
      std::fabs(cap->data.center_y() - s->y) > 0.01f) {
    return fail("the captain should be hidden and pinned to the ship (camera)");
  }
  step(0, 0, runtime::kButtonAction, 30);  // E
  if (sim.find(mine)->spin <= 0.0f) return fail("E should swing her clockwise");
  const std::uint64_t fired0 = sim.shots_fired();
  step(0, 0, runtime::kButtonUse, 30);  // F
  if (sim.shots_fired() <= fired0) return fail("F should fire the sloop's guns");
  {
    bool bang = false;
    for (const auto& ev : w.take_sounds()) bang |= ev.clip == "builtin:gunshot" && ev.positional;
    if (!bang) return fail("the sloop's guns should be heard");
  }
  // Lua: the bounty board pays when a pirate goes down.
  w.take_logs();
  for (const Ship& t : std::vector<Ship>(sim.ships())) {
    if (t.team == Team::Pirates) {
      sim.remove_ship(t.id);
      break;
    }
  }
  step(0, 0, 0, 3);
  bool paid = false, lua_error = false;
  for (const runtime::LogEntry& e : w.take_logs()) {
    paid |= e.text.find("bounty purse now 50") != std::string::npos;
    lua_error |= e.level == runtime::LogLevel::Error;
  }
  if (lua_error || !paid) return fail("the Lua bounty board (ships.list / ships.stats) never paid out");
  // Respawn after the flagship is lost.
  sim.remove_ship(mine);
  step(0, 0, 0, 2);
  if (sim.ship_of(0)) return fail("respawn should wait respawn_seconds");
  step(0, 0, 0, 60 * 5);
  if (!sim.ship_of(0)) return fail("the posse should get a fresh sloop after the respawn delay");
  rp.end();
  session.stop();
  std::cout << "[smoke] shipyard sky-pirates ride OK (plugin from project.json, sloop + 2 marauders, "
               "W/E/F drive / turn / fire, captain pinned for the camera, Lua bounty board paid via "
               "ships.list + ships.stats, respawn after " << sim.sky().respawn << " s)\n";
  return 0;
}

// --- 6. Posse replication -------------------------------------------------------------------
int smoke_mp() {
  TempDir tmp("sky_mp");
  std::string err;
  if (!write_sky_pirates_sample(tmp.path.string(), kSampleName, &err)) return fail(err);
  const std::string dir = tmp.path.string();
  auto sink = [](const plugin::Diagnostic& d) {
    if (d.level == runtime::LogLevel::Error) std::cerr << "[plugins] " << d.text << '\n';
  };
  plugins::RuntimePlugins srp;
  srp.open(dir, "server", sink);
  auto* sp = dynamic_cast<ShipyardPlugin*>(srp.manager().get("ts.shipyard"));
  if (!sp) return fail("ts.shipyard did not load on the server side");
  mp::GameServer server;
  srp.attach(server.world());
  mp::ServerConfig cfg;
  cfg.project_dir = dir;
  cfg.port = 0;
  if (!server.start(std::move(cfg), &err)) return fail("server: " + err);
  srp.begin(server.world());
  plugins::RuntimePlugins crp;
  crp.open(dir, "client", sink);
  auto* cp = dynamic_cast<ShipyardPlugin*>(crp.manager().get("ts.shipyard"));
  if (!cp) return fail("ts.shipyard did not load on the client side");
  mp::GameClient client;
  crp.attach(client.world());
  mp::ClientConfig cc;
  cc.host = "127.0.0.1";
  cc.port = server.port();
  cc.name = "Calamity";
  cc.project_dir = dir;
  if (!client.connect(std::move(cc), &err)) return fail("client: " + err);
  crp.begin(client.world());
  using Clock = std::chrono::steady_clock;
  auto last = Clock::now();
  runtime::PlayerInput in;
  in.connected = true;
  auto pump = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    server.update(dt);
    client.update(dt, in);
  };
  auto wait_for = [&](double s, const std::function<bool()>& done) {
    const auto t0 = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - t0).count() < s) {
      pump();
      if (done()) return true;
    }
    return done();
  };
  ShipSim& ss = sp->sim();
  ShipSim& cs = cp->sim();
  if (!wait_for(8.0, [&] { return client.joined() && cs.ship_of(client.slot()) != 0 && cs.states_received() > 3; })) {
    return fail("the client never got its ship (layout + state)");
  }
  const int slot = client.slot();
  if (!cs.mirror() || cs.my_slot() != slot) return fail("the client's sim should be a mirror that knows its slot");
  // Ships, pose and parts mirror the server.
  if (!wait_for(3.0, [&] { return cs.ships().size() == ss.ships().size(); })) return fail("ship counts differ");
  const std::uint32_t id = ss.ship_of(slot);
  if (!id || cs.ship_of(slot) != id) return fail("the client should see the server's id for its ship");
  // The client's W is flown by the server.
  const float y0 = ss.find(id)->y;
  in.move_y = -1.0f;
  if (!wait_for(4.0, [&] { const Ship* s = ss.find(id); return s && s->y < y0 - 40.0f; })) {
    return fail("the client's W never moved its ship on the server");
  }
  in.move_y = 0.0f;
  if (!wait_for(3.0, [&] {
        const Ship* a = ss.find(id);
        const Ship* b = cs.find(id);
        return a && b && std::fabs(a->y - b->y) < 30.0f && b->y < y0 - 40.0f;
      })) {
    return fail("the server's pose never reached the client");
  }
  // Part damage replicates, then a break-up.
  ss.damage_part(id, 0, 25);
  if (!wait_for(3.0, [&] { const Ship* b = cs.find(id); return b && b->parts[0].hp == ss.find(id)->parts[0].hp; })) {
    return fail("part damage never replicated");
  }
  const Design sloop = ShipSim::sloop_design();
  ss.damage_part(id, find_part(sloop, "bunkhouse", 2, 4), 99999);
  ss.damage_part(id, find_part(sloop, "gangway", 4, 4) - 1, 99999);
  const std::size_t want = ss.ships().size();
  if (!wait_for(4.0, [&] {
        const Ship* b = cs.find(id);
        return cs.ships().size() == want && b && b->parts.size() == ss.find(id)->parts.size();
      })) {
    return fail("the break-up (wrecks + shorter hull) never replicated");
  }
  // Clients cannot hurt ships on their own.
  if (cs.damage_part(id, 0, 999)) return fail("a client mirror accepted damage");
  std::cout << "[smoke] shipyard posse OK (ts_server + ts_client over ENet loopback: layouts + "
            << cs.states_received() << " state message(s), client W flown by the server, pose mirrored, "
               "part damage and a 3-wreck break-up replicated, client can't deal damage)\n";
  client.disconnect();
  for (int i = 0; i < 20; ++i) pump();
  crp.end();
  srp.end();
  server.stop();
  return 0;
}

}  // namespace

int run_shipyard_smoke() {
  std::cout << "[smoke] Airship Works (ts.shipyard)\n";
  if (smoke_design() != 0) return 1;
  if (smoke_physics() != 0) return 1;
  if (smoke_damage() != 0) return 1;
  if (smoke_determinism() != 0) return 1;
  if (smoke_ride() != 0) return 1;
  if (smoke_mp() != 0) return 1;
  std::cout << "[smoke] Airship Works OK\n";
  return 0;
}
