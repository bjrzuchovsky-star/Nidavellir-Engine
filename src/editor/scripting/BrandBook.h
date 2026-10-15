#pragma once

// The Brand Book (Scripts > Brand Book...): the visual script editor for
// *.graph.json assets. A hand-rolled ImGui canvas in the frontier theme:
// node cards branded by family (events amber, flow copper, math / compare
// sand, variables, entities, audio, particles, UI, plugins), exec wires
// drawn heavy, data wires tinted by the pin's value, a palette (right-click
// / Space) of every graph_node_catalog() entry, and a Ledger for the
// selected node's constants. Saving writes the asset; a running ride
// hot-reloads it (variables kept). Also shows every script language's
// status (C++ toolchain, C# / .NET SDK hint) and builds C++ scripts. One
// shared window for the 2D and 3D/VR editors.

#include "runtime/GraphScript.h"

#include <atomic>
#include <future>
#include <string>
#include <vector>

struct ImDrawList;

namespace ts {
namespace tombstone {
namespace editor {

class BrandBook {
 public:
  static BrandBook& get();

  void open(const std::string& project_dir, const std::string& project_name);
  // Opens straight onto one graph (creating it from the starter if missing).
  void open_graph(const std::string& project_dir, const std::string& project_name, const std::string& rel);
  bool is_open() const { return open_; }
  void draw();  // every frame, from the active editor screen
  // Standalone (ts_admin --brand-book): the window fills the viewport.
  void set_fill_viewport(bool on) { fill_ = on; }

  // Headless helpers (smokes / tools).
  const runtime::Graph& graph() const { return graph_; }
  static runtime::Graph starter_graph();

 private:
  struct Pin {
    int node = 0;
    std::string name;
    bool output = false;
    bool exec = false;
  };

  void refresh_list();
  bool load(const std::string& rel);
  bool save();
  void draw_sidebar();
  void draw_canvas();
  void draw_ledger();
  void draw_palette_popup();
  void draw_languages();
  int add_node(const std::string& type, float x, float y);
  void delete_node(int id);
  void connect(const Pin& a, const Pin& b);
  void recheck();

  bool open_ = false;
  bool fill_ = false;
  bool fit_pending_ = false;  // frame the graph on the next canvas draw
  std::string project_dir_;
  std::string project_name_;
  std::vector<std::string> graphs_;
  std::string rel_;
  runtime::Graph graph_;
  bool dirty_ = false;
  std::vector<std::string> problems_;
  std::string status_;
  char new_name_[64] = {};
  float pan_x_ = 40.0f, pan_y_ = 40.0f, zoom_ = 1.0f;
  int selected_ = 0;
  int dragging_ = 0;
  bool linking_ = false;
  Pin link_from_;
  float palette_x_ = 0.0f, palette_y_ = 0.0f;
  char palette_filter_[48] = {};
  double list_time_ = -10.0;
  // C++ build from the editor (background).
  std::future<std::string> cpp_build_;
  std::string cpp_status_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
