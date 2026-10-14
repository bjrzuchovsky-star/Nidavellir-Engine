#include "SmokeSurvival.h"

#include "mp/GameClient.h"
#include "mp/GameServer.h"
#include "plugins/RuntimePlugins.h"
#include "plugins/survival/ChunkMap.h"
#include "plugins/survival/Sample.h"
#include "plugins/survival/SurvivalPlugin.h"
#include "plugins/survival/WorldGen.h"
#include "runtime/Collision.h"
#include "runtime/Input.h"
#include "runtime/Log.h"
#include "runtime/PlaySession.h"
#include "runtime/World.h"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace ts::tombstone;
using namespace ts::tombstone::survival;
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
  std::cerr << "[smoke] survival FAILED: " << what << '\n';
  return 1;
}

std::string hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%016" PRIx64, v);
  return b;
}

std::uint64_t region_hash(std::uint64_t seed) {
  const WorldGen gen(seed);
  std::uint64_t h = 1469598103934665603ull;
  std::array<Tile, kChunk * kChunk> tiles{};
  for (int cy = -3; cy <= 3; ++cy) {
    for (int cx = -3; cx <= 3; ++cx) {
      gen.generate(cx, cy, tiles.data());
      h = (h ^ chunk_hash(tiles.data())) * 1099511628211ull;
    }
  }
  return h;
}

// --- 1. Worldgen -------------------------------------------------------------------------
int smoke_worldgen() {
  const std::uint64_t a = region_hash(1849);
  const std::uint64_t b = region_hash(1849);
  const std::uint64_t c = region_hash(1850);
  if (a != b) return fail("worldgen is not deterministic (same seed, different tiles)");
  if (a == c) return fail("seeds 1849 and 1850 made the same world");
  // tile_at agrees with generate (chunk edges, negative coordinates).
  const WorldGen gen(1849);
  std::array<Tile, kChunk * kChunk> tiles{};
  gen.generate(-1, -2, tiles.data());
  for (int ly = 0; ly < kChunk; ly += 7) {
    for (int lx = 0; lx < kChunk; lx += 5) {
      if (!(gen.tile_at(-kChunk + lx, -2 * kChunk + ly) == tiles[static_cast<std::size_t>(ly * kChunk + lx)])) {
        return fail("tile_at disagrees with generate at a negative chunk");
      }
    }
  }
  std::array<int, kBiomeCount> biomes{};
  int ores = 0, trees = 0, cacti = 0;
  for (int y = -400; y < 400; y += 3) {
    for (int x = -400; x < 400; x += 3) {
      ++biomes[static_cast<std::size_t>(gen.biome_at(x, y))];
      const Tile t = gen.tile_at(x, y);
      ores += t.object >= kCopperOre && t.object <= kGoldOre;
      trees += t.object == kTree;
      cacti += t.object == kCactus;
    }
  }
  for (int i = 0; i < kBiomeCount; ++i) {
    if (biomes[static_cast<std::size_t>(i)] == 0) return fail("biome " + std::to_string(i) + " never appears");
  }
  if (!ores || !trees || !cacti) return fail("no ores / trees / cacti in an 800x800 tile survey");
  for (int y = -WorldGen::kSpawnClear; y <= WorldGen::kSpawnClear; ++y) {
    for (int x = -WorldGen::kSpawnClear; x <= WorldGen::kSpawnClear; ++x) {
      if (tile_solid(gen.tile_at(x, y))) return fail("the spawn clearing has a solid tile");
    }
  }
  std::cout << "[smoke] survival worldgen OK (seed 1849 region hash " << hex(a)
            << ", deterministic, seed 1850 differs; prairie/desert/badlands/caves "
            << biomes[0] << "/" << biomes[1] << "/" << biomes[2] << "/" << biomes[3]
            << " samples, " << ores << " ore, " << trees << " trees, " << cacti
            << " cacti, spawn clear)\n";
  return 0;
}

