#include "core/Localization.h"
#include "runtime/GraphScript.h"

#include "runtime/Input.h"
#include "scene/JsonTree.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

ScriptValue N(double v) { return ScriptValue::of_number(v); }
ScriptValue T(const char* v) { return ScriptValue::of_text(v); }
ScriptValue B(bool v) { return ScriptValue::of_bool(v); }

std::vector<GraphNodeDef> build_catalog() {
  std::vector<GraphNodeDef> c;
  auto ev = [&](const char* type, const char* title, std::vector<std::string> outs,
                std::vector<GraphPinDef> ins, const char* blurb) {
    c.push_back({type, title, "Events", false, {"out"}, std::move(ins), std::move(outs), blurb});
  };
  ev("event.start", "On Start", {}, {}, "first tick the entity rides");
  ev("event.tick", "On Tick", {"dt", "tick"}, {}, "every tick (dt = 1/60 s)");
  ev("event.trigger_enter", "On Trigger Enter", {"other"}, {}, "somebody rode into the trigger");
  ev("event.trigger_exit", "On Trigger Exit", {"other"}, {}, "...and back out");
  ev("event.interact", "On Interact", {"rider"}, {}, "a rider pressed action close by");
  ev("event.key", "On Button", {"slot"}, {{"slot", N(1)}, {"button", T("action")}},
     "rider `slot` pressed a button (action/alt/start/use/place/fire/next/prev)");
  ev("event.reload", "On Reload", {}, {}, "the graph was hot-reloaded");
  ev("event.vr", "On VR Hand", {"hand", "number"}, {{"hook", T("on_grab")}},
     "on_grab / on_release / on_point / on_unpoint / on_vr_trigger");
  ev("event.plugin", "On Plugin Event", {"text", "number"}, {{"hook", T("on_quest_completed")}},
     "on_quest_started / _completed / _failed, on_objective, on_dialogue_event / _end, "
     "on_game_saved / _loaded");
  ev("event.ui", "On UI", {"value", "text"}, {{"widget", T("")}, {"change", B(false)}},
     "a UI widget was clicked (or changed)");

  auto fl = [&](const char* type, const char* title, std::vector<std::string> outs,
                std::vector<GraphPinDef> ins, std::vector<std::string> douts, const char* blurb) {
    c.push_back({type, title, "Flow", true, std::move(outs), std::move(ins), std::move(douts), blurb});
  };
  fl("flow.branch", "Branch", {"true", "false"}, {{"condition", B(false)}}, {}, "if / else");
  fl("flow.sequence", "Sequence", {"then0", "then1", "then2"}, {}, {}, "runs each output in order");
  fl("flow.delay", "Delay", {"out"}, {{"seconds", N(1)}}, {}, "continues after N seconds of ticks");
  fl("flow.for", "For", {"body", "done"}, {{"from", N(1)}, {"to", N(3)}}, {"index"},
     "body for each index from..to, then done");
  fl("flow.do_once", "Do Once", {"out"}, {}, {}, "passes the first time only");

  auto pure = [&](const char* cat, const char* type, const char* title,
                  std::vector<GraphPinDef> ins, std::vector<std::string> outs, const char* blurb) {
    c.push_back({type, title, cat, false, {}, std::move(ins), std::move(outs), blurb});
  };
  for (const char* op : {"add", "sub", "mul", "div", "mod", "min", "max"}) {
    static const char* titles[] = {"Add", "Subtract", "Multiply", "Divide", "Modulo", "Min", "Max"};
    static const char* ops[] = {"add", "sub", "mul", "div", "mod", "min", "max"};
    std::size_t i = 0;
    while (std::string(ops[i]) != op) ++i;
    pure("Math", (std::string("math.") + op).c_str(), titles[i], {{"a", N(0)}, {"b", N(0)}},
         {"value"}, "a (op) b");
  }
  pure("Math", "math.abs", "Abs", {{"a", N(0)}}, {"value"}, "|a|");
  pure("Math", "math.neg", "Negate", {{"a", N(0)}}, {"value"}, "-a");
  pure("Math", "math.clamp", "Clamp", {{"a", N(0)}, {"lo", N(0)}, {"hi", N(1)}}, {"value"},
       "a held within lo..hi");
  for (const char* op : {"eq", "ne", "lt", "le", "gt", "ge"}) {
    pure("Compare", (std::string("cmp.") + op).c_str(),
         (std::string("a ") + (std::string(op) == "eq" ? "==" : std::string(op) == "ne" ? "!="
                                : std::string(op) == "lt" ? "<" : std::string(op) == "le" ? "<="
                                : std::string(op) == "gt" ? ">" : ">=") + " b").c_str(),
         {{"a", N(0)}, {"b", N(0)}}, {"value"}, "compare (text compares as text)");
  }
  pure("Compare", "logic.and", "And", {{"a", B(false)}, {"b", B(false)}}, {"value"}, "a and b");
  pure("Compare", "logic.or", "Or", {{"a", B(false)}, {"b", B(false)}}, {"value"}, "a or b");
  pure("Compare", "logic.not", "Not", {{"a", B(false)}}, {"value"}, "not a");
  pure("Values", "value.number", "Number", {{"value", N(0)}}, {"value"}, "a constant number");
  pure("Values", "value.text", "Text", {{"value", T("")}}, {"value"}, "a constant text");
  pure("Values", "value.bool", "Bool", {{"value", B(false)}}, {"value"}, "a constant flag");
  pure("Values", "text.join", "Join Text", {{"a", T("")}, {"b", T("")}}, {"value"}, "a .. b");
  pure("Values", "text.tr", "Translate", {{"key", T("")}, {"arg0", T("")}, {"arg1", T("")}}, {"value"},
       "the current language's text for key ({0} {1} = args; Strings panel)");
  pure("Values", "prop.get", "Get Prop", {{"name", T("")}}, {"value"}, "the entity's prop");
  pure("Variables", "var.get", "Get Variable", {{"name", T("")}}, {"value"}, "instance variable");
  fl("var.set", "Set Variable", {"out"}, {{"name", T("")}, {"value", N(0)}}, {"value"},
     "instance variable = value");
  fl("var.add", "Add To Variable", {"out"}, {{"name", T("")}, {"amount", N(1)}}, {"value"},
     "instance variable += amount");

  pure("Entities", "entity.self", "Self", {}, {"id"}, "this entity");
  pure("Entities", "entity.find", "Find Entity", {{"name", T("")}}, {"id"}, "entity by name (0 = none)");
  pure("Entities", "entity.player", "Player", {{"slot", N(1)}}, {"id"}, "rider in slot 1-4");
  pure("Entities", "entity.position", "Position", {{"id", N(0)}}, {"x", "y"}, "where it stands");
  pure("Entities", "entity.alive", "Exists", {{"id", N(0)}}, {"value"}, "still in the world");
  pure("Entities", "state.get", "Get State", {{"id", N(0)}, {"key", T("")}}, {"value"},
       "entity state (shared with every language, replicated)");
  auto act = [&](const char* cat, const char* type, const char* title,
                 std::vector<GraphPinDef> ins, std::vector<std::string> outs, const char* blurb) {
    c.push_back({type, title, cat, true, {"out"}, std::move(ins), std::move(outs), blurb});
  };
  act("Entities", "entity.move", "Move To", {{"id", N(0)}, {"x", N(0)}, {"y", N(0)}}, {}, "teleport");
  act("Entities", "entity.move_by", "Move By", {{"id", N(0)}, {"dx", N(0)}, {"dy", N(0)}}, {}, "nudge");
  act("Entities", "entity.set_visible", "Set Visible", {{"id", N(0)}, {"visible", B(true)}}, {}, "show / hide");
  act("Entities", "entity.set_collider", "Set Collider", {{"id", N(0)}, {"on", B(true)}}, {}, "solid or not");
  act("Entities", "entity.play", "Play Clip", {{"id", N(0)}, {"clip", T("")}, {"hold", B(false)}}, {}, "animation clip");
  act("Entities", "entity.spawn", "Spawn", {{"template", T("")}, {"x", N(0)}, {"y", N(0)}}, {"id"},
      "copy of an authored entity");
  act("Entities", "entity.destroy", "Destroy", {{"id", N(0)}}, {}, "remove from the world");
  act("Entities", "state.set", "Set State", {{"id", N(0)}, {"key", T("")}, {"value", N(0)}}, {}, "entity state");
  act("Audio", "audio.play", "Play Sound", {{"clip", T("builtin:click")}, {"volume", N(1)}}, {}, "one-shot on sfx");
  act("Audio", "audio.music", "Play Music", {{"clip", T("")}, {"fade", N(1)}}, {}, "crossfade the music");
  act("Particles", "fx.burst", "Particle Burst", {{"preset", T("dust")}, {"id", N(0)}}, {},
      "preset burst at the entity (0 = self)");
  act("UI", "ui.show", "Show UI", {{"target", T("")}}, {}, "show a UI document");
  act("UI", "ui.hide", "Hide UI", {{"target", T("")}}, {}, "hide a UI document");
  act("UI", "ui.set_text", "Set UI Text", {{"target", T("")}, {"text", T("")}}, {}, "widget text");
  act("UI", "toast", "Toast", {{"text", T("Howdy")}, {"slot", N(0)}, {"seconds", N(2)}}, {},
      "message to rider slot (0 = all)");
  act("UI", "log", "Log", {{"text", T("")}}, {}, "a Telegraph line");
  // 3D skeletal animation (World3D's anim table through the plugin bridge).
  act("Animation", "anim.play", "Play Animation", {{"id", N(0)}, {"name", T("")}, {"fade", N(0.2)}}, {},
      "state or clip on a 3D animator (0 = self)");
  act("Animation", "anim.crossfade", "Crossfade", {{"id", N(0)}, {"name", T("")}, {"seconds", N(0.25)}}, {},
      "blend to a state over seconds");
  act("Animation", "anim.set_param", "Set Anim Param", {{"id", N(0)}, {"name", T("speed")}, {"value", N(0)}}, {},
      "animator parameter (bools: 0 / 1)");
  // ts.ai (the ai table through the plugin bridge; 0 = self). 2D: x, y px
  // (z unused); 3D: metres, y up.
  act("AI", "ai.agent", "Make Agent", {{"id", N(0)}, {"speed", N(0)}}, {"value"},
      "AI agent (2D speed px/s; 3D: Agent component)");
  act("AI", "ai.bt", "Run Behaviour Tree", {{"id", N(0)}, {"path", T("ai/")}}, {"value"}, "load a .bt.json");
  act("AI", "ai.move_to", "AI Move To", {{"id", N(0)}, {"x", N(0)}, {"y", N(0)}, {"z", N(0)}}, {"value"},
      "path there (false = unreachable)");
  act("AI", "ai.stop", "AI Stop", {{"id", N(0)}}, {}, "stop moving");
  act("AI", "ai.set_bb", "Set Blackboard", {{"id", N(0)}, {"key", T("")}, {"value", N(0)}}, {},
      "behaviour tree blackboard");
  act("AI", "ai.get_bb", "Get Blackboard", {{"id", N(0)}, {"key", T("")}}, {"value"}, "blackboard value");
  act("AI", "ai.state", "AI State", {{"id", N(0)}}, {"value"}, "running branch of the tree");
  // Physics (the physics table through the plugin bridge; 0 = self). 2D:
  // px, px / s (z unused); 3D: metres, y up.
  act("Physics", "phys.apply_force", "Apply Force", {{"id", N(0)}, {"x", N(0)}, {"y", N(0)}, {"z", N(0)}},
      {"value"}, "push a rigid body (every tick)");
  act("Physics", "phys.apply_impulse", "Apply Impulse", {{"id", N(0)}, {"x", N(0)}, {"y", N(0)}, {"z", N(0)}},
      {"value"}, "one kick (kg * speed)");
  act("Physics", "phys.set_velocity", "Set Velocity", {{"id", N(0)}, {"x", N(0)}, {"y", N(0)}, {"z", N(0)}},
      {"value"}, "rigid body velocity");
  act("Physics", "phys.velocity_axis", "Get Velocity", {{"id", N(0)}, {"axis", N(0)}}, {"value"},
      "velocity along x (0), y (1) or z (2)");
  act("Physics", "phys.speed", "Get Speed", {{"id", N(0)}}, {"value"}, "how fast it moves");
  act("Physics", "phys.raycast_xyz", "Raycast",
      {{"x", N(0)}, {"y", N(0)}, {"z", N(0)}, {"dx", N(1)}, {"dy", N(0)}, {"dz", N(0)}, {"dist", N(100)}},
      {"value"}, "first entity hit (false = none)");
  act("Physics", "phys.joint_xyz", "Add Joint",
      {{"a", N(0)}, {"b", N(0)}, {"type", T("hinge")}, {"ax", N(0)}, {"ay", N(0)}, {"az", N(0)}}, {"value"},
      "join b to a (0 = the world); joint handle");
  act("Physics", "phys.unjoint", "Remove Joint", {{"joint", N(0)}}, {}, "break a joint");
  act("Plugins", "plugin.call", "Plugin Call",
      {{"table", T("")}, {"fn", T("")}, {"a", N(0)}, {"b", N(0)}, {"c", N(0)}}, {"value"},
      "table.fn(a, b, c) on a plugin's Lua table");
  return c;
}

