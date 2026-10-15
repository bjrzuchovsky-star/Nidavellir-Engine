#include "scene/AutoTile.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {

namespace fs = std::filesystem;

const char* to_string(AutoTileMode m) {
  switch (m) {
    case AutoTileMode::Blob47: return "blob47";
    case AutoTileMode::Wang16: return "wang16";
    case AutoTileMode::Rules: return "rules";
  }
  return "blob47";
}

bool autotile_mode_from_string(const std::string& s, AutoTileMode* out) {
  if (s == "blob47" || s == "blob") *out = AutoTileMode::Blob47;
  else if (s == "wang16" || s == "wang") *out = AutoTileMode::Wang16;
  else if (s == "rules") *out = AutoTileMode::Rules;
  else return false;
  return true;
}

bool TileRule::matches(std::uint8_t m) const {
  // Pattern cells in reading order, minus the centre.
  static const std::uint8_t bits[9] = {kNbNW, kNbN, kNbNE, kNbW, 0, kNbE, kNbSW, kNbS, kNbSE};
  int cell = 0;
  for (char c : pattern) {
    if (c == '/' || c == ' ') continue;
    if (cell >= 9) break;
    const std::uint8_t b = bits[cell++];
    if (!b) continue;
    if (c == 'x' && !(m & b)) return false;
    if (c == 'o' && (m & b)) return false;
  }
  return true;
}

std::uint8_t blob_reduce(std::uint8_t m) {
  if (!((m & kNbN) && (m & kNbE))) m &= static_cast<std::uint8_t>(~kNbNE);
  if (!((m & kNbS) && (m & kNbE))) m &= static_cast<std::uint8_t>(~kNbSE);
  if (!((m & kNbS) && (m & kNbW))) m &= static_cast<std::uint8_t>(~kNbSW);
  if (!((m & kNbN) && (m & kNbW))) m &= static_cast<std::uint8_t>(~kNbNW);
  return m;
}

const std::array<std::uint8_t, 47>& blob47_masks() {
  static const std::array<std::uint8_t, 47> masks = [] {
    std::array<std::uint8_t, 47> a{};
    bool seen[256] = {};
    int n = 0;
    for (int m = 0; m < 256; ++m) {
      const std::uint8_t r = blob_reduce(static_cast<std::uint8_t>(m));
      if (!seen[r]) {
        seen[r] = true;
        if (n < 47) a[static_cast<std::size_t>(n)] = r;
        ++n;
      }
    }
    std::sort(a.begin(), a.end());
    return a;
  }();
  return masks;
}

int blob47_slot(std::uint8_t mask8) {
  const auto& a = blob47_masks();
  const std::uint8_t r = blob_reduce(mask8);
  return static_cast<int>(std::lower_bound(a.begin(), a.end(), r) - a.begin());
}

int wang16_slot(std::uint8_t m) {
  return ((m & kNbN) ? 1 : 0) | ((m & kNbE) ? 2 : 0) | ((m & kNbS) ? 4 : 0) | ((m & kNbW) ? 8 : 0);
}

int AutoTileSet::slot_count() const {
  switch (mode) {
    case AutoTileMode::Blob47: return 47;
    case AutoTileMode::Wang16: return 16;
    case AutoTileMode::Rules: return static_cast<int>(rules.size());
  }
  return 0;
}

int AutoTileSet::slot_tile(int slot) const {
  if (mode == AutoTileMode::Rules) {
    return slot >= 0 && slot < static_cast<int>(rules.size()) ? rules[static_cast<std::size_t>(slot)].tile : 0;
  }
  if (slot >= 0 && slot < static_cast<int>(map.size()) && map[static_cast<std::size_t>(slot)] > 0) {
    return map[static_cast<std::size_t>(slot)];
  }
  return base + slot;
}

bool AutoTileSet::member(int id) const {
  if (id <= 0) return false;
  if (std::find(members.begin(), members.end(), id) != members.end()) return true;
  if (mode == AutoTileMode::Rules) {
    if (id == fill) return true;
    for (const TileRule& r : rules) {
      if (r.tile == id) return true;
    }
    return false;
  }
  if (map.empty()) return id >= base && id < base + slot_count();
  for (int i = 0; i < slot_count(); ++i) {
    if (slot_tile(i) == id) return true;
  }
  return false;
}

int AutoTileSet::resolve(std::uint8_t mask8) const {
  switch (mode) {
    case AutoTileMode::Blob47: return slot_tile(blob47_slot(mask8));
    case AutoTileMode::Wang16: return slot_tile(wang16_slot(mask8));
    case AutoTileMode::Rules:
      for (const TileRule& r : rules) {
        if (r.matches(mask8)) return r.tile;
      }
      return fill > 0 ? fill : (rules.empty() ? 0 : rules.front().tile);
  }
  return 0;
}

