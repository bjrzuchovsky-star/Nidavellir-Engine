// --smoke-ai, part 2: the ts.ai plugin riding a 2D World (agents, BT file,
// walls, hearing, Lua table + bridge, client role, Frontier coyote steering).
// The 3D half (ts_ai3d) appends to this file's run via run_ai3d_smoke().

#include "SmokeAI.h"

#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "plugins/ai/AiPlugin.h"
#include "ai3d/AiWorld3D.h"
#include "render3d/Scene3D.h"
#include "runtime/GraphScript.h"
#include "runtime/World.h"
#include "world3d/World3D.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ai = ts::ai;
using namespace ts::tombstone;

namespace {

namespace fs = std::filesystem;

int fail(const std::string& why) {
  std::cerr << "[smoke] ai world FAILED: " << why << "\n";
  return 1;
}

Entity2D box(std::uint64_t id, const char* name, float x, float y, float w, float h, bool solid) {
  Entity2D e;
  e.id = id;
  e.name = name;
  e.x = x;
  e.y = y;
  e.w = w;
  e.h = h;
  if (solid) {
    ColliderData c;
    c.w = w;
    c.h = h;
    e.collider = c;
  }
  return e;
}

}  // namespace

int run_ai3d_smoke();

int run_ai_world_smoke() {
  const fs::path dir = fs::temp_directory_path() / "ts_ai_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "ai", ec);
  {
    std::ofstream f(dir / "ai" / "goto.bt.json");
    f << R"({"blackboard": {"goal": [560, 0, 120]},
  "root": {"type": "sequence", "children": [
    {"type": "move_to", "key": "goal"},
    {"type": "set", "key": "arrived", "value": true},
    {"type": "running"}]}})";
  }

  plugin::Catalog cat;
  plugins::register_builtin_plugins(cat, nullptr);
  if (!cat.find("ts.ai")) return fail("ts.ai should be a built-in plugin");
  plugin::PluginManager m(cat);
  plugin::PluginContext ctx;
  m.load({"ts.ai"}, ctx);
  if (!m.loaded("ts.ai")) return fail("ts.ai should load");
  auto* aip = static_cast<plugins::AiPlugin*>(m.get("ts.ai"));

  // A wall at x 300..332 from y 0 to 360: the way round is under it.
  runtime::World w;
  w.set_extensions(m.world_extensions());
  std::vector<Entity2D> ents;
  ents.push_back(box(1, "Wall", 300, -200, 32, 560, true));
  ents.push_back(box(2, "Guard", 100 - 12, 120 - 12, 24, 24, false));
  ents.push_back(box(3, "Guard Two", 140 - 12, 120 - 12, 24, 24, false));
  ents.push_back(box(4, "Listener", 100 - 12, 600 - 12, 24, 24, false));
  Entity2D probe = box(5, "Probe", 0, 900, 8, 8, false);
  probe.script = ScriptData{};
  probe.script->path = "scripts/probe.lua";
  ents.push_back(probe);
  w.script_library().put("scripts/probe.lua",
                         "function on_start()\n"
                         "  ai.agent(find('Guard Two'), {speed = 120, radius = 12})\n"
                         "  ai.move_to(find('Guard Two'), 560, 160)\n"
                         "  log('n=' .. ai.count() .. ' len=' .. math.floor(ai.path_length(100, 120, 560, 120)))\n"
                         "end\n");
  // A graph-scripted agent: start -> ai.agent -> ai.move_to (0 = self).
  {
    runtime::Graph g;
    auto node = [&g](const std::string& type, std::map<std::string, ScriptValue> in) {
      runtime::GraphNode n;
      n.id = g.next_id();
      n.type = type;
      n.inputs = std::move(in);
      g.nodes.push_back(n);
      return n.id;
    };
    const int start = node("event.start", {});
    const int mk = node("ai.agent", {{"speed", ScriptValue::of_number(140)}});
    const int go = node("ai.move_to", {{"x", ScriptValue::of_number(560)}, {"y", ScriptValue::of_number(220)}});
    g.links.push_back({start, "out", mk, "in"});
    g.links.push_back({mk, "out", go, "in"});
    w.script_library().put("scripts/runner.graph.json", runtime::write_graph(g));
    Entity2D runner = box(6, "Graph Runner", 100 - 12, 220 - 12, 24, 24, false);
    runner.script = ScriptData{};
    runner.script->path = "scripts/runner.graph.json";
    ents.push_back(runner);
  }
  std::string err;
  if (!w.build(ents, dir.string(), &err)) return fail("world build: " + err);
  m.play_begin(w);

  ai::AgentParams p;
  p.speed = 150.0f;
  p.radius = 12.0f;
  if (!aip->add_agent(w, 2, p)) return fail("add agent");
  if (!aip->load_tree(w, 2, "ai/goto.bt.json", &err)) return fail("bt load: " + err);
  ai::PerceptionConfig ears;
  ears.hearing_range = 300.0f;
  aip->add_agent(w, 4, p, ears);

  std::vector<ai::V3> path;
  if (!aip->find_path(w, 100, 120, 560, 120, &path)) return fail("path round the wall");
  float len = 0.0f;
  for (std::size_t i = 1; i < path.size(); ++i) len += ai::distance_xz(path[i - 1], path[i]);
  // Straight line is 460; down under the wall end (y 360+) and back.
  if (len < 700.0f || len > 1000.0f) return fail("path length round the wall " + std::to_string(len));

  runtime::InputFrame input;
  float min_gap = 1e9f;
  bool in_wall = false;
  bool heard_ok = false;
  for (int i = 0; i < 60 * 12; ++i) {
    w.step(input);
    const runtime::Actor* g = w.find(2);
    const runtime::Actor* g2 = w.find(3);
    const float gx = g->data.x + 12, gy = g->data.y + 12;
    if (gx + 10 > 300 && gx - 10 < 332 && gy - 10 < 360) in_wall = true;
    min_gap = std::min(min_gap, std::hypot(gx - (g2->data.x + 12), gy - (g2->data.y + 12)) - 24.0f);
    if (i == 30) {
      // A gunshot near the listener: positional sound -> hearing.
      runtime::SoundEvent shot;
      shot.clip = "builtin:gunshot";
      shot.positional = true;
      shot.x = 180;
      shot.y = 640;
      w.play_sound(shot);
    }
    if (i == 40) {
      const auto* la = aip->agent(w, 4);
      const ai::Percept* h = la->brain->perception().last_heard(w.time_seconds(), 2.0);
      heard_ok = h && std::fabs(h->pos.x - 180) < 1 && la->brain->blackboard().text("noise") == "builtin:gunshot";
    }
  }
  const runtime::Actor* g = w.find(2);
  const float gx = g->data.x + 12, gy = g->data.y + 12;
  if (std::hypot(gx - 560, gy - 120) > 16.0f) {
    return fail("BT guard should reach the goal round the wall (at " + std::to_string(gx) + "," + std::to_string(gy) + ")");
  }
  if (in_wall) return fail("guard cut through the wall");
  const auto* ga = aip->agent(w, 2);
  if (!ga->brain->blackboard().flag("arrived")) return fail("bt should set arrived after move_to");
  if (ga->brain->tree()->running_path().find("running") == std::string::npos) {
    return fail("bt live state: " + ga->brain->tree()->running_path());
  }
  const runtime::Actor* g2 = w.find(3);
  if (std::hypot(g2->data.x + 12 - 560, g2->data.y + 12 - 160) > 16.0f) return fail("Lua move_to agent should arrive");
  const runtime::Actor* gr = w.find(6);
  if (!gr || std::hypot(gr->data.x + 12 - 560, gr->data.y + 12 - 220) > 16.0f) return fail("graph ai.agent + ai.move_to runner should arrive");
  if (min_gap < -2.0f) return fail("agents overlapped (gap " + std::to_string(min_gap) + ")");
  if (!heard_ok) return fail("listener should hear the positional gunshot");
  bool lua_ok = false;
  std::string last;
  for (const auto& l : w.take_logs()) {
    if (l.text.find("n=") != std::string::npos) last = l.text;
    lua_ok |= l.text.find(" len=7") != std::string::npos;
  }
  if (!lua_ok) return fail("Lua ai table: " + last);
  // Graph / C++ / C# reach the same table through the bridge.
  ScriptValue out;
  if (!w.script_plugin_call("ai", "path_length",
                            {ScriptValue::of_number(100), ScriptValue::of_number(120), ScriptValue::of_number(560),
                             ScriptValue::of_number(120)},
                            &out, &err) ||
      out.number < 700.0) {
    return fail("bridge ai.path_length: " + err);
  }
  m.play_end();
  w.clear();

  // Client worlds never run AI (server-side only).
  runtime::World cw;
  cw.set_net_role(runtime::NetRole::Client);
  cw.set_extensions(m.world_extensions());
  if (!cw.build({box(2, "Guard", 0, 0, 24, 24, false)}, dir.string(), &err)) return fail("client build");
  m.play_begin(cw);
  if (aip->add_agent(cw, 2, p) || aip->active(cw)) return fail("client world should not take agents");
  m.play_end();
  cw.set_extensions(nullptr);
  w.set_extensions(nullptr);

  // Frontier coyote steering: a wall between coyote and rider.
  runtime::World fw;
  fw.set_extensions(m.world_extensions());
  fw.build({}, dir.string(), &err);
  m.play_begin(fw);
  const auto walk = [](float x, float y) { return !(x > 160 && x < 192 && y > -64 && y < 256); };
  float dx = 0, dy = 0;
  if (!aip->steer_around(fw, 7, 100, 100, 300, 100, 32.0f, walk, &dx, &dy) || std::fabs(dy) < 0.2f) {
    return fail("coyote should steer round the fence (heading " + std::to_string(dx) + "," + std::to_string(dy) + ")");
  }
  m.play_end();
  fw.set_extensions(nullptr);
  m.unload_all();
  fs::remove_all(dir, ec);

  std::cout << "[smoke] ai world OK (plugin ts.ai; path round wall " << static_cast<int>(len)
            << " px; BT guard + Lua agent + graph agent arrived, worst gap " << min_gap
            << " px; heard gunshot; Lua + bridge ai table; client skipped; coyote steers round fence)\n";
  return run_ai3d_smoke();
}