double num(const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Number: return v.number;
    case ScriptValue::Type::Bool: return v.flag ? 1.0 : 0.0;
    case ScriptValue::Type::Text: return std::atof(v.text.c_str());
  }
  return 0.0;
}
bool truthy(const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Number: return v.number != 0.0;
    case ScriptValue::Type::Bool: return v.flag;
    case ScriptValue::Type::Text: return !v.text.empty();
  }
  return false;
}
std::string text(const ScriptValue& v) {
  if (v.type == ScriptValue::Type::Text) return v.text;
  return describe(v);
}
std::uint64_t id_of(const ScriptValue& v) {
  const double d = num(v);
  return d > 0 ? static_cast<std::uint64_t>(d) : 0;
}

json::Value to_json(const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Number: return json::Value::number(v.number);
    case ScriptValue::Type::Bool: return json::Value::boolean(v.flag);
    case ScriptValue::Type::Text: return json::Value::string(v.text);
  }
  return {};
}
bool from_json(const json::Value& j, ScriptValue* out) {
  if (j.is_number()) *out = N(j.as_number());
  else if (j.is_bool()) *out = B(j.as_bool());
  else if (j.is_string()) *out = ScriptValue::of_text(j.as_string());
  else return false;
  return true;
}

std::uint32_t button_bit(const std::string& name) {
  if (name == "action") return kButtonAction;
  if (name == "alt") return kButtonAlt;
  if (name == "start") return kButtonStart;
  if (name == "use") return kButtonUse;
  if (name == "place") return kButtonPlace;
  if (name == "fire") return kButtonFire;
  if (name == "next") return kButtonNext;
  if (name == "prev") return kButtonPrev;
  return 0;
}

}  // namespace

