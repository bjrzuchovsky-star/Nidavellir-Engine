#include "plugins/shipyard/Design.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace shipyard {

namespace fs = std::filesystem;

void footprint(const PartDef& d, int rot, int* w, int* h) {
  const bool turned = (rot & 1) != 0;
  *w = turned ? d.h : d.w;
  *h = turned ? d.w : d.h;
}

void engine_dir(int rot, float* dx, float* dy) {
  switch (rot & 3) {
    case 0: *dx = 0.0f; *dy = -1.0f; break;
    case 1: *dx = 1.0f; *dy = 0.0f; break;
    case 2: *dx = 0.0f; *dy = 1.0f; break;
    default: *dx = -1.0f; *dy = 0.0f; break;
  }
}

void part_center(const PartDef& d, const PartPose& p, float* cx, float* cy) {
  int w = 1, h = 1;
  footprint(d, p.rot, &w, &h);
  *cx = (static_cast<float>(p.x) + static_cast<float>(w) * 0.5f) * kCell;
  *cy = (static_cast<float>(p.y) + static_cast<float>(h) * 0.5f) * kCell;
}

ShipStats compute_stats(const PartLibrary& lib, const std::vector<PartPose>& parts) {
  ShipStats s;
  float mx = 0.0f, my = 0.0f;
  for (const PartPose& p : parts) {
    const PartDef* d = lib.get(p.def);
    if (!d) continue;
    float cx = 0, cy = 0;
    part_center(*d, p, &cx, &cy);
    s.mass += d->mass;
    mx += d->mass * cx;
    my += d->mass * cy;
    ++s.parts;
    s.cells += d->w * d->h;
    s.hp += d->hp;
    if (d->power > 0) s.power_made += d->power;
    else s.power_used -= d->power;
    if (d->crew > 0) s.berths += d->crew;
    else s.hands -= d->crew;
    if (d->kind == PartKind::Gun) {
      ++s.guns;
      s.dps += static_cast<float>(d->damage) * d->fire_rate;
    }
    s.shield += d->shield;
    s.capacity += d->capacity;
  }
  if (s.mass <= 0.0f) return s;
  s.cx = mx / s.mass;
  s.cy = my / s.mass;
  for (const PartPose& p : parts) {
    const PartDef* d = lib.get(p.def);
    if (!d) continue;
    float cx = 0, cy = 0;
    part_center(*d, p, &cx, &cy);
    int w = 1, h = 1;
    footprint(*d, p.rot, &w, &h);
    const float rx = cx - s.cx, ry = cy - s.cy;
    const float ww = static_cast<float>(w) * kCell, hh = static_cast<float>(h) * kCell;
    s.inertia += d->mass * (rx * rx + ry * ry + (ww * ww + hh * hh) / 12.0f);
    const float far = std::sqrt((std::fabs(rx) + ww * 0.5f) * (std::fabs(rx) + ww * 0.5f) +
                                (std::fabs(ry) + hh * 0.5f) * (std::fabs(ry) + hh * 0.5f));
    s.radius = std::max(s.radius, far);
    if (d->kind == PartKind::Engine && d->thrust > 0.0f) {
      float fx = 0, fy = 0;
      engine_dir(p.rot, &fx, &fy);
      if (fy < 0) s.thrust[kFwd] += d->thrust;
      if (fy > 0) s.thrust[kBack] += d->thrust;
      if (fx < 0) s.thrust[kLeft] += d->thrust;
      if (fx > 0) s.thrust[kRight] += d->thrust;
      const float tq = (rx * fy - ry * fx) * d->thrust;
      if (tq > 0) s.torque_cw += tq;
      if (tq < 0) s.torque_ccw -= tq;
    }
  }
  s.accel = s.thrust[kFwd] / s.mass;
  s.top_speed = s.accel / kLinearDamping;
  return s;
}

std::vector<PartPose> resolve(const PartLibrary& lib, const Design& d,
                              std::vector<std::string>* missing) {
  std::vector<PartPose> out;
  out.reserve(d.parts.size());
  for (const Placed& p : d.parts) {
    PartPose q;
    q.def = lib.find(p.part);
    q.x = p.x;
    q.y = p.y;
    q.rot = p.rot & 3;
    if (q.def < 0 && missing) missing->push_back(p.part);
    out.push_back(q);
  }
  return out;
}

namespace {

// Cell -> part index over the bounding box of the parts.
struct CellMap {
  int x0 = 0, y0 = 0, w = 0, h = 0;
  std::vector<int> cells;
  int at(int x, int y) const {
    if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h) return -1;
    return cells[static_cast<std::size_t>((y - y0) * w + (x - x0))];
  }
};