// --- 2. Chunk persistence ----------------------------------------------------------------
int smoke_chunks() {
  TempDir tmp("frontier_chunks");
  const std::string dir = (tmp.path / "saves" / "frontier").string();
  ChunkMap m;
  m.reset(1849, dir);
  m.stream({{0.0f, 0.0f}});
  if (m.loaded() != 25) return fail("streaming radius 2 should load 25 chunks, got " + std::to_string(m.loaded()));
  Tile wall{kGrass, kWoodWall, 0};
  Tile floor{kPlanks, kObjNone, 0};
  m.set(3, 4, wall);
  m.set(-40, -70, floor);  // far chunk, loads on demand
  m.set(100, 5, wall);
  if (m.take_edits().size() != 3) return fail("three edits should be recorded");
  // Ride away: the edited chunks near the origin unload and get written.
  m.stream({{5000.0f * kTile / 32.0f * 32.0f, 0.0f}});
  m.save_all();
  const std::size_t written = m.written();
  if (written < 3) return fail("edited chunks should be written, wrote " + std::to_string(written));
  if (!fs::exists(m.chunk_path(0, 0))) return fail("no chunk file at " + m.chunk_path(0, 0));
  ChunkMap again;
  again.reset(1849, dir);
  if (!(again.get(3, 4) == wall) || !(again.get(-40, -70) == floor) || !(again.get(100, 5) == wall)) {
    return fail("edits did not survive a save / load");
  }
  const WorldGen gen(1849);
  if (!(again.get(7, 9) == gen.tile_at(7, 9))) return fail("unedited tile changed after reload");
  if (again.read_from_disk() < 3) return fail("chunks should come back from disk");
  // Wire / file codec round trip.
  const Chunk* c = again.find(0, 0);
  std::uint64_t seed = 0;
  Chunk back;
  if (!c || !ChunkMap::decode(ChunkMap::encode(*c, 1849), &back, &seed) || seed != 1849 ||
      back.tiles != c->tiles || !back.edited) {
    return fail("chunk encode / decode round trip");
  }
  // A different seed ignores the old files.
  ChunkMap other;
  other.reset(77, dir);
  if (other.get(3, 4) == wall && WorldGen(77).tile_at(3, 4).object != kWoodWall) {
    return fail("a save from seed 1849 leaked into seed 77");
  }
  std::cout << "[smoke] survival chunks OK (25 streamed, " << written
            << " chunk file(s) written, edits back after reload incl. negative coords, codec "
               "round trip, seed-mismatched files ignored)\n";
  return 0;
}

// A ride in a fresh copy of the sample, plugins as in ts_game.
struct Ride {
  TempDir tmp{"frontier_ride"};
  plugins::RuntimePlugins rp;
  runtime::PlaySession session;
  SurvivalPlugin* plugin = nullptr;
  std::string error;

  bool start() {
    if (!write_frontier_sample(tmp.path.string(), kSampleName, &error)) return false;
    rp.open(tmp.path.string(), "smoke", [this](const plugin::Diagnostic& d) {
      if (d.level == runtime::LogLevel::Error) error += d.text + "; ";
    });
    plugin = dynamic_cast<SurvivalPlugin*>(rp.manager().get("ts.frontier"));
    if (!plugin) {
      error = "ts.frontier did not load from the sample's project.json " + error;
      return false;
    }
    plugin->set_saving(false);
    rp.attach(session.world());
    if (!session.start_project(tmp.path.string(), &error)) return false;
    rp.begin(session.world());
    return true;
  }
  ~Ride() {
    rp.end();
    session.stop();
  }
  runtime::World& world() { return session.world(); }
  FrontierSim& sim() { return plugin->sim(); }
  void step(std::uint32_t buttons = 0, float mx = 0.0f, float my = 0.0f, int n = 1) {
    runtime::InputFrame f;
    f.slot(0).connected = true;
    f.slot(0).buttons = buttons;
    f.slot(0).move_x = mx;
    f.slot(0).move_y = my;
    for (int i = 0; i < n; ++i) world().step(f);
  }
  int item(const char* id) { return sim().data().item(id); }
  int count(const char* id) { return sim().player(0).inv.count(item(id)); }
  int slot_of(const char* id) {
    for (int i = 0; i < Inventory::kHotbar; ++i) {
      if (sim().player(0).inv.at(i).item == item(id)) return i;
    }
    return -1;
  }
  void hold(const char* id) {
    int s = slot_of(id);
    if (s < 0) {
      // Put it on the hotbar.
      for (int i = Inventory::kHotbar; i < Inventory::kSlots; ++i) {
        if (sim().player(0).inv.at(i).item == item(id)) {
          sim().swap(world(), 0, i, 9);
          s = 9;
          break;
        }
      }
    }
    if (s >= 0) sim().select(world(), 0, s);
  }
};

