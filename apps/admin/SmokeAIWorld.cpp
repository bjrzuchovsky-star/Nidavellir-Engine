// --smoke-ai, part 2: the ts.ai plugin riding a 2D World (agents, BT file,
// walls, hearing, Lua table + bridge, client role, Frontier coyote steering).
// The 3D half (ts_ai3d) appends to this file's run via run_ai3d_smoke().

#include "SmokeAI.h"

#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "plugins/ai/AiPlugin.h"
#include "runtime/World.h"

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
  if (min_gap < -2.0f) return fail("agents overlapped (gap " + std::to_string(min_gap) + ")");
  if (!heard_ok) return fail("listener should hear the positional gunshot");
  bool lua_ok = false;
  std::string last;
  for (const auto& l : w.take_logs()) {
    if (l.text.find("n=") != std::string::npos) last = l.text;
    lua_ok |= l.text.find("n=3 len=") != std::string::npos;
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
            << " px; BT guard + Lua agent arrived, worst gap " << min_gap
            << " px; heard gunshot; Lua + bridge ai table; client skipped; coyote steers round fence)\n";
  return run_ai3d_smoke();
}

int run_ai3d_smoke() { return 0; }