CellMap build_map(const PartLibrary& lib, const std::vector<PartPose>& parts, int* overlaps) {
  CellMap m;
  int x1 = 0, y1 = 0;
  bool any = false;
  for (const PartPose& p : parts) {
    const PartDef* d = lib.get(p.def);
    if (!d) continue;
    int w = 1, h = 1;
    footprint(*d, p.rot, &w, &h);
    if (!any) {
      m.x0 = p.x;
      m.y0 = p.y;
      x1 = p.x + w;
      y1 = p.y + h;
      any = true;
    }
    m.x0 = std::min(m.x0, p.x);
    m.y0 = std::min(m.y0, p.y);
    x1 = std::max(x1, p.x + w);
    y1 = std::max(y1, p.y + h);
  }
  if (!any) return m;
  m.w = x1 - m.x0;
  m.h = y1 - m.y0;
  m.cells.assign(static_cast<std::size_t>(m.w * m.h), -1);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const PartDef* d = lib.get(parts[i].def);
    if (!d) continue;
    int w = 1, h = 1;
    footprint(*d, parts[i].rot, &w, &h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        int& c = m.cells[static_cast<std::size_t>((parts[i].y + y - m.y0) * m.w + (parts[i].x + x - m.x0))];
        if (c >= 0 && overlaps) ++*overlaps;
        c = static_cast<int>(i);
      }
    }
  }
  return m;
}

}  // namespace

std::vector<std::vector<int>> components(const PartLibrary& lib,
                                         const std::vector<PartPose>& parts) {
  const CellMap m = build_map(lib, parts, nullptr);
  std::vector<int> group(parts.size(), -1);
  std::vector<std::vector<int>> out;
  for (std::size_t start = 0; start < parts.size(); ++start) {
    if (group[start] >= 0 || !lib.get(parts[start].def)) continue;
    const int g = static_cast<int>(out.size());
    out.emplace_back();
    std::vector<int> stack{static_cast<int>(start)};
    group[start] = g;
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      out[static_cast<std::size_t>(g)].push_back(i);
      const PartPose& p = parts[static_cast<std::size_t>(i)];
      int w = 1, h = 1;
      footprint(*lib.get(p.def), p.rot, &w, &h);
      auto visit = [&](int x, int y) {
        const int j = m.at(x, y);
        if (j >= 0 && group[static_cast<std::size_t>(j)] < 0) {
          group[static_cast<std::size_t>(j)] = g;
          stack.push_back(j);
        }
      };
      for (int x = p.x; x < p.x + w; ++x) {
        visit(x, p.y - 1);
        visit(x, p.y + h);
      }
      for (int y = p.y; y < p.y + h; ++y) {
        visit(p.x - 1, y);
        visit(p.x + w, y);
      }
    }
  }
  for (auto& c : out) std::sort(c.begin(), c.end());
  std::stable_sort(out.begin(), out.end(), [](const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() != b.size()) return a.size() > b.size();
    return a.front() < b.front();
  });
  return out;
}

Validation validate(const PartLibrary& lib, const Design& d) {
  Validation v;
  std::vector<std::string> missing;
  const std::vector<PartPose> parts = resolve(lib, d, &missing);
  for (const std::string& m : missing) {
    v.errors.push_back("Unknown part '" + m + "' (not in shipyard/parts.json).");
  }
  if (parts.empty()) {
    v.errors.push_back("Nothing on the slipway yet: lay down some hull.");
    return v;
  }
  int overlaps = 0;
  build_map(lib, parts, &overlaps);
  if (overlaps > 0) v.errors.push_back("Parts overlap (" + std::to_string(overlaps) + " cell(s)).");
  for (const PartPose& p : parts) {
    const PartDef* def = lib.get(p.def);
    if (!def) continue;
    int w = 1, h = 1;
    footprint(*def, p.rot, &w, &h);
    if (p.x < 0 || p.y < 0 || p.x + w > kMaxGrid || p.y + h > kMaxGrid) {
      v.errors.push_back("A " + def->name + " hangs off the slipway.");
      break;
    }
  }
  const auto groups = components(lib, parts);
  if (groups.size() > 1) {
    std::vector<bool> main(parts.size(), false);
    for (int i : groups.front()) main[static_cast<std::size_t>(i)] = true;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (!main[i] && lib.get(parts[i].def)) v.loose.push_back(static_cast<int>(i));
    }
    v.errors.push_back(std::to_string(v.loose.size()) +
                       " part(s) aren't bolted to the rest of the ship.");
  }
  v.stats = compute_stats(lib, parts);
  const ShipStats& s = v.stats;
  if (s.power_used > s.power_made) {
    char b[160];
    std::snprintf(b, sizeof(b), "Short on steam: parts burn %.0f, boilers make %.0f.",
                  static_cast<double>(s.power_used), static_cast<double>(s.power_made));
    v.errors.push_back(b);
  }
  if (s.thrust[kFwd] <= 0.0f) v.errors.push_back("No engine pushes her forward.");
  if (s.thrust[kBack] <= 0.0f) v.warnings.push_back("No engine to back her up (she can't reverse).");
  if (s.thrust[kLeft] <= 0.0f || s.thrust[kRight] <= 0.0f) {
    v.warnings.push_back("No side thrust one way or both (she can't strafe).");
  }
  if (s.torque_cw <= 1.0f || s.torque_ccw <= 1.0f) {
    v.warnings.push_back("Engines can't swing her both ways: put trim jets off the centreline.");
  }
  if (s.hands > s.berths) {
    v.warnings.push_back("Needs " + std::to_string(s.hands) + " hands but bunks " +
                         std::to_string(s.berths) + ".");
  }
  if (s.guns == 0) v.warnings.push_back("Unarmed: no gun turrets.");
  v.ok = v.errors.empty();
  return v;
}