// --- 3. Saddlebags, crafting, mining, placing, Lua -----------------------------------------
int smoke_craft() {
  Ride r;
  if (!r.start()) return fail("ride: " + r.error);
  if (!r.sim().active() || !r.world().player(0)) return fail("no rider / sim in the sample ride");
  // Starter kit.
  if (r.count("torch") != 3) return fail("a fresh claim should start with 3 torches");
  // Lua: ride right into the supply crate (frontier.give from scripts/).
  bool crate = false;
  for (int i = 0; i < 240 && !crate; ++i) {
    r.step(0, 1.0f, 0.0f);
    crate = r.count("wood") >= 12;
  }
  if (!crate) return fail("the supply crate script (frontier.give) never paid out");
  if (r.count("stone") != 6 || r.count("jerky") != 2) return fail("crate kit counts wrong");
  r.step(0, 0.0f, 0.0f, 2);
  // Craft by hand.
  std::string why;
  const int bench = r.sim().data().recipe("workbench");
  if (!r.sim().craft(r.world(), 0, bench, &why)) return fail("workbench craft: " + why);
  if (r.count("wood") != 6 || r.count("workbench") != 1) return fail("workbench should cost 6 logs");
  const int pick = r.sim().data().recipe("stone_pick");
  if (r.sim().can_craft(0, pick, &why) || why.find("Workbench") == std::string::npos) {
    return fail("stone pick should need a workbench close by (got: " + why + ")");
  }
  // Place the workbench in front (G) and craft at it.
  r.hold("workbench");
  r.step(0, 0.0f, 0.0f);
  const int tx = r.sim().player(0).target_tx, ty = r.sim().player(0).target_ty;
  Tile clear = r.sim().tile(tx, ty);
  clear.object = kObjNone;
  r.sim().set_tile(tx, ty, clear);
  r.step(runtime::kButtonPlace);
  r.step(0);
  if (r.sim().tile(tx, ty).object != kWorkbench || r.count("workbench") != 0) {
    return fail("placing the workbench (G) did not land it in front of the rider");
  }
  if (!r.sim().craft(r.world(), 0, pick, &why)) return fail("stone pick at the workbench: " + why);
  // Mine a boulder for stone with the pick (F held).
  r.hold("stone_pick");
  Tile rock = r.sim().tile(tx, ty);
  rock.object = kBoulder;
  rock.damage = 0;
  r.sim().set_tile(tx, ty, rock);
  const int stone0 = r.count("stone");
  int swings = 0;
  for (int i = 0; i < 600 && r.sim().tile(tx, ty).object == kBoulder; ++i, ++swings) {
    r.step(runtime::kButtonUse);
  }
  if (r.sim().tile(tx, ty).object != kObjNone || r.count("stone") <= stone0) {
    return fail("the stone pick never broke the boulder");
  }
  // Gold ore needs a better pick.
  Tile gold = r.sim().tile(tx, ty);
  gold.object = kGoldOre;
  r.sim().set_tile(tx, ty, gold);
  r.step(runtime::kButtonUse, 0, 0, 180);
  if (r.sim().tile(tx, ty).object != kGoldOre) return fail("a stone pick should not crack gold ore");
  // Saddlebags: overflow and swaps.
  Inventory inv;
  const GameData& d = r.sim().data();
  const int left = inv.add(d, r.item("stone_pick"), Inventory::kSlots + 3);
  if (left != 3) return fail("a full inventory should hand back the leftover");
  inv.clear();
  inv.add(d, r.item("wood"), 150);
  if (inv.at(0).count != 99 || inv.at(1).count != 51) return fail("stacks should cap at 99");
  inv.swap(0, 5);
  if (inv.at(5).count != 99 || !inv.at(0).empty()) return fail("swap");
  // Hotbar keys.
  r.sim().select(r.world(), 0, 2);
  r.step(runtime::kButtonNext);
  r.step(0);
  r.step(runtime::kButtonPrev);
  r.step(0);
  r.step(runtime::kButtonPrev);
  if (r.sim().player(0).hotbar != 1) return fail("Q / X hotbar cycling");
  // Housing: a 3x3 room of walls with a door, bed inside.
  const int hx = tx + 6, hy = ty;
  for (int y = -2; y <= 2; ++y) {
    for (int x = -2; x <= 2; ++x) {
      Tile t{kPlanks, kObjNone, 0};
      if (std::abs(x) == 2 || std::abs(y) == 2) t.object = (x == 0 && y == 2) ? kDoor : kWoodWall;
      r.sim().set_tile(hx + x, hy + y, t);
    }
  }
  const HouseCheck house = r.sim().check_house(hx, hy);
  if (!house.ok || house.area != 9 || house.doors != 1) {
    return fail("housing check on a closed 3x3 room with a door: " + house.why);
  }
  Tile open = r.sim().tile(hx + 2, hy);
  open.object = kObjNone;
  r.sim().set_tile(hx + 2, hy, open);
  if (r.sim().check_house(hx, hy).ok) return fail("a room with a hole should not house a settler");
  std::cout << "[smoke] survival craft OK (Lua supply crate paid out, workbench by hand, placed "
               "with G, stone pick at it, boulder mined in "
            << swings << " ticks, gold ore refused, stacks/overflow/swap, Q/X hotbar, housing "
               "3x3 + door ok / holed room refused)\n";
  return 0;
}