const std::vector<GraphNodeDef>& graph_node_catalog() {
  static const std::vector<GraphNodeDef> c = build_catalog();
  return c;
}

const GraphNodeDef* find_graph_node(const std::string& type) {
  for (const GraphNodeDef& d : graph_node_catalog()) {
    if (d.type == type) return &d;
  }
  return nullptr;
}

const GraphNode* Graph::node(int id) const {
  for (const GraphNode& n : nodes) if (n.id == id) return &n;
  return nullptr;
}
GraphNode* Graph::node(int id) {
  for (GraphNode& n : nodes) if (n.id == id) return &n;
  return nullptr;
}
int Graph::next_id() const {
  int m = 0;
  for (const GraphNode& n : nodes) m = std::max(m, n.id);
  return m + 1;
}

bool parse_graph(const std::string& src, Graph* out, std::string* error) {
  json::Value root;
  if (!json::parse(src, &root, error)) return false;
  if (!root.is_object() || !root.has("nodes")) {
    if (error) *error = "not a TombStone graph (no \"nodes\")";
    return false;
  }
  Graph g;
  for (const auto& [k, v] : root["props"].members()) {
    ScriptValue sv;
    if (from_json(v, &sv)) g.props.push_back({k, sv});
  }
  for (const auto& [k, v] : root["variables"].members()) {
    ScriptValue sv;
    if (from_json(v, &sv)) g.variables.push_back({k, sv});
  }
  std::set<int> ids;
  for (const json::Value& n : root["nodes"].items()) {
    GraphNode gn;
    gn.id = static_cast<int>(n.num("id", 0));
    gn.type = n.str("type");
    gn.x = n.flt("x", 0.0f);
    gn.y = n.flt("y", 0.0f);
    if (gn.id <= 0 || !ids.insert(gn.id).second) {
      if (error) *error = "node ids must be positive and unique (" + std::to_string(gn.id) + ")";
      return false;
    }
    for (const auto& [k, v] : n["inputs"].members()) {
      ScriptValue sv;
      if (from_json(v, &sv)) gn.inputs[k] = sv;
    }
    g.nodes.push_back(std::move(gn));
  }
  std::sort(g.nodes.begin(), g.nodes.end(), [](const GraphNode& a, const GraphNode& b) { return a.id < b.id; });
  for (const json::Value& l : root["links"].items()) {
    const json::Value& f = l["from"];
    const json::Value& t = l["to"];
    if (f.size() != 2 || t.size() != 2) continue;
    g.links.push_back({static_cast<int>(f[0].as_number()), f[1].as_string_or(""),
                       static_cast<int>(t[0].as_number()), t[1].as_string_or("")});
  }
  *out = std::move(g);
  return true;
}

std::string write_graph(const Graph& g) {
  json::Value root = json::Value::object();
  root.set("graph", json::Value::number(1));
  json::Value props = json::Value::object();
  for (const ScriptProp& p : g.props) props.set(p.name, to_json(p.value));
  root.set("props", std::move(props));
  json::Value vars = json::Value::object();
  for (const ScriptProp& p : g.variables) vars.set(p.name, to_json(p.value));
  root.set("variables", std::move(vars));
  json::Value nodes = json::Value::array();
  for (const GraphNode& n : g.nodes) {
    json::Value jn = json::Value::object();
    jn.set("id", json::Value::number(n.id));
    jn.set("type", json::Value::string(n.type));
    jn.set("x", json::Value::number(std::round(n.x)));
    jn.set("y", json::Value::number(std::round(n.y)));
    if (!n.inputs.empty()) {
      json::Value in = json::Value::object();
      for (const auto& [k, v] : n.inputs) in.set(k, to_json(v));
      jn.set("inputs", std::move(in));
    }
    nodes.push(std::move(jn));
  }
  root.set("nodes", std::move(nodes));
  json::Value links = json::Value::array();
  for (const GraphLink& l : g.links) {
    json::Value jl = json::Value::object();
    json::Value f = json::Value::array();
    f.push(json::Value::number(l.from_node));
    f.push(json::Value::string(l.from_pin));
    json::Value t = json::Value::array();
    t.push(json::Value::number(l.to_node));
    t.push(json::Value::string(l.to_pin));
    jl.set("from", std::move(f));
    jl.set("to", std::move(t));
    links.push(std::move(jl));
  }
  root.set("links", std::move(links));
  return json::write(root);
}