int part_at(const PartLibrary& lib, const Design& d, int cx, int cy) {
  for (std::size_t i = d.parts.size(); i-- > 0;) {
    const Placed& p = d.parts[i];
    const PartDef* def = lib.get(lib.find(p.part));
    if (!def) {
      if (p.x == cx && p.y == cy) return static_cast<int>(i);
      continue;
    }
    int w = 1, h = 1;
    footprint(*def, p.rot, &w, &h);
    if (cx >= p.x && cy >= p.y && cx < p.x + w && cy < p.y + h) return static_cast<int>(i);
  }
  return -1;
}

namespace {

bool fits(const PartLibrary& lib, const Design& d, const PartDef& def, int x, int y, int rot,
          int ignore, std::string* why) {
  int w = 1, h = 1;
  footprint(def, rot, &w, &h);
  if (x < 0 || y < 0 || x + w > kMaxGrid || y + h > kMaxGrid) {
    if (why) *why = "Off the slipway.";
    return false;
  }
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) {
      const int o = part_at(lib, d, xx, yy);
      if (o >= 0 && o != ignore) {
        if (why) *why = "Something's already bolted there.";
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool place_part(const PartLibrary& lib, Design* d, const std::string& part, int x, int y,
                int rot, std::string* why) {
  const PartDef* def = lib.get(lib.find(part));
  if (!def) {
    if (why) *why = "No such part '" + part + "'.";
    return false;
  }
  if (!fits(lib, *d, *def, x, y, rot & 3, -1, why)) return false;
  d->parts.push_back(Placed{part, x, y, rot & 3});
  return true;
}

bool erase_at(const PartLibrary& lib, Design* d, int cx, int cy) {
  const int i = part_at(lib, *d, cx, cy);
  if (i < 0) return false;
  d->parts.erase(d->parts.begin() + i);
  return true;
}

bool rotate_at(const PartLibrary& lib, Design* d, int cx, int cy) {
  const int i = part_at(lib, *d, cx, cy);
  if (i < 0) return false;
  Placed& p = d->parts[static_cast<std::size_t>(i)];
  const PartDef* def = lib.get(lib.find(p.part));
  if (!def) return false;
  const int next = (p.rot + 1) & 3;
  if (!fits(lib, *d, *def, p.x, p.y, next, i, nullptr)) return false;
  p.rot = next;
  return true;
}

std::string write_design(const Design& d) {
  json::Value root = json::Value::object();
  root.set("format", json::Value::string("tombstone.ship"));
  root.set("version", json::Value::number(kDesignVersion));
  root.set("name", json::Value::string(d.name));
  json::Value arr = json::Value::array();
  for (const Placed& p : d.parts) {
    json::Value o = json::Value::object();
    o.set("part", json::Value::string(p.part));
    o.set("x", json::Value::number(p.x));
    o.set("y", json::Value::number(p.y));
    if (p.rot) o.set("rot", json::Value::number(p.rot));
    arr.push(std::move(o));
  }
  root.set("parts", std::move(arr));
  return json::write(root);
}

bool parse_design(const std::string& text, Design* out, std::string* error) {
  json::Value root;
  std::string err;
  if (!json::parse(text, &root, &err)) {
    if (error) *error = err;
    return false;
  }
  if (root.str("format") != "tombstone.ship") {
    if (error) *error = "not a ship design (format != tombstone.ship)";
    return false;
  }
  if (root.num("version", 1) > kDesignVersion) {
    if (error) *error = "ship design from a newer TombStone (version " +
                        std::to_string(static_cast<int>(root.num("version", 1))) + ")";
    return false;
  }
  Design d;
  d.name = root.str("name", d.name);
  for (const json::Value& v : root["parts"].items()) {
    Placed p;
    p.part = v.str("part");
    p.x = static_cast<int>(v.num("x", 0));
    p.y = static_cast<int>(v.num("y", 0));
    p.rot = static_cast<int>(v.num("rot", 0)) & 3;
    if (!p.part.empty()) d.parts.push_back(std::move(p));
  }
  *out = std::move(d);
  return true;
}

bool save_design(const std::string& path, const Design& d, std::string* error) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << write_design(d);
    if (!out) {
      if (error) *error = "could not write " + tmp;
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
  }
  if (ec) {
    if (error) *error = "could not replace " + path + ": " + ec.message();
    return false;
  }
  return true;
}

bool load_design(const std::string& path, Design* out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "could not open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string err;
  if (!parse_design(ss.str(), out, &err)) {
    if (error) *error = path + ": " + err;
    return false;
  }
  return true;
}

std::vector<std::string> list_designs(const std::string& project_dir) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path root(project_dir);
  const fs::path dir = root / "shipyard" / "designs";
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    if (name.size() > 10 && name.compare(name.size() - 10, 10, ".ship.json") == 0) {
      out.push_back(fs::relative(e.path(), root, ec).generic_string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