// --- 4. Combat -------------------------------------------------------------------------------
int smoke_combat() {
  Ride r;
  if (!r.start()) return fail("ride: " + r.error);
  r.step(0, 1.0f, 0.0f, 3);  // face right
  r.step(0, 0.0f, 0.0f, 2);
  const runtime::Actor* a = r.world().player(0);
  const runtime::Aabb b = runtime::collider_box(a->data);
  const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
  const int coyote = r.sim().data().creature("coyote");
  // A coyote at the rider's elbow bites.
  r.sim().spawn(coyote, cx + 26.0f, cy);
  const int hp0 = r.sim().player(0).hp;
  r.step(0, 0, 0, 120);
  const int bitten = hp0 - r.sim().player(0).hp;
  if (bitten <= 0) return fail("a coyote next to the rider should bite");
  // Knife it.
  r.sim().give(0, r.item("knife"), 1);
  r.hold("knife");
  const std::uint64_t kills0 = r.sim().kills();
  for (int i = 0; i < 400 && r.sim().kills() == kills0; ++i) {
    // Face whatever is closest each tick (it moves).
    float dx = 1.0f;
    for (const FrontierSim::Mob& m : r.sim().mobs()) {
      if (!m.settler) dx = m.x < cx ? -1.0f : 1.0f;
    }
    r.step(runtime::kButtonUse, i % 30 == 0 ? dx * 0.2f : 0.0f, 0.0f);
  }
  if (r.sim().kills() == kills0) return fail("the knife never put the coyote down");
  if (r.count("hide") < 1) return fail("a coyote should drop rawhide");
  // Ranged: a bow and arrows, a coyote four tiles to the right.
  for (int i = 0; i < 4; ++i) r.step(0, 1.0f, 0.0f);
  r.step(0, 0, 0, 1);
  r.sim().give(0, r.item("bow"), 1);
  r.sim().give(0, r.item("arrow"), 5);
  r.hold("bow");
  const runtime::Aabb b2 = runtime::collider_box(r.world().player(0)->data);
  const std::uint32_t id = r.sim().spawn(coyote, b2.x1 + 4.0f * kTile, (b2.y0 + b2.y1) * 0.5f);
  const int arrows0 = r.count("arrow");
  int hit_hp = -1;
  for (int i = 0; i < 60; ++i) {
    r.step(i < 30 ? runtime::kButtonFire : 0);
    for (const FrontierSim::Mob& m : r.sim().mobs()) {
      if (m.id == id) hit_hp = m.hp;
    }
  }
  const bool dead = [&] {
    for (const FrontierSim::Mob& m : r.sim().mobs()) {
      if (m.id == id) return false;
    }
    return true;
  }();
  if (r.count("arrow") >= arrows0) return fail("firing (C) should spend an arrow");
  {
    // Footsteps, knife / pick knocks and the shot are all heard.
    bool step = false, knock = false, shot = false;
    for (const auto& ev : r.world().take_sounds()) {
      knock |= ev.clip == "builtin:mine";
      shot |= ev.clip == "builtin:gunshot" && ev.positional;
    }
    for (int i = 0; i < 40; ++i) r.step(0, i < 20 ? 1.0f : -1.0f, 0.0f);
    for (const auto& ev : r.world().take_sounds()) step |= ev.clip == "builtin:step";
    if (!step || !knock || !shot) {
      return fail(std::string("frontier sounds: step ") + (step ? "y" : "n") + " knock " +
                  (knock ? "y" : "n") + " shot " + (shot ? "y" : "n"));
    }
  }
  if (!dead && hit_hp >= r.sim().data().creatures[static_cast<std::size_t>(coyote)].hp) {
    return fail("the arrow never hit the coyote");
  }
  // Death and respawn.
  const int deaths0 = r.sim().player(0).deaths;
  r.sim().hurt_player(r.world(), 0, 1000, "a smoke test");
  if (r.sim().player(0).deaths != deaths0 + 1 || r.sim().player(0).hp != FrontierSim::kMaxHp) {
    return fail("dying should respawn the rider at full grit");
  }
  // Eat (F with food selected).
  r.sim().hurt_player(r.world(), 0, 40, "a smoke test");
  r.sim().give(0, r.item("jerky"), 1);
  r.hold("jerky");
  r.step(0, 0, 0, 40);
  const int before = r.sim().player(0).hp;
  r.step(runtime::kButtonUse);
  if (r.sim().player(0).hp <= before) return fail("eating jerky should heal");
  // Night spawns: run the clock with spawning on, something hostile shows.
  r.step(0, 0, 0, 60 * 12);
  std::cout << "[smoke] survival combat OK (coyote bit for " << bitten
            << " grit, knifed, rawhide dropped, bow shot spent an arrow and "
            << (dead ? "killed" : "hit") << " a coyote, death respawns, jerky heals, "
            << r.sim().spawned_total() << " creature(s) spawned by biome so far)\n";
  return 0;
}