std::uint8_t neighbour_mask(const TileMapData& map, const AutoTileSet& set, int col, int row) {
  static const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  static const int dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
  std::uint8_t m = 0;
  for (int i = 0; i < 8; ++i) {
    const int c = col + dx[i], r = row + dy[i];
    const bool on = map.in_bounds(c, r) ? set.member(map.at(c, r)) : set.edges_connect;
    if (on) m |= static_cast<std::uint8_t>(1u << i);
  }
  return m;
}

std::size_t retile(TileMapData& map, const AutoTileSet& set, int c0, int r0, int c1, int r1) {
  c0 = std::max(c0, 0);
  r0 = std::max(r0, 0);
  c1 = std::min(c1, map.cols - 1);
  r1 = std::min(r1, map.rows - 1);
  // Decide from a snapshot so the result does not depend on scan order.
  std::vector<int> want;
  for (int r = r0; r <= r1; ++r) {
    for (int c = c0; c <= c1; ++c) {
      want.push_back(set.member(map.at(c, r)) ? set.resolve(neighbour_mask(map, set, c, r)) : -1);
    }
  }
  std::size_t changed = 0, i = 0;
  for (int r = r0; r <= r1; ++r) {
    for (int c = c0; c <= c1; ++c, ++i) {
      if (want[i] > 0 && map.set(c, r, want[i])) ++changed;
    }
  }
  return changed;
}

std::size_t retile_all(TileMapData& map, const AutoTileSet& set) {
  return retile(map, set, 0, 0, map.cols - 1, map.rows - 1);
}

std::size_t paint_autotile(TileMapData& map, const AutoTileSet& set, int col, int row, int brush,
                           bool erase) {
  brush = std::clamp(brush, 1, 3);
  const int c0 = col - (brush - 1) / 2, r0 = row - (brush - 1) / 2;
  std::size_t changed = 0;
  const int full = set.full_tile();
  for (int r = r0; r < r0 + brush; ++r) {
    for (int c = c0; c < c0 + brush; ++c) {
      if (!map.in_bounds(c, r)) continue;
      if (erase) {
        if (set.member(map.at(c, r)) && map.set(c, r, 0)) ++changed;
      } else if (!set.member(map.at(c, r)) && map.set(c, r, full)) {
        ++changed;
      }
    }
  }
  changed += retile(map, set, c0 - 1, r0 - 1, c0 + brush, r0 + brush);
  return changed;
}

namespace autotile_json {

bool parse(const std::string& text, AutoTileSet* out, std::string* error) {
  json::Value v;
  if (!json::parse(text, &v, error)) return false;
  if (!v.is_object()) {
    if (error) *error = "an .autotile.json must be an object";
    return false;
  }
  AutoTileSet s;
  s.name = v.str("name", s.name);
  if (!autotile_mode_from_string(v.str("mode", "blob47"), &s.mode)) {
    if (error) *error = "mode must be blob47, wang16 or rules";
    return false;
  }
  s.tileset = v.str("tileset");
  s.tile_size = std::clamp(static_cast<int>(v.num("tile_size", 32)), TileMapData::kMinTileSize,
                           TileMapData::kMaxTileSize);
  s.base = std::max(1, static_cast<int>(v.num("base", 1)));
  s.fill = static_cast<int>(v.num("fill", 0));
  s.edges_connect = v.flag("edges_connect", true);
  for (const json::Value& m : v["map"].items()) s.map.push_back(static_cast<int>(m.as_number()));
  for (const json::Value& m : v["members"].items()) s.members.push_back(static_cast<int>(m.as_number()));
  for (const json::Value& r : v["rules"].items()) {
    TileRule tr;
    tr.tile = static_cast<int>(r.num("tile", 0));
    tr.pattern = r.str("pattern", tr.pattern);
    if (tr.tile > 0) s.rules.push_back(tr);
  }
  if (s.mode == AutoTileMode::Rules && s.rules.empty()) {
    if (error) *error = "rules mode needs at least one rule";
    return false;
  }
  *out = std::move(s);
  return true;
}

std::string write(const AutoTileSet& s) {
  json::Value v = json::Value::object();
  v.set("name", json::Value::string(s.name));
  v.set("mode", json::Value::string(to_string(s.mode)));
  v.set("tileset", json::Value::string(s.tileset));
  v.set("tile_size", json::Value::number(s.tile_size));
  if (s.mode != AutoTileMode::Rules) v.set("base", json::Value::number(s.base));
  if (!s.map.empty()) {
    json::Value a = json::Value::array();
    for (int id : s.map) a.push(json::Value::number(id));
    v.set("map", std::move(a));
  }
  if (s.mode == AutoTileMode::Rules) {
    json::Value a = json::Value::array();
    for (const TileRule& r : s.rules) {
      json::Value o = json::Value::object();
      o.set("tile", json::Value::number(r.tile));
      o.set("pattern", json::Value::string(r.pattern));
      a.push(std::move(o));
    }
    v.set("rules", std::move(a));
    if (s.fill > 0) v.set("fill", json::Value::number(s.fill));
  }
  if (!s.members.empty()) {
    json::Value a = json::Value::array();
    for (int id : s.members) a.push(json::Value::number(id));
    v.set("members", std::move(a));
  }
  if (!s.edges_connect) v.set("edges_connect", json::Value::boolean(false));
  return json::write(v);
}

bool load_file(const std::string& path, AutoTileSet* out, std::string* error) {
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse(ss.str(), out, error);
}

bool save_file(const std::string& path, const AutoTileSet& set, std::string* error) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream o(fs::path(path), std::ios::binary | std::ios::trunc);
  o << write(set);
  if (!o) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  return true;
}

