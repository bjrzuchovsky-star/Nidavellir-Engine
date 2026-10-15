#include "ai/GridPath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace ts {
namespace ai {

namespace {

constexpr float kSqrt2 = 1.41421356f;
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

struct Open {
  float f;
  float h;
  int idx;
  bool operator<(const Open& o) const {
    // priority_queue is a max-heap: invert. Ties: lower h first, then index
    // (deterministic regardless of insertion order).
    if (f != o.f) return f > o.f;
    if (h != o.h) return h > o.h;
    return idx > o.idx;
  }
};

struct Window {
  GridBounds b;
  int W = 0, H = 0;
  explicit Window(const GridBounds& bounds) : b(bounds), W(bounds.width()), H(bounds.height()) {}
  int index(int x, int y) const { return (y - b.y0) * W + (x - b.x0); }
  Cell cell(int i) const { return {b.x0 + i % W, b.y0 + i / W}; }
  std::size_t size() const { return static_cast<std::size_t>(std::max(0, W) * std::max(0, H)); }
};

// Diagonal steps need both orthogonal neighbours open.
inline bool can_step(const WalkFn& w, const GridBounds& b, int x, int y, int dx, int dy) {
  const int nx = x + dx, ny = y + dy;
  if (!b.contains(nx, ny) || !w(nx, ny)) return false;
  if (dx != 0 && dy != 0) {
    if (!b.contains(x + dx, y) || !w(x + dx, y)) return false;
    if (!b.contains(x, y + dy) || !w(x, y + dy)) return false;
  }
  return true;
}

GridPath search(const WalkFn& walk, Cell s, Cell g, const GridOptions& opt, bool heuristic) {
  GridPath out;
  const Window win(opt.bounds);
  if (!win.b.contains(s.x, s.y) || !win.b.contains(g.x, g.y) || !walk(s.x, s.y) || !walk(g.x, g.y)) {
    return out;
  }
  std::vector<float> gs(win.size(), kInf);
  std::vector<int> parent(win.size(), -1);
  std::vector<bool> closed(win.size(), false);
  std::priority_queue<Open> open;
  const int si = win.index(s.x, s.y), gi = win.index(g.x, g.y);
  gs[static_cast<std::size_t>(si)] = 0.0f;
  open.push({heuristic ? octile(s, g) : 0.0f, 0.0f, si});
  const int dirs = opt.diagonal ? 8 : 4;
  while (!open.empty()) {
    const Open cur = open.top();
    open.pop();
    const std::size_t ci = static_cast<std::size_t>(cur.idx);
    if (closed[ci]) continue;
    closed[ci] = true;
    if (++out.expanded > opt.max_expanded) return GridPath{false, {}, 0.0f, out.expanded};
    if (cur.idx == gi) break;
    const Cell c = win.cell(cur.idx);
    for (int d = 0; d < dirs; ++d) {
      if (!can_step(walk, win.b, c.x, c.y, kDx[d], kDy[d])) continue;
      const int nx = c.x + kDx[d], ny = c.y + kDy[d];
      const int ni = win.index(nx, ny);
      if (closed[static_cast<std::size_t>(ni)]) continue;
      float step = d < 4 ? 1.0f : kSqrt2;
      if (opt.cost) step *= std::max(1.0f, opt.cost(nx, ny));
      const float ng = gs[ci] + step;
      if (ng < gs[static_cast<std::size_t>(ni)]) {
        gs[static_cast<std::size_t>(ni)] = ng;
        parent[static_cast<std::size_t>(ni)] = cur.idx;
        const float h = heuristic ? octile({nx, ny}, g) : 0.0f;
        open.push({ng + h, h, ni});
      }
    }
  }
  if (!closed[static_cast<std::size_t>(gi)]) return out;
  out.found = true;
  out.cost = gs[static_cast<std::size_t>(gi)];
  for (int i = gi; i >= 0; i = parent[static_cast<std::size_t>(i)]) out.cells.push_back(win.cell(i));
  std::reverse(out.cells.begin(), out.cells.end());
  return out;
}

int sgn(int v) { return (v > 0) - (v < 0); }

}  // namespace

GridBounds GridBounds::around(Cell a, Cell b, int margin) {
  return {std::min(a.x, b.x) - margin, std::min(a.y, b.y) - margin, std::max(a.x, b.x) + margin + 1,
          std::max(a.y, b.y) + margin + 1};
}

float octile(Cell a, Cell b) {
  const int dx = std::abs(a.x - b.x), dy = std::abs(a.y - b.y);
  return static_cast<float>(std::max(dx, dy) - std::min(dx, dy)) + kSqrt2 * static_cast<float>(std::min(dx, dy));
}

float path_length(const std::vector<Cell>& cells) {
  float len = 0.0f;
  for (std::size_t i = 1; i < cells.size(); ++i) len += octile(cells[i - 1], cells[i]);
  return len;
}

GridPath astar(const WalkFn& walkable, Cell start, Cell goal, const GridOptions& opt) {
  return search(walkable, start, goal, opt, true);
}

GridPath dijkstra(const WalkFn& walkable, Cell start, Cell goal, const GridOptions& opt) {
  return search(walkable, start, goal, opt, false);
}

// --- Jump Point Search (no corner cutting) ------------------------------------

namespace {

struct Jps {
  const WalkFn& w;
  GridBounds b;
  Cell goal;
  bool ok(int x, int y) const { return b.contains(x, y) && w(x, y); }