bool check_graph(const Graph& g, std::vector<std::string>* problems) {
  bool ok = true;
  auto bad = [&](const std::string& s) {
    ok = false;
    if (problems) problems->push_back(s);
  };
  for (const GraphNode& n : g.nodes) {
    if (!find_graph_node(n.type)) bad("node " + std::to_string(n.id) + ": unknown type " + n.type);
  }
  for (const GraphLink& l : g.links) {
    const GraphNode* a = g.node(l.from_node);
    const GraphNode* b = g.node(l.to_node);
    const GraphNodeDef* da = a ? find_graph_node(a->type) : nullptr;
    const GraphNodeDef* db = b ? find_graph_node(b->type) : nullptr;
    const std::string where = "link " + std::to_string(l.from_node) + "." + l.from_pin + " -> " +
                              std::to_string(l.to_node) + "." + l.to_pin;
    if (!da || !db) {
      bad(where + ": missing node");
      continue;
    }
    const bool exec_out = std::count(da->exec_outs.begin(), da->exec_outs.end(), l.from_pin) > 0;
    const bool data_out = std::count(da->data_outs.begin(), da->data_outs.end(), l.from_pin) > 0;
    if (exec_out) {
      if (l.to_pin != "in" || !db->exec_in) bad(where + ": exec must go to an \"in\"");
    } else if (data_out) {
      const bool has = std::any_of(db->data_ins.begin(), db->data_ins.end(),
                                   [&](const GraphPinDef& p) { return p.name == l.to_pin; });
      if (!has) bad(where + ": no such input");
    } else {
      bad(where + ": no such output");
    }
  }
  return ok;
}

bool describe_graph_props(const std::string& source, const std::string& rel,
                          std::vector<ScriptProp>* defaults, std::string* error) {
  Graph g;
  std::string err;
  if (!parse_graph(source, &g, &err)) {
    if (error) *error = rel + ": " + err;
    return false;
  }
  if (defaults) {
    *defaults = g.props;
    std::sort(defaults->begin(), defaults->end(),
              [](const ScriptProp& a, const ScriptProp& b) { return a.name < b.name; });
  }
  return true;
}

// ---------------------------------------------------------------------------
// Interpreter

namespace {

struct Delay {
  std::uint64_t due = 0;
  std::uint64_t seq = 0;
  std::uint64_t entity = 0;
  int slot = 0;
  int node = 0;
};

struct Instance {
  std::uint64_t entity = 0;
  int slot = 0;  // place in the entity's script stack
  std::string path;
  std::string name;
  std::uint64_t version = 0;
  std::shared_ptr<const Graph> graph;
  std::vector<ScriptProp> overrides;
  std::map<std::string, ScriptValue> vars;
  std::map<std::pair<int, std::string>, ScriptValue> outs;  // event / node outputs
  std::set<int> done_once;
  std::map<int, std::uint32_t> prev_buttons;
  bool loaded = false, enabled = true, started = false, gone = false;
};

}  // namespace

struct GraphScriptHost::Impl {
  ScriptWorld* world = nullptr;
  ScriptLibrary* library = nullptr;
  std::string project_dir;
  std::vector<std::unique_ptr<Instance>> instances;
  std::vector<Delay> delays;
  std::uint64_t delay_seq = 0;
  int steps = 0;
  bool aborted = false;
  std::string abort_reason;
  int abort_node = 0;

  Instance* find(std::uint64_t e, int slot) {
    for (auto& in : instances) if (in->entity == e && in->slot == slot && !in->gone) return in.get();
    return nullptr;
  }
  const Instance* find(std::uint64_t e, int slot) const {
    for (const auto& in : instances) if (in->entity == e && in->slot == slot && !in->gone) return in.get();
    return nullptr;
  }
  const Instance* find_any(std::uint64_t e) const {
    for (const auto& in : instances) if (in->entity == e && !in->gone) return in.get();
    return nullptr;
  }

  // Errors name the object and the graph: `"Gate" (scripts/gate.graph.json)`.
  void log(LogLevel level, const std::string& msg, const Instance* in, int node) {
    LogEntry e;
    e.level = level;
    e.channel = "script";
    e.text = node ? msg + " (node " + std::to_string(node) + ")" : msg;
    if (in && level >= LogLevel::Warn) {
      const Entity2D* ent = world->script_entity(in->entity);
      e.text += " -- \"" + (ent ? ent->name : in->name) + "\" (" + in->path + ")";
    }
    e.file = in ? in->path : std::string();
    e.entity = in ? in->entity : 0;
    e.tick = world->script_tick();
    world->script_log(std::move(e));
  }

  bool load(Instance& in, std::string* err) {
    const ScriptLibrary::Entry* src = library->get(project_dir, in.path);
    if (!src || !src->ok) {
      *err = src ? src->error : std::string("Script not found");
      return false;
    }
    auto g = std::make_shared<Graph>();
    if (!parse_graph(src->text, g.get(), err)) return false;
    std::vector<std::string> problems;
    if (!check_graph(*g, &problems)) {
      *err = problems.front() + (problems.size() > 1 ? " (+" + std::to_string(problems.size() - 1) + " more)" : "");
      return false;
    }
    in.graph = std::move(g);
    in.version = src->version;
    for (const ScriptProp& v : in.graph->variables) in.vars.emplace(v.name, v.value);
    return true;
  }

  const GraphLink* data_link(const Graph& g, int node, const std::string& pin) const {
    for (const GraphLink& l : g.links) {
      if (l.to_node == node && l.to_pin == pin) return &l;
    }
    return nullptr;
  }

  ScriptValue input(Instance& in, const GraphNode& n, const std::string& pin, int depth) {
    if (const GraphLink* l = data_link(*in.graph, n.id, pin)) {
      return output(in, l->from_node, l->from_pin, depth + 1);
    }
    const auto it = n.inputs.find(pin);
    if (it != n.inputs.end()) return it->second;
    if (const GraphNodeDef* d = find_graph_node(n.type)) {
      for (const GraphPinDef& p : d->data_ins) if (p.name == pin) return p.def;
    }
    return N(0);
  }