std::vector<std::string> list(const std::string& project_dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(project_dir) / "tiles", ec)) {
    const std::string n = e.path().filename().string();
    if (n.size() > 14 && n.compare(n.size() - 14, 14, ".autotile.json") == 0) out.push_back("tiles/" + n);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace autotile_json

TemplateImage autotile_template(AutoTileMode mode, int t, const unsigned char fill[4],
                                const unsigned char rim[4]) {
  TemplateImage img;
  const int slots = mode == AutoTileMode::Wang16 ? 16 : 47;
  img.cols = mode == AutoTileMode::Wang16 ? 4 : 8;
  const int rows = (slots + img.cols - 1) / img.cols;
  img.width = img.cols * t;
  img.height = rows * t;
  img.rgba.assign(static_cast<std::size_t>(img.width) * img.height * 4, 0);
  const int q = std::max(2, t / 4);       // edge band
  const int rimw = std::max(1, t / 16);   // rim thickness
  for (int s = 0; s < slots; ++s) {
    const std::uint8_t m = mode == AutoTileMode::Wang16
                               ? static_cast<std::uint8_t>(((s & 1) ? kNbN : 0) | ((s & 2) ? kNbE : 0) |
                                                           ((s & 4) ? kNbS : 0) | ((s & 8) ? kNbW : 0))
                               : blob47_masks()[static_cast<std::size_t>(s)];
    const int ox = (s % img.cols) * t, oy = (s / img.cols) * t;
    auto inside = [&](int x, int y) {
      const bool l = x < q, r = x >= t - q, u = y < q, d = y >= t - q;
      if (!l && !r && !u && !d) return true;
      if (u && l) return (m & kNbN) && (m & kNbW) && (mode == AutoTileMode::Wang16 || (m & kNbNW));
      if (u && r) return (m & kNbN) && (m & kNbE) && (mode == AutoTileMode::Wang16 || (m & kNbNE));
      if (d && l) return (m & kNbS) && (m & kNbW) && (mode == AutoTileMode::Wang16 || (m & kNbSW));
      if (d && r) return (m & kNbS) && (m & kNbE) && (mode == AutoTileMode::Wang16 || (m & kNbSE));
      if (u) return (m & kNbN) != 0;
      if (d) return (m & kNbS) != 0;
      if (l) return (m & kNbW) != 0;
      return (m & kNbE) != 0;
    };
    for (int y = 0; y < t; ++y) {
      for (int x = 0; x < t; ++x) {
        if (!inside(x, y)) continue;
        bool edge = false;
        for (int k = 1; k <= rimw && !edge; ++k) {
          edge = (x - k >= 0 && !inside(x - k, y)) || (x + k < t && !inside(x + k, y)) ||
                 (y - k >= 0 && !inside(x, y - k)) || (y + k < t && !inside(x, y + k));
        }
        // Light speckle so repeated fills read as texture.
        const int speck = ((x * 7 + y * 13 + s * 3) % 17 == 0) ? 18 : 0;
        unsigned char* p = &img.rgba[(static_cast<std::size_t>(oy + y) * img.width + (ox + x)) * 4];
        const unsigned char* c = edge ? rim : fill;
        for (int k = 0; k < 3; ++k) p[k] = static_cast<unsigned char>(std::min(255, c[k] + (edge ? 0 : speck)));
        p[3] = c[3];
      }
    }
  }
  return img;
}

}  // namespace tombstone
}  // namespace ts