  // Step into (x, y) moving (dx, dy); the next jump point or false.
  bool jump(int x, int y, int dx, int dy, Cell* out) const {
    for (;;) {
      if (!ok(x, y)) return false;
      if (x == goal.x && y == goal.y) {
        *out = {x, y};
        return true;
      }
      if (dx != 0 && dy != 0) {
        Cell tmp;
        if (jump(x + dx, y, dx, 0, &tmp) || jump(x, y + dy, 0, dy, &tmp)) {
          *out = {x, y};
          return true;
        }
        if (!(ok(x + dx, y) && ok(x, y + dy))) return false;
      } else if (dx != 0) {
        if ((ok(x, y - 1) && !ok(x - dx, y - 1)) || (ok(x, y + 1) && !ok(x - dx, y + 1))) {
          *out = {x, y};
          return true;
        }
      } else {
        if ((ok(x - 1, y) && !ok(x - 1, y - dy)) || (ok(x + 1, y) && !ok(x + 1, y - dy))) {
          *out = {x, y};
          return true;
        }
      }
      x += dx;
      y += dy;
    }
  }

  void neighbours(Cell c, int parent_dx, int parent_dy, bool has_parent, std::vector<std::pair<int, int>>* out) const {
    out->clear();
    if (!has_parent) {
      for (int d = 0; d < 8; ++d) {
        if (can_step(w, b, c.x, c.y, kDx[d], kDy[d])) out->push_back({kDx[d], kDy[d]});
      }
      return;
    }
    const int dx = parent_dx, dy = parent_dy;
    const int x = c.x, y = c.y;
    if (dx != 0 && dy != 0) {
      const bool v = ok(x, y + dy), h = ok(x + dx, y);
      if (v) out->push_back({0, dy});
      if (h) out->push_back({dx, 0});
      if (v && h && ok(x + dx, y + dy)) out->push_back({dx, dy});
    } else if (dx != 0) {
      const bool next = ok(x + dx, y), up = ok(x, y + 1), down = ok(x, y - 1);
      if (next) {
        out->push_back({dx, 0});
        if (up && ok(x + dx, y + 1)) out->push_back({dx, 1});
        if (down && ok(x + dx, y - 1)) out->push_back({dx, -1});
      }
      if (up) out->push_back({0, 1});
      if (down) out->push_back({0, -1});
    } else {
      const bool next = ok(x, y + dy), right = ok(x + 1, y), left = ok(x - 1, y);
      if (next) {
        out->push_back({0, dy});
        if (right && ok(x + 1, y + dy)) out->push_back({1, dy});
        if (left && ok(x - 1, y + dy)) out->push_back({-1, dy});
      }
      if (right) out->push_back({1, 0});
      if (left) out->push_back({-1, 0});
    }
  }
};

}  // namespace

GridPath jps(const WalkFn& walkable, Cell start, Cell goal, const GridOptions& opt) {
  GridPath out;
  const Window win(opt.bounds);
  if (!win.b.contains(start.x, start.y) || !win.b.contains(goal.x, goal.y) || !walkable(start.x, start.y) ||
      !walkable(goal.x, goal.y)) {
    return out;
  }
  const Jps J{walkable, opt.bounds, goal};
  std::vector<float> gs(win.size(), kInf);
  std::vector<int> parent(win.size(), -1);
  std::vector<bool> closed(win.size(), false);
  std::priority_queue<Open> open;
  const int si = win.index(start.x, start.y), gi = win.index(goal.x, goal.y);
  gs[static_cast<std::size_t>(si)] = 0.0f;
  open.push({octile(start, goal), 0.0f, si});
  std::vector<std::pair<int, int>> dirs;
  while (!open.empty()) {
    const Open cur = open.top();
    open.pop();
    const std::size_t ci = static_cast<std::size_t>(cur.idx);
    if (closed[ci]) continue;
    closed[ci] = true;
    if (++out.expanded > opt.max_expanded) return GridPath{false, {}, 0.0f, out.expanded};
    if (cur.idx == gi) break;
    const Cell c = win.cell(cur.idx);
    const int pi = parent[ci];
    int pdx = 0, pdy = 0;
    if (pi >= 0) {
      const Cell p = win.cell(pi);
      pdx = sgn(c.x - p.x);
      pdy = sgn(c.y - p.y);
    }
    J.neighbours(c, pdx, pdy, pi >= 0, &dirs);
    for (const auto& [dx, dy] : dirs) {
      Cell jp;
      if (!J.jump(c.x + dx, c.y + dy, dx, dy, &jp)) continue;
      const int ni = win.index(jp.x, jp.y);
      if (closed[static_cast<std::size_t>(ni)]) continue;
      const float ng = gs[ci] + octile(c, jp);
      if (ng < gs[static_cast<std::size_t>(ni)]) {
        gs[static_cast<std::size_t>(ni)] = ng;
        parent[static_cast<std::size_t>(ni)] = cur.idx;
        const float h = octile(jp, goal);
        open.push({ng + h, h, ni});
      }
    }
  }
  if (!closed[static_cast<std::size_t>(gi)]) return out;
  out.found = true;
  out.cost = gs[static_cast<std::size_t>(gi)];
  std::vector<Cell> jumps;
  for (int i = gi; i >= 0; i = parent[static_cast<std::size_t>(i)]) jumps.push_back(win.cell(i));
  std::reverse(jumps.begin(), jumps.end());
  // Expand jump points into every cell (segments are straight or 45 deg).
  out.cells.push_back(jumps.front());
  for (std::size_t k = 1; k < jumps.size(); ++k) {
    Cell c = jumps[k - 1];
    const int dx = sgn(jumps[k].x - c.x), dy = sgn(jumps[k].y - c.y);
    while (!(c == jumps[k])) {
      c.x += dx;
      c.y += dy;
      out.cells.push_back(c);
    }
  }
  return out;
}

// --- Smoothing ----------------------------------------------------------------------

bool line_walkable(const WalkFn& w, Cell a, Cell b) {
  // Walk the supercover of the segment between cell centres; diagonal
  // crossings need both side cells (no corner cutting).
  int x = a.x, y = a.y;
  const int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y);
  const int sx = sgn(b.x - a.x), sy = sgn(b.y - a.y);
  if (!w(x, y)) return false;
  int ix = 0, iy = 0;
  while (ix < dx || iy < dy) {
    const long lhs = static_cast<long>(1 + 2 * ix) * dy;
    const long rhs = static_cast<long>(1 + 2 * iy) * dx;
    if (lhs == rhs) {
      if (!w(x + sx, y) || !w(x, y + sy)) return false;
      x += sx;
      y += sy;
      ++ix;
      ++iy;
    } else if (lhs < rhs) {
      x += sx;
      ++ix;
    } else {
      y += sy;
      ++iy;
    }
    if (!w(x, y)) return false;
  }
  return true;
}