  void abort(const std::string& why, int node) {
    if (!aborted) {
      aborted = true;
      abort_reason = why;
      abort_node = node;
    }
  }

  ScriptValue prop(Instance& in, const std::string& name) {
    for (const ScriptProp& p : in.overrides) if (p.name == name) return p.value;
    for (const ScriptProp& p : in.graph->props) if (p.name == name) return p.value;
    return N(0);
  }

  // Data output of a node (pure nodes compute; exec nodes report what
  // they stored when they last ran).
  ScriptValue output(Instance& in, int node_id, const std::string& pin, int depth) {
    if (depth > kMaxDepth) {
      abort("data pulled too deep (a loop between data pins?)", node_id);
      return N(0);
    }
    const GraphNode* np = in.graph->node(node_id);
    if (!np) return N(0);
    const GraphNode& n = *np;
    const std::string& t = n.type;
    auto I = [&](const char* p) { return input(in, n, p, depth); };
    const auto stored = in.outs.find({node_id, pin});
    if (t.rfind("event.", 0) == 0 || t == "flow.for" || t == "var.set" || t == "var.add" || t == "plugin.call" ||
        t == "entity.spawn" || t.rfind("ai.", 0) == 0 || t.rfind("phys.", 0) == 0) {
      return stored == in.outs.end() ? N(0) : stored->second;
    }
    if (t.rfind("math.", 0) == 0) {
      const double a = num(I("a"));
      if (t == "math.abs") return N(std::fabs(a));
      if (t == "math.neg") return N(-a);
      if (t == "math.clamp") return N(std::clamp(a, num(I("lo")), std::max(num(I("lo")), num(I("hi")))));
      const double b = num(I("b"));
      if (t == "math.add") return N(a + b);
      if (t == "math.sub") return N(a - b);
      if (t == "math.mul") return N(a * b);
      if (t == "math.div") return N(b == 0.0 ? 0.0 : a / b);
      if (t == "math.mod") return N(b == 0.0 ? 0.0 : std::fmod(a, b));
      if (t == "math.min") return N(std::min(a, b));
      if (t == "math.max") return N(std::max(a, b));
    }
    if (t.rfind("cmp.", 0) == 0) {
      const ScriptValue a = I("a"), b = I("b");
      int c = 0;
      if (a.type == ScriptValue::Type::Text || b.type == ScriptValue::Type::Text) {
        const std::string x = text(a), y = text(b);
        c = x < y ? -1 : (x > y ? 1 : 0);
      } else {
        const double x = num(a), y = num(b);
        c = x < y ? -1 : (x > y ? 1 : 0);
      }
      if (t == "cmp.eq") return B(c == 0);
      if (t == "cmp.ne") return B(c != 0);
      if (t == "cmp.lt") return B(c < 0);
      if (t == "cmp.le") return B(c <= 0);
      if (t == "cmp.gt") return B(c > 0);
      if (t == "cmp.ge") return B(c >= 0);
    }
    if (t == "logic.and") return B(truthy(I("a")) && truthy(I("b")));
    if (t == "logic.or") return B(truthy(I("a")) || truthy(I("b")));
    if (t == "logic.not") return B(!truthy(I("a")));
    if (t == "value.number" || t == "value.text" || t == "value.bool") return I("value");
    if (t == "text.join") return ScriptValue::of_text(text(I("a")) + text(I("b")));
    if (t == "text.tr") {
      return ScriptValue::of_text(loc::format(loc::tr(text(I("key"))), {text(I("arg0")), text(I("arg1"))}));
    }
    if (t == "prop.get") return prop(in, text(I("name")));
    if (t == "var.get") {
      const auto it = in.vars.find(text(I("name")));
      return it == in.vars.end() ? N(0) : it->second;
    }
    if (t == "entity.self") return N(static_cast<double>(in.entity));
    if (t == "entity.find") return N(static_cast<double>(world->script_find(text(I("name")))));
    if (t == "entity.player") {
      return N(static_cast<double>(world->script_player(static_cast<int>(num(I("slot"))) - 1)));
    }
    if (t == "entity.position") {
      const Entity2D* e = world->script_entity(id_of(I("id")));
      return N(e ? (pin == "x" ? e->x : e->y) : 0.0);
    }
    if (t == "entity.alive") return B(world->script_entity(id_of(I("id"))) != nullptr);
    if (t == "state.get") {
      const ScriptValue* v = world->script_get(id_of(I("id")), text(I("key")));
      return v ? *v : N(0);
    }
    return N(0);
  }