namespace {

namespace s3 = ts::tombstone::scene3d;

s3::Entity3D cube(std::uint64_t id, const char* name, s3::Vec3 pos, s3::Vec3 size) {
  s3::Entity3D e;
  e.id = id;
  e.name = name;
  e.transform.position = pos;
  e.transform.scale = size;
  e.mesh = s3::MeshRef{"primitive:cube", true};
  s3::Collider3D c;
  e.collider = c;
  return e;
}

int fail3(const std::string& why) {
  std::cerr << "[smoke] ai 3D FAILED: " << why << "\n";
  return 1;
}

}  // namespace

int run_ai3d_smoke() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ts_ai3d_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  std::string err;

  // 1. Scene v13 round trip: agent + nav_link.
  s3::Scene3D s;
  s.entities.push_back(cube(1, "Ground", {0, -0.25f, 0}, {24, 0.5f, 24}));
  s.entities.push_back(cube(2, "Wall", {0, 1, 0}, {0.5f, 2, 16}));
  s.entities.push_back(cube(3, "Ledge", {-5, 0.75f, -10.5f}, {4, 1.5f, 3}));
  // The ledge (1.5 m up) is reachable only through the link.
  s3::Entity3D link;
  link.id = 4;
  link.name = "Ladder";
  link.transform.position = {-5, 0, -8.4f};
  s3::NavLink3D nl;
  nl.to = {-5, 1.5f, -9.6f};
  link.nav_link = nl;
  s.entities.push_back(link);
  for (int i = 0; i < 6; ++i) {
    s3::Entity3D a;
    a.id = 10 + static_cast<std::uint64_t>(i);
    a.name = "Walker " + std::to_string(i);
    a.transform.position = {-6.0f, 0, -3.0f + i * 1.2f};
    s3::Agent3D ag;
    ag.speed = 3.0f;
    a.agent = ag;
    s.entities.push_back(a);
  }
  s3::Entity3D climber;
  climber.id = 20;
  climber.name = "Climber";
  climber.transform.position = {-4, 0, -6};
  climber.agent = s3::Agent3D{};
  s.entities.push_back(climber);
  const std::string text = s3::write(s);
  s3::Scene3D back;
  if (!s3::parse(text, &back, &err) || !(back.entities == s.entities)) return fail3("scene v13 agent / nav_link round trip " + err);

  world3d::World3D w;
  auto ai = ai3d::attach(w, true);
  if (!ai) return fail3("attach");
  if (!w.start(s, dir.string(), &err)) return fail3("start: " + err);
  if (!ai->navmesh().ready()) return fail3("navmesh bake: " + ai->build_error());
  if (ai->agent_ids().size() != 7) return fail3("agents from components: " + std::to_string(ai->agent_ids().size()));
  if (ai->navmesh().stats().offmesh != 1) return fail3("nav_link baked as off-mesh link");
  // Path round the wall, and onto the ledge via the link.
  const ai::NavPath round = ai->find_path({-6, 0, 0}, {6, 0, 0});
  if (!round.found || round.partial || round.length() < 17.0f) return fail3("path round the wall");
  const ai::NavPath up = ai->find_path({-4, 0, -6}, {-5, 1.5f, -11});
  bool via = false;
  for (auto f : up.offmesh) via |= f != 0;
  if (!up.found || up.partial || !via) return fail3("path onto the ledge via the nav_link");
  // Six walkers cross to the far side of the wall in a column (crowd).
  for (int i = 0; i < 6; ++i) ai->move_to(10 + static_cast<std::uint64_t>(i), {6.0f, 0, -3.0f + (5 - i) * 1.2f});
  ai->move_to(20, {-5, 1.5f, -11});
  float worst = 1e9f;
  for (int t = 0; t < 60 * 20; ++t) {
    w.step(world3d::Input3D{});
    if (ai->crowd()) worst = std::min(worst, ai->crowd()->min_clearance());
  }
  for (int i = 0; i < 6; ++i) {
    const auto* e = w.find(10 + static_cast<std::uint64_t>(i));
    if (!e || e->transform.position.x < 5.0f) {
      return fail3("walker " + std::to_string(i) + " should cross (x=" + std::to_string(e ? e->transform.position.x : -99.f) + ")");
    }
  }
  if (worst < -0.1f) return fail3("crowd overlap " + std::to_string(worst));
  const auto* cl = w.find(20);
  if (!cl || cl->transform.position.y < 1.2f) {
    return fail3("climber should take the link onto the ledge (y=" + std::to_string(cl ? cl->transform.position.y : -1.f) + ")");
  }
  w.stop();

  // 2. Ghost town: Old Hank wanders (navmesh agent + ai/hank.bt.json), hears a shot.
  if (!s3::write_ghost_town_sample(dir.string(), "AI Town", &err)) return fail3(err);
  s3::Scene3D town;
  if (!s3::load_file(s3::scene_path(dir.string()), &town, &err)) return fail3(err);
  const s3::Entity3D* hank0 = town.find_named("Old Hank");
  if (!hank0 || !hank0->agent || hank0->agent->bt != "ai/hank.bt.json") return fail3("Old Hank agent component");
  // Park the rider far away so Hank strolls instead of greeting.
  for (auto& e : town.entities) {
    if (e.player) e.transform.position = {40, 0, 40};
  }
  world3d::World3D tw;
  auto tai = ai3d::attach(tw, true);
  tw.scripts().put("scripts/probe.lua", "");
  if (!tw.start(town, dir.string(), &err)) return fail3(err);
  if (!tai->navmesh().ready()) return fail3("town navmesh: " + tai->build_error());
  const std::uint64_t hid = hank0->id;
  if (!tai->brain(hid) || !tai->brain(hid)->tree()) return fail3("Hank's tree");
  const render3d::Vec3 start = hank0->transform.position;
  float far = 0.0f, top_speed = 0.0f;
  bool heard = false;
  for (int t = 0; t < 60 * 25; ++t) {
    tw.step(world3d::Input3D{});
    const auto* h = tw.find(hid);
    far = std::max(far, std::hypot(h->transform.position.x - start.x, h->transform.position.z - start.z));
    if (auto* an = tw.animator(hid)) top_speed = std::max(top_speed, an->param("speed"));
    if (t == 100) {
      runtime::SoundEvent shot;
      shot.clip = "builtin:gunshot";
      shot.positional = true;
      shot.x = start.x + 3;
      shot.z = start.z;
      tw.play_sound(shot);
      heard = tai->brain(hid)->blackboard().text("noise") == "builtin:gunshot";
    }
  }
  if (far < 1.0f) return fail3("Old Hank should wander (max " + std::to_string(far) + " m)");
  if (top_speed < 0.3f) return fail3("Hank's animator speed param should follow the crowd");
  if (!heard) return fail3("Hank should hear the shot");
  const std::string live = tai->brain(hid)->tree()->running_path();
  tai->set_debug(true);
  std::vector<world3d::World3DExtension::Line> lines;
  tw.debug_lines(&lines);
  if (lines.size() < 20) return fail3("debug lines (navmesh, path, BT state)");
  tw.stop();
  fs::remove_all(dir, ec);
  std::cout << "[smoke] ai 3D OK (scene v" << s3::kScene3DVersion << " agent + nav_link; navmesh "
            << ai->navmesh().stats().polys << " polys + 1 link; 6-agent crowd crossed, worst gap " << worst
            << " m; climber took the link; Old Hank wandered " << far << " m, heard a shot, BT \"" << live
            << "\"; " << lines.size() << " debug lines)\n";
  return 0;
}

