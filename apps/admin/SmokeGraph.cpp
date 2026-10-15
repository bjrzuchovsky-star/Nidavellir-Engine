#include "SmokeGraph.h"

#include "runtime/GraphScript.h"
#include "runtime/World.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ts::tombstone;
using namespace ts::tombstone::runtime;

int fail(const std::string& what) {
  std::cerr << "[smoke] graph FAILED: " << what << '\n';
  return 1;
}

struct G {
  Graph g;
  int add(const std::string& type, std::map<std::string, ScriptValue> in = {}) {
    GraphNode n;
    n.id = g.next_id();
    n.type = type;
    n.x = static_cast<float>(n.id * 40);
    n.inputs = std::move(in);
    g.nodes.push_back(n);
    return n.id;
  }
  void link(int a, const std::string& pa, int b, const std::string& pb) {
    g.links.push_back({a, pa, b, pb});
  }
  void flow(int a, int b, const std::string& pin = "out") { link(a, pin, b, "in"); }
};

ScriptValue N(double v) { return ScriptValue::of_number(v); }
ScriptValue T(const std::string& v) { return ScriptValue::of_text(v); }

Entity2D scripted(std::uint64_t id, const std::string& name, const std::string& path) {
  Entity2D e;
  e.id = id;
  e.name = name;
  e.script = ScriptData{};
  e.script->path = path;
  return e;
}

// start: count = 0, sum += i for i in 1..3, delay 0.5 s -> log "delayed";
// tick: count += 1; when count % 20 == 0 log "count N"; button -> log.
std::string counter_graph() {
  G b;
  const int start = b.add("event.start");
  const int set = b.add("var.set", {{"name", T("count")}, {"value", N(0)}});
  const int loop = b.add("flow.for", {{"from", N(1)}, {"to", N(3)}});
  const int sum = b.add("var.add", {{"name", T("sum")}});
  const int delay = b.add("flow.delay", {{"seconds", N(0.5)}});
  const int dlog = b.add("log", {{"text", T("delayed")}});
  b.flow(start, set);
  b.flow(set, loop);
  b.flow(loop, sum, "body");
  b.link(loop, "index", sum, "amount");
  b.flow(loop, delay, "done");
  b.flow(delay, dlog);
  const int tick = b.add("event.tick");
  const int inc = b.add("var.add", {{"name", T("count")}, {"amount", N(1)}});
  const int mod = b.add("math.mod", {{"b", N(20)}});
  const int eq = b.add("cmp.eq", {{"b", N(0)}});
  const int br = b.add("flow.branch");
  const int join = b.add("text.join", {{"a", T("count ")}});
  const int log = b.add("log");
  b.flow(tick, inc);
  b.link(inc, "value", mod, "a");
  b.link(mod, "value", eq, "a");
  b.link(eq, "value", br, "condition");
  b.flow(inc, br);
  b.flow(br, log, "true");
  b.link(inc, "value", join, "b");
  b.link(join, "value", log, "text");
  const int key = b.add("event.key", {{"slot", N(1)}, {"button", T("action")}});
  const int klog = b.add("log", {{"text", T("pressed")}});
  b.flow(key, klog);
  b.g.variables.push_back({"sum", N(0)});
  b.g.props.push_back({"label", T("clock")});
  return write_graph(b.g);
}

std::vector<std::string> ride(World& w, int ticks, bool press_from_tick_10) {
  std::vector<std::string> out;
  for (int i = 1; i <= ticks; ++i) {
    InputFrame in;
    if (press_from_tick_10 && i >= 10 && i < 20) in.slot(0).buttons |= kButtonAction;
    w.step(in);
    for (const LogEntry& e : w.take_logs()) {
      std::ostringstream s;
      s << e.tick << ":" << to_string(e.level) << ":" << e.text;
      out.push_back(s.str());
    }
  }
  return out;
}

int check_counter(std::vector<std::string>* trace) {
  World w;
  w.script_library().put("scripts/counter.graph.json", counter_graph());
  std::string err;
  if (!w.build({scripted(2, "Clock", "scripts/counter.graph.json")}, "", &err)) {
    return fail("build: " + err);
  }
  w.take_logs();
  *trace = ride(w, 120, true);
  auto* host = dynamic_cast<GraphScriptHost*>(w.scripts()->language("graph"));
  if (!host) return fail("graph host not created");
  const ScriptValue* count = host->variable(2, "count");
  const ScriptValue* sum = host->variable(2, "sum");
  if (!count || count->number != 120 || !sum || sum->number != 6) {
    return fail("variables: count " + (count ? describe(*count) : "-") + " sum " +
                (sum ? describe(*sum) : "-"));
  }
  const std::vector<std::string> want = {"20:info:count 20", "31:info:delayed", "60:info:count 60"};
  auto has = [&](const std::string& s) {
    for (const std::string& t : *trace) if (t == s) return true;
    return false;
  };
  for (const std::string& s : want) {
    if (!has(s)) {
      std::string d;
      for (const std::string& x : *trace) d += "\n  " + x;
      return fail("missing trace line " + s + d);
    }
  }
  int presses = 0;
  for (const std::string& t : *trace) presses += t == "10:info:pressed";
  for (const std::string& t : *trace) if (t.find("pressed") != std::string::npos && t != "10:info:pressed") ++presses;
  if (presses != 1) return fail("button event should fire once per press");
  return 0;
}