  // Runs the chain starting at node `id` (its "in" fired).
  void exec(Instance& in, int id, int depth) {
    while (id && !aborted && in.enabled && !in.gone) {
      if (++steps > kStepBudget) {
        abort("ran over its step budget (" + std::to_string(kStepBudget) + " nodes in one hook)", id);
        return;
      }
      if (depth > kMaxDepth) {
        abort("flow nested too deep", id);
        return;
      }
      const GraphNode* np = in.graph->node(id);
      if (!np) return;
      const GraphNode& n = *np;
      const std::string& t = n.type;
      auto I = [&](const char* p) { return input(in, n, p, 0); };
      std::string next = "out";
      if (t == "flow.branch") {
        next = truthy(I("condition")) ? "true" : "false";
      } else if (t == "flow.sequence") {
        fire(in, n.id, "then0", depth + 1);
        fire(in, n.id, "then1", depth + 1);
        next = "then2";
      } else if (t == "flow.delay") {
        const double secs = std::max(0.0, num(I("seconds")));
        const auto ticks = static_cast<std::uint64_t>(std::llround(secs * kScriptTickRate));
        delays.push_back({world->script_tick() + std::max<std::uint64_t>(1, ticks), ++delay_seq,
                          in.entity, in.slot, n.id});
        return;
      } else if (t == "flow.for") {
        const long long from = std::llround(num(I("from")));
        const long long to = std::llround(num(I("to")));
        for (long long i = from; i <= to && !aborted; ++i) {
          in.outs[{n.id, "index"}] = N(static_cast<double>(i));
          fire(in, n.id, "body", depth + 1);
        }
        next = "done";
      } else if (t == "flow.do_once") {
        if (!in.done_once.insert(n.id).second) return;
      } else if (t == "var.set") {
        const ScriptValue v = I("value");
        in.vars[text(I("name"))] = v;
        in.outs[{n.id, "value"}] = v;
      } else if (t == "var.add") {
        ScriptValue& v = in.vars[text(I("name"))];
        v = N(num(v) + num(I("amount")));
        in.outs[{n.id, "value"}] = v;
      } else if (t == "entity.move") {
        world->script_move(id_of(I("id")), static_cast<float>(num(I("x"))), static_cast<float>(num(I("y"))));
      } else if (t == "entity.move_by") {
        const std::uint64_t e = id_of(I("id"));
        if (const Entity2D* p = world->script_entity(e)) {
          world->script_move(e, p->x + static_cast<float>(num(I("dx"))), p->y + static_cast<float>(num(I("dy"))));
        }
      } else if (t == "entity.set_visible") {
        world->script_set_visible(id_of(I("id")), truthy(I("visible")));
      } else if (t == "entity.set_collider") {
        world->script_set_collider(id_of(I("id")), truthy(I("on")));
      } else if (t == "entity.play") {
        world->script_play(id_of(I("id")), text(I("clip")), truthy(I("hold")));
      } else if (t == "entity.spawn") {
        in.outs[{n.id, "id"}] = N(static_cast<double>(world->script_spawn(
            text(I("template")), static_cast<float>(num(I("x"))), static_cast<float>(num(I("y"))))));
      } else if (t == "entity.destroy") {
        world->script_destroy(id_of(I("id")));
      } else if (t == "state.set") {
        const ScriptValue v = I("value");
        world->script_set(id_of(I("id")), text(I("key")), &v);
      } else if (t == "audio.play") {
        SoundEvent ev;
        ev.clip = text(I("clip"));
        ev.volume = static_cast<float>(num(I("volume")));
        ev.entity = in.entity;
        world->script_sound(std::move(ev));
      } else if (t == "audio.music") {
        SoundEvent ev;
        ev.kind = SoundEvent::Kind::Music;
        ev.clip = text(I("clip"));
        ev.bus = "music";
        ev.fade = static_cast<float>(num(I("fade")));
        ev.entity = in.entity;
        world->script_sound(std::move(ev));
      } else if (t == "fx.burst") {
        FxEvent ev;
        ev.preset = text(I("preset"));
        const std::uint64_t at = id_of(I("id"));
        ev.entity = at ? at : in.entity;
        ev.at_entity = true;
        if (const Entity2D* e = world->script_entity(ev.entity)) {
          ev.x = e->x;
          ev.y = e->y;
        }
        world->script_fx(std::move(ev));
      } else if (t == "ui.show" || t == "ui.hide" || t == "ui.set_text") {
        UiCommand c;
        c.kind = t == "ui.show" ? UiCommand::Kind::Show
                 : t == "ui.hide" ? UiCommand::Kind::Hide : UiCommand::Kind::SetText;
        c.target = text(I("target"));
        if (t == "ui.set_text") c.text = text(I("text"));
        world->script_ui(std::move(c));
      } else if (t == "toast") {
        const int slot = static_cast<int>(num(I("slot")));
        world->script_toast(slot <= 0 ? -1 : slot - 1, text(I("text")), num(I("seconds")), in.entity);
      } else if (t.rfind("anim.", 0) == 0) {
        const std::uint64_t e = id_of(I("id"));
        const char* third = t == "anim.crossfade" ? "seconds" : t == "anim.set_param" ? "value" : "fade";
        ScriptValue r;
        std::string err;
        if (!world->script_plugin_call("anim", t.substr(5),
                                       {N(static_cast<double>(e ? e : in.entity)), I("name"), I(third)}, &r,
                                       &err)) {
          log(LogLevel::Error, err, &in, 0);
        }
      } else if (t.rfind("ai.", 0) == 0) {
        const std::uint64_t e0 = id_of(I("id"));
        std::vector<ScriptValue> args{N(static_cast<double>(e0 ? e0 : in.entity))};
        if (t == "ai.agent") args.push_back(I("speed"));
        else if (t == "ai.bt") args.push_back(I("path"));
        else if (t == "ai.move_to") args.insert(args.end(), {I("x"), I("y"), I("z")});
        else if (t == "ai.set_bb") args.insert(args.end(), {I("key"), I("value")});
        else if (t == "ai.get_bb") args.push_back(I("key"));
        ScriptValue r;
        std::string err;
        if (world->script_plugin_call("ai", t.substr(3), args, &r, &err)) {
          in.outs[{n.id, "value"}] = r;
        } else {
          in.outs[{n.id, "value"}] = B(false);
          log(LogLevel::Error, err, &in, 0);
        }
      } else if (t.rfind("phys.", 0) == 0) {
        const std::string fn = t.substr(5);
        std::vector<ScriptValue> args;
        auto self_or = [&](const char* pin) {
          const std::uint64_t e = id_of(I(pin));
          return N(static_cast<double>(e ? e : in.entity));
        };
        if (fn == "raycast_xyz") {
          args = {I("x"), I("y"), I("z"), I("dx"), I("dy"), I("dz"), I("dist")};
        } else if (fn == "joint_xyz") {
          args = {N(static_cast<double>(id_of(I("a")))), self_or("b"), I("type"), I("ax"), I("ay"), I("az")};
        } else if (fn == "unjoint") {
          args = {I("joint")};
        } else {
          args = {self_or("id")};
          if (fn == "velocity_axis") args.push_back(I("axis"));
          else if (fn != "speed") args.insert(args.end(), {I("x"), I("y"), I("z")});
        }
        ScriptValue r;
        std::string err;
        if (world->script_plugin_call("physics", fn, args, &r, &err)) {
          in.outs[{n.id, "value"}] = r;
        } else {
          in.outs[{n.id, "value"}] = B(false);
          log(LogLevel::Error, err, &in, 0);
        }
      } else if (t == "plugin.call") {
        ScriptValue r;
        std::string err;
        if (world->script_plugin_call(text(I("table")), text(I("fn")), {I("a"), I("b"), I("c")}, &r, &err)) {
          in.outs[{n.id, "value"}] = r;
        } else {
          in.outs[{n.id, "value"}] = B(false);
          log(LogLevel::Error, err, &in, 0);
        }
      } else if (t == "log") {
        log(LogLevel::Info, text(I("text")), &in, 0);
      } else if (t.rfind("event.", 0) == 0) {
        // Events have no "in"; a link into one is rejected by check_graph.
        return;
      }
      // Follow every link from (id, next); the last one continues this loop.
      int follow = 0;
      for (const GraphLink& l : in.graph->links) {
        if (l.from_node == n.id && l.from_pin == next) {
          if (follow) exec(in, follow, depth + 1);
          follow = l.to_node;
        }
      }
      id = follow;
    }
  }

