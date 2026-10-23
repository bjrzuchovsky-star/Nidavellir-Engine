#pragma once

// Visual scripts: node graphs saved as <project>/scripts/*.graph.json and
// interpreted on the world's fixed tick, deterministically (no clock, no
// randomness; delays count ticks; evaluation order is node-id order), so a
// graph rides the same on the server and on every client replay.
//
//   { "graph": 1,
//     "props": { "reach": 24 },              // overridable per entity
//     "variables": { "count": 0 },           // per instance
//     "nodes": [ { "id": 1, "type": "event.start", "x": 0, "y": 0,
//                  "inputs": { "text": "Howdy" } } ],
//     "links": [ { "from": [1, "out"], "to": [2, "in"] } ] }
//
// Exec links run from an exec output pin to a node's "in"; data links from
// a data output pin to a data input pin (unlinked inputs use the node's
// "inputs" constant, else the pin default). The node catalog below is
// shared with the editor's Brand Book panel.

#include "runtime/Script.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

struct GraphPinDef {
  std::string name;
  ScriptValue def;
};

struct GraphNodeDef {
  std::string type;      // "flow.branch"
  std::string title;     // "Branch"
  std::string category;  // "Events", "Flow", "Math", ...
  bool exec_in = false;
  std::vector<std::string> exec_outs;
  std::vector<GraphPinDef> data_ins;
  std::vector<std::string> data_outs;
  std::string blurb;
};

const std::vector<GraphNodeDef>& graph_node_catalog();
const GraphNodeDef* find_graph_node(const std::string& type);

struct GraphNode {
  int id = 0;
  std::string type;
  float x = 0.0f, y = 0.0f;
  std::map<std::string, ScriptValue> inputs;  // constants for unlinked pins
};

struct GraphLink {
  int from_node = 0;
  std::string from_pin;
  int to_node = 0;
  std::string to_pin;
  bool operator==(const GraphLink& o) const = default;
};

struct Graph {
  std::vector<ScriptProp> props;
  std::vector<ScriptProp> variables;
  std::vector<GraphNode> nodes;  // kept sorted by id
  std::vector<GraphLink> links;
  const GraphNode* node(int id) const;
  GraphNode* node(int id);
  int next_id() const;
};

bool parse_graph(const std::string& text, Graph* out, std::string* error);
std::string write_graph(const Graph& g);
// Structural check: known node types, links between existing pins of the
// right kind. Problems go to *problems ("node 4 (flow.branch): ...").
bool check_graph(const Graph& g, std::vector<std::string>* problems);

bool describe_graph_props(const std::string& source, const std::string& rel,
                          std::vector<ScriptProp>* defaults, std::string* error);

// The graph language host (see ScriptHost.h).
class GraphScriptHost final : public ScriptLanguageHost {
 public:
  static constexpr int kStepBudget = 20000;  // node executions per hook call
  static constexpr int kMaxDepth = 64;       // data pull / exec nesting

  GraphScriptHost(ScriptWorld& world, ScriptLibrary& library, std::string project_dir);
  ~GraphScriptHost() override;

  const char* language() const override { return "graph"; }
  bool attach(const Entity2D& e, int slot) override;
  void detach(std::uint64_t entity) override;
  bool has(std::uint64_t entity, int slot) const override;
  bool running(std::uint64_t entity, int slot) const override;
  bool has_hook(std::uint64_t entity, int slot, const char* hook) override;
  std::size_t instance_count() const override;
  void start_pending(int slot) override;
  void trigger(bool enter, std::uint64_t trigger, int slot, std::uint64_t other) override;
  void interact(std::uint64_t target, int slot, std::uint64_t rider) override;
  void hand_event(std::uint64_t target, int slot, const char* hook, const std::string& text,
                  double number) override;
  void ui_event(const std::string& widget, bool change, double value,
                const std::string& text) override;
  void run_timers(std::uint64_t tick) override;
  void broadcast(int slot, const char* hook, const std::string& text, double number) override;
  void tick(double dt, int slot) override;
  int reload_changed() override { return 0; }  // via ScriptHost + library
  bool reload(const std::string& rel) override;
  bool uses(const std::string& rel) const override;

  // A variable's value in an entity's (first) graph instance (tests, inspector).
  const ScriptValue* variable(std::uint64_t entity, const std::string& name) const;

  struct Impl;

 private:
  std::unique_ptr<Impl> m_;
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