// --- 5. Posse replication ---------------------------------------------------------------------
int smoke_mp() {
  TempDir tmp("frontier_mp");
  std::string err;
  if (!write_frontier_sample(tmp.path.string(), kSampleName, &err)) return fail(err);
  const std::string dir = tmp.path.string();
  auto sink = [](const plugin::Diagnostic& d) {
    if (d.level == runtime::LogLevel::Error) std::cerr << "[plugins] " << d.text << '\n';
  };
  plugins::RuntimePlugins srp;
  srp.open(dir, "server", sink);
  auto* sp = dynamic_cast<SurvivalPlugin*>(srp.manager().get("ts.frontier"));
  if (!sp) return fail("ts.frontier did not load on the server side");
  sp->set_saving(false);
  mp::GameServer server;
  srp.attach(server.world());
  mp::ServerConfig cfg;
  cfg.project_dir = dir;
  cfg.port = 0;
  if (!server.start(std::move(cfg), &err)) return fail("server: " + err);
  srp.begin(server.world());

  struct Rider {
    plugins::RuntimePlugins rp;
    mp::GameClient client;
    SurvivalPlugin* plugin = nullptr;
  };
  auto join = [&](const char* name, Rider* r) -> bool {
    r->rp.open(dir, "client", sink);
    r->plugin = dynamic_cast<SurvivalPlugin*>(r->rp.manager().get("ts.frontier"));
    if (!r->plugin) return false;
    r->rp.attach(r->client.world());
    mp::ClientConfig cc;
    cc.host = "127.0.0.1";
    cc.port = server.port();
    cc.name = name;
    cc.project_dir = dir;
    if (!r->client.connect(std::move(cc), &err)) return false;
    r->rp.begin(r->client.world());
    return true;
  };
  auto first = std::make_unique<Rider>();
  if (!join("Ike", first.get())) return fail("client one: " + err);
  std::unique_ptr<Rider> second;
  using Clock = std::chrono::steady_clock;
  auto last = Clock::now();
  std::uint32_t buttons = 0;
  auto pump = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    server.update(dt);
    runtime::PlayerInput in;
    in.connected = true;
    in.buttons = buttons;
    first->client.update(dt, in);
    if (second) second->client.update(dt, runtime::PlayerInput{});
  };
  auto wait_for = [&](double s, const std::function<bool()>& done) {
    const auto t0 = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - t0).count() < s) {
      pump();
      if (done()) return true;
    }
    return done();
  };
  FrontierSim& ss = sp->sim();
  FrontierSim& cs = first->plugin->sim();
  if (!wait_for(8.0, [&] { return first->client.joined() && cs.has_seed(); })) {
    return fail("client never got the frontier Welcome (seed)");
  }
  if (!cs.mirror()) return fail("the client's sim should be a mirror");
  if (cs.chunks().seed() != ss.chunks().seed()) return fail("client seed differs from server");
  const int slot = first->client.slot();
  if (cs.my_slot() != slot) return fail("frontier Welcome slot mismatch");
  // Server-side edits near the rider replicate.
  const runtime::Actor* ra = server.world().player(slot);
  if (!ra) return fail("no server rider");
  const int rtx = static_cast<int>(std::floor(ra->data.center_x() / kTile));
  const int rty = static_cast<int>(std::floor(ra->data.center_y() / kTile));
  const Tile wall{kGrass, kWoodWall, 0};
  ss.set_tile(rtx + 3, rty, wall);
  ss.set_tile(rtx + 3, rty + 1, Tile{kPlanks, kTorch, 0});
  ss.set_tile(rtx - 4, rty - 2, Tile{kSand, kObjNone, 0});
  if (!wait_for(4.0, [&] {
        return cs.tile(rtx + 3, rty) == wall && cs.tile(rtx + 3, rty + 1).object == kTorch &&
               cs.tile(rtx - 4, rty - 2).ground == kSand;
      })) {
    return fail("server tile edits never reached the client");
  }
  // Inventory is the server's: a give shows up on the client.
  const int wood = ss.data().item("wood");
  ss.give(slot, wood, 10);
  if (!wait_for(4.0, [&] { return cs.player(slot).inv.count(wood) >= 10; })) {
    return fail("server inventory never reached the client");
  }
  // A craft request from the client is done by the server.
  const int bench = ss.data().item("workbench");
  cs.craft(first->client.world(), slot, ss.data().recipe("workbench"));
  if (!wait_for(4.0, [&] { return ss.player(slot).inv.count(bench) == 1 &&
                                  cs.player(slot).inv.count(bench) == 1; })) {
    return fail("client craft request was not honoured by the server");
  }
  // The client cannot edit the world directly.
  if (cs.set_tile(rtx, rty + 5, wall)) return fail("a client mirror accepted a direct tile edit");
  // Mining through the client's input (F): break a cactus in front.
  ss.set_tile(ss.player(slot).target_tx, ss.player(slot).target_ty, Tile{kSand, kCactus, 0});
  const int ttx = ss.player(slot).target_tx, tty = ss.player(slot).target_ty;
  if (!wait_for(3.0, [&] { return cs.tile(ttx, tty).object == kCactus; })) {
    return fail("cactus never showed on the client");
  }
  buttons = runtime::kButtonUse;
  const bool mined = wait_for(8.0, [&] { return cs.tile(ttx, tty).object == kObjNone; });
  buttons = 0;
  if (!mined) return fail("mining with the client's F never broke the cactus (server-authoritative)");
  {
    // The server's mining knock rides to the client as a sound event.
    bool knock = false;
    wait_for(2.0, [&] {
      for (const auto& ev : first->client.world().take_sounds()) {
        knock |= ev.clip == "builtin:mine" && ev.positional;
      }
      return knock;
    });
    if (!knock) return fail("the mining sound never reached the client");
  }
  // A late rider gets the edited chunks.
  second = std::make_unique<Rider>();
  if (!join("Wyatt", second.get())) return fail("client two: " + err);
  FrontierSim& c2 = second->plugin->sim();
  if (!wait_for(8.0, [&] { return second->client.joined() && c2.has_seed() &&
                                  c2.tile(rtx + 3, rty) == wall; })) {
    return fail("a late joiner never received the edited chunk");
  }
  // State: creatures replicate.
  ss.spawn(ss.data().creature("coyote"), ra->data.center_x() + 6 * kTile, ra->data.center_y());
  if (!wait_for(3.0, [&] { return !cs.mobs().empty(); })) return fail("creature state never replicated");
  std::cout << "[smoke] survival posse OK (ts_server + ts_client plugin hosts over ENet loopback: "
               "seed via Welcome, " << ss.edits_sent() << " tile edit(s) sent / "
            << cs.edits_received() << " applied, inventory server-side, craft request honoured, "
               "client F mined a cactus, late joiner got the edited chunk, "
            << cs.mobs().size() << " creature(s) mirrored)\n";
  first->client.disconnect();
  second->client.disconnect();
  for (int i = 0; i < 20; ++i) pump();
  first->rp.end();
  second->rp.end();
  srp.end();
  server.stop();
  return 0;
}

}  // namespace

int run_survival_smoke() {
  std::cout << "[smoke] Frontier Survival (ts.frontier)\n";
  if (smoke_worldgen() != 0) return 1;
  if (smoke_chunks() != 0) return 1;
  if (smoke_craft() != 0) return 1;
  if (smoke_combat() != 0) return 1;
  if (smoke_mp() != 0) return 1;
  std::cout << "[smoke] survival OK\n";
  return 0;
}