std::vector<Cell> smooth_path(const WalkFn& w, const std::vector<Cell>& cells) {
  if (cells.size() < 3) return cells;
  std::vector<Cell> out{cells.front()};
  std::size_t anchor = 0;
  for (std::size_t i = 2; i < cells.size(); ++i) {
    if (!line_walkable(w, cells[anchor], cells[i])) {
      out.push_back(cells[i - 1]);
      anchor = i - 1;
    }
  }
  out.push_back(cells.back());
  return out;
}

// --- HPA* ---------------------------------------------------------------------------

namespace {
inline int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline std::int64_t cell_key(Cell c) {
  return (static_cast<std::int64_t>(c.x) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(c.y));
}
}  // namespace

HierGrid::HierGrid(WalkFn walkable, int cluster) : walk_(std::move(walkable)), cluster_(std::max(4, cluster)) {}

void HierGrid::set_walkable(WalkFn walkable) {
  walk_ = std::move(walkable);
  clear();
}

void HierGrid::clear() {
  clusters_.clear();
  borders_.clear();
  nodes_.clear();
  node_index_.clear();
  node_dead_.clear();
}

HierGrid::Key HierGrid::cluster_of(Cell c) const { return {floordiv(c.x, cluster_), floordiv(c.y, cluster_)}; }