  void fire(Instance& in, int node, const std::string& pin, int depth) {
    for (const GraphLink& l : in.graph->links) {
      if (l.from_node == node && l.from_pin == pin) exec(in, l.to_node, depth);
      if (aborted) return;
    }
  }

  // One hook: every matching event node, in id order.
  template <class Match>
  void dispatch(Instance& in, const char* type, Match&& match) {
    if (!in.loaded || !in.enabled || in.gone || !in.graph) return;
    const std::shared_ptr<const Graph> keep = in.graph;  // survives a reload mid-hook
    for (const GraphNode& n : keep->nodes) {
      if (n.type != type || !match(n)) continue;
      steps = 0;
      aborted = false;
      fire(in, n.id, "out", 0);
      if (aborted) {
        log(LogLevel::Error, abort_reason + "; graph stopped", &in, abort_node);
        in.enabled = false;
        return;
      }
    }
  }
  template <class Match>
  void dispatch_one(std::uint64_t entity, int slot, const char* type, Match&& match) {
    if (Instance* in = find(entity, slot)) {
      if (in->started) dispatch(*in, type, match);
    }
    sweep();
  }

  void sweep() {
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                   [](const auto& in) { return in->gone; }),
                    instances.end());
  }
};

GraphScriptHost::GraphScriptHost(ScriptWorld& world, ScriptLibrary& library, std::string project_dir)
    : m_(std::make_unique<Impl>()) {
  m_->world = &world;
  m_->library = &library;
  m_->project_dir = std::move(project_dir);
}

GraphScriptHost::~GraphScriptHost() = default;

bool GraphScriptHost::attach(const Entity2D& e, int slot) {
  if (!e.script || m_->find(e.id, slot)) return false;
  auto in = std::make_unique<Instance>();
  in->entity = e.id;
  in->slot = slot;
  in->path = e.script->path;
  in->name = e.name;
  in->overrides = e.script->props;
  std::string err;
  in->loaded = m_->load(*in, &err);
  if (!in->loaded) m_->log(LogLevel::Error, err, in.get(), 0);
  m_->instances.push_back(std::move(in));
  return m_->instances.back()->loaded;
}

void GraphScriptHost::detach(std::uint64_t entity) {
  for (auto& in : m_->instances) {
    if (in->entity == entity) in->gone = true;
  }
  m_->delays.erase(std::remove_if(m_->delays.begin(), m_->delays.end(),
                                  [&](const Delay& d) { return d.entity == entity; }),
                   m_->delays.end());
  m_->sweep();
}

bool GraphScriptHost::has(std::uint64_t entity, int slot) const {
  return m_->find(entity, slot) != nullptr;
}

bool GraphScriptHost::running(std::uint64_t entity, int slot) const {
  const Instance* in = m_->find(entity, slot);
  return in && in->loaded && in->enabled;
}

bool GraphScriptHost::has_hook(std::uint64_t entity, int slot, const char* hook) {
  const Instance* in = m_->find(entity, slot);
  if (!in || !in->graph) return false;
  const std::string h = hook;
  for (const GraphNode& n : in->graph->nodes) {
    if ((h == "on_start" && n.type == "event.start") || (h == "on_tick" && n.type == "event.tick") ||
        (h == "on_interact" && n.type == "event.interact") ||
        (h == "on_trigger_enter" && n.type == "event.trigger_enter") ||
        (h == "on_trigger_exit" && n.type == "event.trigger_exit") ||
        (h == "on_reload" && n.type == "event.reload")) {
      return true;
    }
    if (n.type == "event.vr") {
      const auto it = n.inputs.find("hook");
      if ((it == n.inputs.end() ? std::string("on_grab") : text(it->second)) == h) return true;
    }
    if (n.type == "event.plugin") {
      const auto it = n.inputs.find("hook");
      if ((it == n.inputs.end() ? std::string("on_quest_completed") : text(it->second)) == h) return true;
    }
  }
  return false;
}

std::size_t GraphScriptHost::instance_count() const { return m_->instances.size(); }

void GraphScriptHost::start_pending(int slot) {
  for (std::size_t i = 0; i < m_->instances.size(); ++i) {
    Instance& in = *m_->instances[i];
    if (in.slot != slot || in.started || in.gone || !in.loaded || !in.enabled) continue;
    in.started = true;
    m_->dispatch(in, "event.start", [](const GraphNode&) { return true; });
  }
  m_->sweep();
}

void GraphScriptHost::trigger(bool enter, std::uint64_t trigger, int slot, std::uint64_t other) {
  const char* type = enter ? "event.trigger_enter" : "event.trigger_exit";
  if (Instance* in = m_->find(trigger, slot)) {
    for (const GraphNode& n : in->graph ? in->graph->nodes : std::vector<GraphNode>{}) {
      if (n.type == type) in->outs[{n.id, "other"}] = N(static_cast<double>(other));
    }
  }
  m_->dispatch_one(trigger, slot, type, [](const GraphNode&) { return true; });
}

void GraphScriptHost::interact(std::uint64_t target, int slot, std::uint64_t rider) {
  if (Instance* in = m_->find(target, slot)) {
    for (const GraphNode& n : in->graph ? in->graph->nodes : std::vector<GraphNode>{}) {
      if (n.type == "event.interact") in->outs[{n.id, "rider"}] = N(static_cast<double>(rider));
    }
  }
  m_->dispatch_one(target, slot, "event.interact", [](const GraphNode&) { return true; });
}

void GraphScriptHost::hand_event(std::uint64_t target, int slot, const char* hook,
                                 const std::string& txt, double number) {
  Instance* in = m_->find(target, slot);
  if (!in || !in->graph) return;
  const std::string h = hook;
  auto match = [&](const GraphNode& n) {
    const auto it = n.inputs.find("hook");
    return (it == n.inputs.end() ? std::string("on_grab") : text(it->second)) == h;
  };
  for (const GraphNode& n : in->graph->nodes) {
    if (n.type == "event.vr" && match(n)) {
      in->outs[{n.id, "hand"}] = ScriptValue::of_text(txt);
      in->outs[{n.id, "number"}] = N(number);
    }
  }
  m_->dispatch_one(target, slot, "event.vr", match);
}