int check_mixed() {
  World w;
  w.script_library().put("scripts/bank.lua",
                         "function on_start() self:set('gold', 5) log('lua start') end\n");
  G b;
  const int start = b.add("event.start");
  const int find = b.add("entity.find", {{"name", T("Bank")}});
  const int get = b.add("state.get", {{"key", T("gold")}});
  const int join = b.add("text.join", {{"a", T("graph sees gold ")}});
  const int log = b.add("log");
  const int toast = b.add("toast", {{"text", T("Howdy from a graph")}});
  b.link(find, "id", get, "id");
  b.link(get, "value", join, "b");
  b.link(join, "value", log, "text");
  b.flow(start, log);
  b.flow(log, toast);
  w.script_library().put("scripts/teller.graph.json", write_graph(b.g));
  std::string err;
  if (!w.build({scripted(2, "Bank", "scripts/bank.lua"), scripted(3, "Teller", "scripts/teller.graph.json")},
               "", &err)) {
    return fail("mixed build: " + err);
  }
  w.take_logs();
  const std::vector<std::string> t = ride(w, 2, false);
  const std::vector<std::string> want = {"1:info:lua start", "1:info:graph sees gold 5"};
  std::vector<std::string> got;
  for (const std::string& s : t) if (s.find("toast") == std::string::npos) got.push_back(s);
  if (got.size() < 2 || got[0] != want[0] || got[1] != want[1]) {
    std::string d;
    for (const std::string& s : t) d += "\n  " + s;
    return fail("mixed Lua + graph order:" + d);
  }
  if (w.scripts()->instance_count() != 2) return fail("two instances (one per language)");
  return 0;
}

int check_budget_and_reload() {
  World w;
  G b;
  const int tick = b.add("event.tick");
  const int loop = b.add("flow.for", {{"from", N(1)}, {"to", N(1e9)}});
  const int add = b.add("var.add", {{"name", T("spin")}});
  b.flow(tick, loop);
  b.flow(loop, add, "body");
  w.script_library().put("scripts/spin.graph.json", write_graph(b.g));
  G ok;
  const int s = ok.add("event.start");
  const int set = ok.add("var.set", {{"name", T("keep")}, {"value", N(7)}});
  ok.flow(s, set);
  w.script_library().put("scripts/keep.graph.json", write_graph(ok.g));
  std::string err;
  if (!w.build({scripted(2, "Spinner", "scripts/spin.graph.json"),
                scripted(3, "Keeper", "scripts/keep.graph.json")}, "", &err)) {
    return fail("build: " + err);
  }
  const std::vector<std::string> t = ride(w, 3, false);
  bool budget = false;
  for (const std::string& l : t) budget = budget || l.find("step budget") != std::string::npos;
  if (!budget || w.scripts()->running(2) || !w.scripts()->running(3)) {
    return fail("a runaway graph should stop alone (budget error logged)");
  }
  // Hot reload keeps instance variables.
  G ok2 = ok;
  const int r = ok2.add("event.reload");
  const int rlog = ok2.add("log", {{"text", T("reloaded")}});
  ok2.flow(r, rlog);
  w.script_library().put("scripts/keep.graph.json", write_graph(ok2.g));
  if (!w.scripts()->reload("scripts/keep.graph.json")) return fail("reload");
  auto* host = dynamic_cast<GraphScriptHost*>(w.scripts()->language("graph"));
  const ScriptValue* keep = host->variable(3, "keep");
  if (!keep || keep->number != 7) return fail("reload should keep variables");
  // A broken save keeps the old graph.
  w.script_library().put("scripts/keep.graph.json", "{ \"nodes\": [ { \"id\": 1, \"type\": \"nope\" } ] }");
  if (w.scripts()->reload("scripts/keep.graph.json") || !w.scripts()->running(3)) {
    return fail("a broken graph should not replace the running one");
  }
  return 0;
}

int check_round_trip() {
  const std::string text = counter_graph();
  Graph g;
  std::string err;
  if (!parse_graph(text, &g, &err)) return fail("parse: " + err);
  if (write_graph(g) != text) return fail("write(parse(x)) != x");
  std::vector<std::string> problems;
  if (!check_graph(g, &problems)) return fail("check: " + problems.front());
  g.links.push_back({1, "out", 5, "seconds"});
  if (check_graph(g, &problems)) return fail("exec -> data link should be rejected");
  std::vector<ScriptProp> props;
  if (!ScriptHost::describe(text, "scripts/c.graph.json", &props, &err) || props.size() != 1) {
    return fail("describe graph props");
  }
  if (graph_node_catalog().size() < 50) return fail("catalog too small");
  return 0;
}

}  // namespace

int run_graph_smoke() {
  if (int r = check_round_trip()) return r;
  std::vector<std::string> a, b;
  if (int r = check_counter(&a)) return r;
  if (int r = check_counter(&b)) return r;
  if (a != b) return fail("two runs of the same graph differ (not deterministic)");
  if (int r = check_mixed()) return r;
  if (int r = check_budget_and_reload()) return r;
  std::cout << "[smoke] graph OK (" << graph_node_catalog().size()
            << " node types, round trip, events / flow / delay / for / button edge, "
               "variables, budget stop, reload keeps vars, deterministic x2 ("
            << a.size() << " trace lines), mixed Lua + graph)\n";
  return 0;
}