GridBounds HierGrid::cluster_bounds(Key k) const {
  return {k.first * cluster_, k.second * cluster_, (k.first + 1) * cluster_, (k.second + 1) * cluster_};
}

std::size_t HierGrid::node_at(Cell c) {
  const auto it = node_index_.find(cell_key(c));
  if (it != node_index_.end() && !node_dead_[it->second]) return it->second;
  nodes_.push_back(Node{c, {}, {}});
  node_dead_.push_back(false);
  node_index_[cell_key(c)] = nodes_.size() - 1;
  Cluster& cl = clusters_[cluster_of(c)];
  cl.nodes.push_back(nodes_.size() - 1);
  cl.linked = false;
  return nodes_.size() - 1;
}

// Entrances on the border between a and its right (vertical border) or
// lower neighbour b.
void HierGrid::border(Key a, Key b, bool vertical) {
  const auto bk = std::make_pair(a, b);
  if (borders_.count(bk)) return;
  borders_[bk] = true;
  const GridBounds ab = cluster_bounds(a);
  auto pair_at = [&](int i, Cell* ca, Cell* cb) {
    if (vertical) {
      *ca = {ab.x1 - 1, ab.y0 + i};
      *cb = {ab.x1, ab.y0 + i};
    } else {
      *ca = {ab.x0 + i, ab.y1 - 1};
      *cb = {ab.x0 + i, ab.y1};
    }
  };
  auto open_at = [&](int i) {
    Cell ca, cb;
    pair_at(i, &ca, &cb);
    return walk_(ca.x, ca.y) && walk_(cb.x, cb.y);
  };
  int i = 0;
  while (i < cluster_) {
    if (!open_at(i)) {
      ++i;
      continue;
    }
    int j = i;
    while (j + 1 < cluster_ && open_at(j + 1)) ++j;
    std::vector<int> picks;
    if (j - i + 1 <= 5) {
      picks.push_back((i + j) / 2);
    } else {
      picks.push_back(i);
      picks.push_back(j);
    }
    for (int p : picks) {
      Cell ca, cb;
      pair_at(p, &ca, &cb);
      const std::size_t na = node_at(ca), nb = node_at(cb);
      if (std::find(nodes_[na].pairs.begin(), nodes_[na].pairs.end(), nb) == nodes_[na].pairs.end()) {
        nodes_[na].pairs.push_back(nb);
        nodes_[nb].pairs.push_back(na);
      }
    }
    i = j + 1;
  }
}