void GraphScriptHost::broadcast(int slot, const char* hook, const std::string& txt,
                                double number) {
  const std::string h = hook;
  auto match = [&](const GraphNode& n) {
    const auto it = n.inputs.find("hook");
    return (it == n.inputs.end() ? std::string("on_quest_completed") : text(it->second)) == h;
  };
  std::vector<std::uint64_t> ids;
  for (const auto& in : m_->instances) {
    if (in->started && in->graph && in->slot == slot) ids.push_back(in->entity);
  }
  std::sort(ids.begin(), ids.end());
  for (std::uint64_t id : ids) {
    Instance* in = m_->find(id, slot);
    if (!in || !in->started || !in->graph) continue;
    const std::shared_ptr<const Graph> keep = in->graph;
    for (const GraphNode& n : keep->nodes) {
      if (n.type == "event.plugin" && match(n)) {
        in->outs[{n.id, "text"}] = ScriptValue::of_text(txt);
        in->outs[{n.id, "number"}] = N(number);
      }
    }
    m_->dispatch(*in, "event.plugin", match);
  }
  m_->sweep();
}

void GraphScriptHost::ui_event(const std::string& widget, bool change, double value,
                               const std::string& txt) {
  for (std::size_t i = 0; i < m_->instances.size(); ++i) {
    Instance& in = *m_->instances[i];
    if (!in.graph || !in.started) continue;
    auto match = [&](const GraphNode& n) {
      const auto w = n.inputs.find("widget");
      const auto c = n.inputs.find("change");
      return w != n.inputs.end() && text(w->second) == widget &&
             (c != n.inputs.end() && truthy(c->second)) == change;
    };
    for (const GraphNode& n : in.graph->nodes) {
      if (n.type == "event.ui" && match(n)) {
        in.outs[{n.id, "value"}] = N(value);
        in.outs[{n.id, "text"}] = ScriptValue::of_text(txt);
      }
    }
    m_->dispatch(in, "event.ui", match);
  }
  m_->sweep();
}

void GraphScriptHost::run_timers(std::uint64_t tick) {
  for (;;) {
    auto best = m_->delays.end();
    for (auto it = m_->delays.begin(); it != m_->delays.end(); ++it) {
      if (it->due <= tick && (best == m_->delays.end() || it->due < best->due ||
                              (it->due == best->due && it->seq < best->seq))) {
        best = it;
      }
    }
    if (best == m_->delays.end()) break;
    const Delay d = *best;
    m_->delays.erase(best);
    Instance* in = m_->find(d.entity, d.slot);
    if (!in || !in->enabled || !in->graph || !in->graph->node(d.node)) continue;
    m_->steps = 0;
    m_->aborted = false;
    m_->fire(*in, d.node, "out", 0);
    if (m_->aborted) {
      m_->log(LogLevel::Error, m_->abort_reason + "; graph stopped", in, m_->abort_node);
      in->enabled = false;
    }
  }
  m_->sweep();
}

void GraphScriptHost::tick(double dt, int slot) {
  const std::size_t n = m_->instances.size();
  for (std::size_t i = 0; i < n && i < m_->instances.size(); ++i) {
    Instance& in = *m_->instances[i];
    if (in.slot != slot || !in.started || !in.graph) continue;
    const std::shared_ptr<const Graph> g = in.graph;
    // Buttons: rising edges per On Button node.
    for (const GraphNode& node : g->nodes) {
      if (node.type != "event.key") continue;
      const auto si = node.inputs.find("slot");
      const auto bi = node.inputs.find("button");
      const int rider = si == node.inputs.end() ? 1 : static_cast<int>(num(si->second));
      const std::uint32_t bit = button_bit(bi == node.inputs.end() ? "action" : text(bi->second));
      const std::uint32_t now = m_->world->script_buttons(rider - 1) & bit;
      const std::uint32_t was = in.prev_buttons[node.id];
      in.prev_buttons[node.id] = now;
      if (now && !was) {
        in.outs[{node.id, "slot"}] = N(rider);
        const int id = node.id;
        m_->dispatch(in, "event.key", [id](const GraphNode& x) { return x.id == id; });
      }
    }
    for (const GraphNode& node : g->nodes) {
      if (node.type == "event.tick") {
        in.outs[{node.id, "dt"}] = N(dt);
        in.outs[{node.id, "tick"}] = N(static_cast<double>(m_->world->script_tick()));
      }
    }
    m_->dispatch(in, "event.tick", [](const GraphNode&) { return true; });
  }
  m_->sweep();
}

bool GraphScriptHost::uses(const std::string& rel) const {
  for (const auto& in : m_->instances) if (!in->gone && in->path == rel) return true;
  return false;
}

bool GraphScriptHost::reload(const std::string& rel) {
  int good = 0, users = 0;
  for (auto& inp : m_->instances) {
    Instance& in = *inp;
    if (in.gone || in.path != rel) continue;
    ++users;
    Instance fresh;
    fresh.path = in.path;
    std::string err;
    if (!m_->load(fresh, &err)) {
      m_->log(LogLevel::Error, err + " -- reload skipped, the previous graph keeps riding", &in, 0);
      return false;
    }
    // State-preserving: every variable keeps its value; newly declared
    // ones start at their defaults.
    for (const auto& [k, v] : fresh.vars) in.vars.emplace(k, v);
    in.graph = fresh.graph;
    in.version = fresh.version;
    in.loaded = true;
    in.enabled = true;
    ++good;
    if (in.started) m_->dispatch(in, "event.reload", [](const GraphNode&) { return true; });
  }
  if (users) {
    m_->log(LogLevel::Info, "Reloaded " + rel + " (" + std::to_string(good) + " of " +
                                std::to_string(users) + " running)", nullptr, 0);
  }
  return users > 0 && good == users;
}

const ScriptValue* GraphScriptHost::variable(std::uint64_t entity, const std::string& name) const {
  const Instance* in = m_->find_any(entity);
  if (!in) return nullptr;
  const auto it = in->vars.find(name);
  return it == in->vars.end() ? nullptr : &it->second;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
