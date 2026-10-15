#pragma once

// 2D grid pathfinding: A* (weighted cells), Jump Point Search (uniform
// cost, much faster on open maps), a hierarchical cluster search (HPA*) for
// big or endless chunked worlds, and flow fields for crowds heading to one
// goal. Grids are given as a walkable(x, y) callback, so tile maps, chunk
// maps and editor previews all plug in without copying.
//
// Moves are 8-way with octile costs (1 straight, sqrt 2 diagonal) and never
// cut corners: a diagonal step needs both orthogonal neighbours open.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ts {
namespace ai {

struct Cell {
  int x = 0;
  int y = 0;
  bool operator==(const Cell& o) const = default;
};

using WalkFn = std::function<bool(int x, int y)>;
// Extra cost multiplier of entering a cell (>= 1); null = uniform.
using CostFn = std::function<float(int x, int y)>;

// Half-open search window [x0, x1) x [y0, y1).
struct GridBounds {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool contains(int x, int y) const { return x >= x0 && y >= y0 && x < x1 && y < y1; }
  int width() const { return x1 - x0; }
  int height() const { return y1 - y0; }
  // Square window of `radius` cells around both a and b.
  static GridBounds around(Cell a, Cell b, int margin);
};

struct GridOptions {
  GridBounds bounds;
  bool diagonal = true;
  std::size_t max_expanded = 1u << 20;
  CostFn cost;  // A* only
};

struct GridPath {
  bool found = false;
  std::vector<Cell> cells;   // start .. goal, every cell
  float cost = 0.0f;
  std::size_t expanded = 0;  // nodes closed (search effort)
};

float octile(Cell a, Cell b);
// Octile length of a cell path (consecutive cells 8-adjacent).
float path_length(const std::vector<Cell>& cells);

GridPath astar(const WalkFn& walkable, Cell start, Cell goal, const GridOptions& opt);
// Uniform cost only (ignores opt.cost); same optimal length as astar.
GridPath jps(const WalkFn& walkable, Cell start, Cell goal, const GridOptions& opt);
// Plain Dijkstra over the window: the reference the smokes check against.
GridPath dijkstra(const WalkFn& walkable, Cell start, Cell goal, const GridOptions& opt);

// Any-angle shortcut: drop cells while the straight line between kept
// cells stays on walkable ground (supercover line, no corner cutting).
std::vector<Cell> smooth_path(const WalkFn& walkable, const std::vector<Cell>& cells);
bool line_walkable(const WalkFn& walkable, Cell a, Cell b);

// HPA*: the world is cut into cluster x cluster squares; entrances along
// shared borders form an abstract graph whose edges are cached local
// searches. Clusters are built lazily on first use, so endless chunked
// worlds only pay for what agents explore. invalidate() after edits.
class HierGrid {
 public:
  explicit HierGrid(WalkFn walkable, int cluster = 16);
  void set_walkable(WalkFn walkable);
  int cluster_size() const { return cluster_; }
  // A cell changed: drop its cluster and the neighbours' shared borders.
  void invalidate(int x, int y);
  void clear();
  // Nearby goals (same or touching clusters) use plain A* over a small
  // window; far ones search the abstract graph within `max_radius` cells.
  GridPath find(Cell start, Cell goal, int max_radius = 2048);
  std::size_t clusters_built() const { return clusters_.size(); }
  std::size_t abstract_expanded() const { return last_abstract_; }

 private:
  struct Edge {
    std::size_t to = 0;  // node index
    float cost = 0.0f;
    std::vector<Cell> path;  // intra edges: cells after `from` up to `to`
  };
  struct Node {
    Cell cell;
    std::vector<Edge> edges;  // intra edges; inter edges come from pairs
    std::vector<std::size_t> pairs;  // across a border (cost 1)
  };
  struct Cluster {
    std::vector<std::size_t> nodes;
    bool linked = false;  // intra edges computed
  };
  using Key = std::pair<int, int>;

  Key cluster_of(Cell c) const;
  GridBounds cluster_bounds(Key k) const;
  Cluster& ensure(Key k);
  void link(Key k);
  std::size_t node_at(Cell c);
  void border(Key a, Key b, bool vertical);

  WalkFn walk_;
  int cluster_ = 16;
  std::map<Key, Cluster> clusters_;
  std::map<std::pair<Key, Key>, bool> borders_;
  std::vector<Node> nodes_;
  std::unordered_map<std::int64_t, std::size_t> node_index_;
  std::vector<bool> node_dead_;
  std::size_t last_abstract_ = 0;
};

// Integration field to the nearest goal cell (Dijkstra, octile); every cell
// then points down the gradient. One build serves any number of agents.
class FlowField {
 public:
  void build(const WalkFn& walkable, const GridBounds& bounds, const std::vector<Cell>& goals,
             bool diagonal = true);
  bool reachable(int x, int y) const;
  float distance(int x, int y) const;  // cells to the nearest goal; inf if none
  // Unit step toward the goal (0, 0 at a goal / unreachable).
  void direction(int x, int y, float* dx, float* dy) const;
  const GridBounds& bounds() const { return bounds_; }
  std::size_t reached() const { return reached_; }

 private:
  GridBounds bounds_;
  std::vector<float> dist_;
  std::vector<std::int8_t> dir_;  // 0..7 neighbour index, -1 none
  std::size_t reached_ = 0;
};

}  // namespace ai
}  // namespace ts