HierGrid::Cluster& HierGrid::ensure(Key k) {
  border(k, {k.first + 1, k.second}, true);
  border({k.first - 1, k.second}, k, true);
  border(k, {k.first, k.second + 1}, false);
  border({k.first, k.second - 1}, k, false);
  return clusters_[k];
}

void HierGrid::link(Key k) {
  Cluster& cl = ensure(k);
  if (cl.linked) return;
  GridOptions o;
  o.bounds = cluster_bounds(k);
  for (std::size_t ai = 0; ai < cl.nodes.size(); ++ai) nodes_[cl.nodes[ai]].edges.clear();
  for (std::size_t ai = 0; ai < cl.nodes.size(); ++ai) {
    for (std::size_t bi = ai + 1; bi < cl.nodes.size(); ++bi) {
      const std::size_t na = cl.nodes[ai], nb = cl.nodes[bi];
      if (node_dead_[na] || node_dead_[nb]) continue;
      const GridPath p = astar(walk_, nodes_[na].cell, nodes_[nb].cell, o);
      if (!p.found) continue;
      std::vector<Cell> fwd(p.cells.begin() + 1, p.cells.end());
      std::vector<Cell> back(p.cells.rbegin() + 1, p.cells.rend());
      nodes_[na].edges.push_back({nb, p.cost, std::move(fwd)});
      nodes_[nb].edges.push_back({na, p.cost, std::move(back)});
    }
  }
  cl.linked = true;
}

void HierGrid::invalidate(int x, int y) {
  const Key k = cluster_of({x, y});
  // Drop the cluster's nodes (and their partners' links to them), its four
  // borders, and mark the neighbours for relinking.
  std::vector<Key> touch{k, {k.first + 1, k.second}, {k.first - 1, k.second}, {k.first, k.second + 1},
                         {k.first, k.second - 1}};
  for (const Key& t : touch) {
    auto it = clusters_.find(t);
    if (it == clusters_.end()) continue;
    for (std::size_t n : it->second.nodes) {
      node_dead_[n] = true;
      node_index_.erase(cell_key(nodes_[n].cell));
      nodes_[n].edges.clear();
      nodes_[n].pairs.clear();
    }
    clusters_.erase(it);
  }
  for (auto it = borders_.begin(); it != borders_.end();) {
    const bool hit = std::find(touch.begin(), touch.end(), it->first.first) != touch.end() ||
                     std::find(touch.begin(), touch.end(), it->first.second) != touch.end();
    it = hit ? borders_.erase(it) : std::next(it);
  }
  // Partners outside the touched set lose their pairs into it.
  for (auto& [key, cl] : clusters_) {
    for (std::size_t n : cl.nodes) {
      auto& pr = nodes_[n].pairs;
      pr.erase(std::remove_if(pr.begin(), pr.end(), [&](std::size_t m) { return node_dead_[m]; }), pr.end());
    }
  }
}

GridPath HierGrid::find(Cell s, Cell g, int max_radius) {
  last_abstract_ = 0;
  GridPath out;
  if (!walk_(s.x, s.y) || !walk_(g.x, g.y)) return out;
  const Key ks = cluster_of(s), kg = cluster_of(g);
  if (std::abs(ks.first - kg.first) <= 1 && std::abs(ks.second - kg.second) <= 1) {
    GridOptions o;
    o.bounds = GridBounds::around(s, g, cluster_);
    out = astar(walk_, s, g, o);
    if (out.found) return out;
  }
  // Temporary links: start -> its cluster's entrances, goal's entrances -> goal.
  link(ks);
  link(kg);
  struct Link {
    std::size_t node;
    float cost;
    std::vector<Cell> path;  // cells after the first up to the last
  };
  auto local_links = [&](Cell c, Key k, bool from_c) {
    std::vector<Link> links;
    GridOptions o;
    o.bounds = cluster_bounds(k);
    for (std::size_t n : clusters_[k].nodes) {
      if (node_dead_[n]) continue;
      const GridPath p = from_c ? astar(walk_, c, nodes_[n].cell, o) : astar(walk_, nodes_[n].cell, c, o);
      if (!p.found) continue;
      links.push_back({n, p.cost, std::vector<Cell>(p.cells.begin() + 1, p.cells.end())});
    }
    return links;
  };
  const std::vector<Link> from_start = local_links(s, ks, true);
  const std::vector<Link> to_goal = local_links(g, kg, false);
  if (from_start.empty() || to_goal.empty()) return out;

  // Abstract A*: nodes are entrance indices; the goal is a virtual node.
  const std::size_t kGoal = static_cast<std::size_t>(-1);
  std::unordered_map<std::size_t, float> gs;
  std::unordered_map<std::size_t, std::pair<std::size_t, int>> parent;  // prev node, how (-1 start, edge idx, -2 pair, -3 goal)
  struct Q {
    float f;
    std::size_t n;
    bool operator<(const Q& o) const { return f != o.f ? f > o.f : n > o.n; }
  };
  std::priority_queue<Q> open;
  for (std::size_t i = 0; i < from_start.size(); ++i) {
    const Link& l = from_start[i];
    if (!gs.count(l.node) || l.cost < gs[l.node]) {
      gs[l.node] = l.cost;
      parent[l.node] = {static_cast<std::size_t>(-2), static_cast<int>(i)};
      open.push({l.cost + octile(nodes_[l.node].cell, g), l.node});
    }
  }
  std::unordered_map<std::size_t, int> goal_link;  // entrance -> to_goal index
  for (std::size_t i = 0; i < to_goal.size(); ++i) goal_link[to_goal[i].node] = static_cast<int>(i);
  float best_goal = kInf;
  std::size_t best_from = 0;
  std::unordered_map<std::size_t, bool> closed;
  const GridBounds limit = GridBounds::around(s, g, max_radius);
  while (!open.empty()) {
    const Q q = open.top();
    open.pop();
    if (q.f >= best_goal) break;
    if (closed[q.n]) continue;
    closed[q.n] = true;
    if (++last_abstract_ > 200000) break;
    const float gn = gs[q.n];
    if (auto it = goal_link.find(q.n); it != goal_link.end()) {
      const float c = gn + to_goal[static_cast<std::size_t>(it->second)].cost;
      if (c < best_goal) {
        best_goal = c;
        best_from = q.n;
      }
    }
    const Key kn = cluster_of(nodes_[q.n].cell);
    link(kn);
    auto relax = [&](std::size_t to, float cost, std::size_t from, int how) {
      if (node_dead_[to] || !limit.contains(nodes_[to].cell.x, nodes_[to].cell.y)) return;
      const float ng = gn + cost;
      auto it = gs.find(to);
      if (it == gs.end() || ng < it->second) {
        gs[to] = ng;
        parent[to] = {from, how};
        open.push({ng + octile(nodes_[to].cell, g), to});
      }
    };
    // Copy: link() of a neighbour cluster can grow nodes_.
    const std::vector<Edge> edges = nodes_[q.n].edges;
    const std::vector<std::size_t> pairs = nodes_[q.n].pairs;
    for (std::size_t e = 0; e < edges.size(); ++e) relax(edges[e].to, edges[e].cost, q.n, static_cast<int>(e));
    for (std::size_t p : pairs) {
      ensure(cluster_of(nodes_[p].cell));
      relax(p, 1.0f, q.n, -1);
    }
  }
  if (best_goal == kInf) return out;
  // Rebuild: goal link, then walk parents back to a start link.
  std::vector<std::vector<Cell>> pieces;
  pieces.push_back(to_goal[static_cast<std::size_t>(goal_link[best_from])].path);
  std::size_t n = best_from;
  for (;;) {
    const auto [prev, how] = parent[n];
    if (prev == static_cast<std::size_t>(-2)) {
      pieces.push_back(from_start[static_cast<std::size_t>(how)].path);
      break;
    }
    if (how == -1) {
      pieces.push_back({nodes_[n].cell});
    } else {
      pieces.push_back(nodes_[prev].edges[static_cast<std::size_t>(how)].path);
    }
    n = prev;
  }
  (void)kGoal;
  out.cells.push_back(s);
  for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
    for (const Cell& c : *it) out.cells.push_back(c);
  }
  out.found = true;
  out.cost = path_length(out.cells);
  out.expanded = last_abstract_;
  return out;
}

// --- Flow field ---------------------------------------------------------------------

void FlowField::build(const WalkFn& w, const GridBounds& bounds, const std::vector<Cell>& goals, bool diagonal) {
  bounds_ = bounds;
  const Window win(bounds);
  dist_.assign(win.size(), kInf);
  dir_.assign(win.size(), -1);
  reached_ = 0;
  struct Q {
    float d;
    int i;
    bool operator<(const Q& o) const { return d != o.d ? d > o.d : i > o.i; }
  };
  std::priority_queue<Q> open;
  for (const Cell& g : goals) {
    if (!bounds.contains(g.x, g.y) || !w(g.x, g.y)) continue;
    const int i = win.index(g.x, g.y);
    dist_[static_cast<std::size_t>(i)] = 0.0f;
    open.push({0.0f, i});
  }
  std::vector<bool> done(win.size(), false);
  const int dirs = diagonal ? 8 : 4;
  while (!open.empty()) {
    const Q q = open.top();
    open.pop();
    if (done[static_cast<std::size_t>(q.i)]) continue;
    done[static_cast<std::size_t>(q.i)] = true;
    ++reached_;
    const Cell c = win.cell(q.i);
    for (int d = 0; d < dirs; ++d) {
      // Reverse edges: from the neighbour toward c must be a legal step.
      const int nx = c.x + kDx[d], ny = c.y + kDy[d];
      if (!can_step(w, bounds, c.x, c.y, kDx[d], kDy[d])) continue;
      const int ni = win.index(nx, ny);
      const float nd = q.d + (d < 4 ? 1.0f : kSqrt2);
      if (nd < dist_[static_cast<std::size_t>(ni)]) {
        dist_[static_cast<std::size_t>(ni)] = nd;
        open.push({nd, ni});
      }
    }
  }
  // Each cell points at its best neighbour.
  for (int i = 0; i < static_cast<int>(win.size()); ++i) {
    const float here = dist_[static_cast<std::size_t>(i)];
    if (here == kInf || here == 0.0f) continue;
    const Cell c = win.cell(i);
    float best = here;
    for (int d = 0; d < dirs; ++d) {
      if (!can_step(w, bounds, c.x, c.y, kDx[d], kDy[d])) continue;
      const float nd = dist_[static_cast<std::size_t>(win.index(c.x + kDx[d], c.y + kDy[d]))];
      if (nd < best - 1e-5f) {
        best = nd;
        dir_[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(d);
      }
    }
  }
}

bool FlowField::reachable(int x, int y) const { return distance(x, y) != kInf; }

float FlowField::distance(int x, int y) const {
  if (!bounds_.contains(x, y) || dist_.empty()) return kInf;
  return dist_[static_cast<std::size_t>((y - bounds_.y0) * bounds_.width() + (x - bounds_.x0))];
}

void FlowField::direction(int x, int y, float* dx, float* dy) const {
  *dx = *dy = 0.0f;
  if (!bounds_.contains(x, y) || dir_.empty()) return;
  const int d = dir_[static_cast<std::size_t>((y - bounds_.y0) * bounds_.width() + (x - bounds_.x0))];
  if (d < 0) return;
  const float inv = d < 4 ? 1.0f : 1.0f / kSqrt2;
  *dx = static_cast<float>(kDx[d]) * inv;
  *dy = static_cast<float>(kDy[d]) * inv;
}

}  // namespace ai
}  // namespace ts
