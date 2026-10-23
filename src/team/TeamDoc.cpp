#include "team/TeamDoc.h"

#include "scene/JsonFlat.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace ts {
namespace tombstone {
namespace team {

namespace {

void escape(std::string& o, const std::string& s) {
  o.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          o += buf;
        } else {
          o.push_back(c);
        }
    }
  }
  o.push_back('"');
}

void write_into(std::string& o, const Value& v) {
  switch (v.type()) {
    case Value::Type::Null: o += "null"; break;
    case Value::Type::Bool: o += v.as_bool() ? "true" : "false"; break;
    case Value::Type::Number: o += json_flat::format_number(v.as_number()); break;
    case Value::Type::String: escape(o, v.as_string()); break;
    case Value::Type::Array: {
      o.push_back('[');
      bool first = true;
      for (const Value& e : v.items()) {
        if (!first) o.push_back(',');
        first = false;
        write_into(o, e);
      }
      o.push_back(']');
      break;
    }
    case Value::Type::Object: {
      o.push_back('{');
      bool first = true;
      for (const auto& [k, e] : v.members()) {
        if (!first) o.push_back(',');
        first = false;
        escape(o, k);
        o.push_back(':');
        write_into(o, e);
      }
      o.push_back('}');
      break;
    }
  }
}

Value ids_value(const std::vector<std::uint64_t>& ids) {
  Value a = Value::array();
  for (std::uint64_t id : ids) a.push(id_value(id));
  return a;
}

std::vector<std::uint64_t> ids_from(const Value& v) {
  std::vector<std::uint64_t> out;
  for (const Value& e : v.items()) out.push_back(id_of(e));
  return out;
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

Value num(double d) { return Value::number(d); }

}  // namespace

std::string base64_encode(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  const auto* b = reinterpret_cast<const unsigned char*>(in.data());
  for (; i + 2 < in.size(); i += 3) {
    const std::uint32_t v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back(kB64[v & 63]);
  }
  if (i < in.size()) {
    std::uint32_t v = b[i] << 16;
    if (i + 1 < in.size()) v |= b[i + 1] << 8;
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

bool base64_decode(const std::string& text, std::string* out) {
  static int rev[256];
  static bool init = false;
  if (!init) {
    for (int& r : rev) r = -1;
    for (int k = 0; k < 64; ++k) rev[static_cast<unsigned char>(kB64[k])] = k;
    init = true;
  }
  std::string o;
  o.reserve(text.size() / 4 * 3);
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=') break;
    const int v = rev[static_cast<unsigned char>(c)];
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      o.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  *out = std::move(o);
  return true;
}

std::uint64_t TerrainState::hash() const {
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&](const std::string& s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    h = (h ^ 0xFF) * 1099511628211ull;
  };
  mix(std::to_string(res));
  mix(json_flat::format_number(size));
  mix(heights);
  mix(splat);
  mix(write_compact(foliage));
  return h;
}

namespace {

// Copy a grid rect (cell = `stride` bytes) between a full grid and a packed rect.
std::string pack_rect(const std::string& grid, int res, const TerrainRect& r, int stride) {
  std::string out;
  const int w = r.i1 - r.i0 + 1;
  out.reserve(static_cast<std::size_t>(w) * (r.j1 - r.j0 + 1) * stride);
  for (int j = r.j0; j <= r.j1; ++j) {
    out.append(grid, (static_cast<std::size_t>(j) * res + r.i0) * stride, static_cast<std::size_t>(w) * stride);
  }
  return out;
}

bool unpack_rect(std::string& grid, int res, const TerrainRect& r, int stride, const std::string& packed) {
  const int w = r.i1 - r.i0 + 1;
  const std::size_t row = static_cast<std::size_t>(w) * stride;
  if (packed.size() != row * (r.j1 - r.j0 + 1)) return false;
  for (int j = r.j0; j <= r.j1; ++j) {
    grid.replace((static_cast<std::size_t>(j) * res + r.i0) * stride, row, packed, (j - r.j0) * row, row);
  }
  return true;
}

}  // namespace

Value terrain_patch(const TerrainState& t, const TerrainRect& r, bool heights, bool splat, bool foliage) {
  Value o = Value::object();
  o.set("res", num(t.res));
  o.set("size", num(t.size));
  TerrainRect c = r;
  c.i0 = std::max(c.i0, 0);
  c.j0 = std::max(c.j0, 0);
  c.i1 = std::min(c.i1, t.res - 1);
  c.j1 = std::min(c.j1, t.res - 1);
  if ((heights || splat) && c.i0 <= c.i1 && c.j0 <= c.j1 && t.valid()) {
    Value rect = Value::array();
    rect.push(num(c.i0));
    rect.push(num(c.j0));
    rect.push(num(c.i1));
    rect.push(num(c.j1));
    o.set("r", std::move(rect));
    if (heights) o.set("h", Value::string(base64_encode(pack_rect(t.heights, t.res, c, 4))));
    if (splat) o.set("s", Value::string(base64_encode(pack_rect(t.splat, t.res, c, 4))));
  }
  if (foliage) o.set("f", t.foliage);
  return o;
}

Value terrain_full(const TerrainState& t) {
  return terrain_patch(t, TerrainRect{0, 0, t.res - 1, t.res - 1}, true, true, true);
}

bool terrain_patch_rect(const Value& p, TerrainRect* r, bool* heights, bool* splat, bool* foliage) {
  *heights = p.has("h");
  *splat = p.has("s");
  *foliage = p.has("f");
  const Value& rect = p["r"];
  if (rect.size() == 4) {
    r->i0 = static_cast<int>(rect[std::size_t{0}].as_number(0));
    r->j0 = static_cast<int>(rect[std::size_t{1}].as_number(0));
    r->i1 = static_cast<int>(rect[std::size_t{2}].as_number(-1));
    r->j1 = static_cast<int>(rect[std::size_t{3}].as_number(-1));
  } else {
    *heights = *splat = false;
  }
  return *heights || *splat || *foliage;
}

bool apply_terrain_patch(TerrainState& t, const Value& p) {
  const int res = static_cast<int>(p.num("res", 0));
  const float size = static_cast<float>(p.num("size", 0));
  if (res < 2 || res > 4097) return false;
  if (res != t.res) {
    t.res = res;
    const std::size_t n = static_cast<std::size_t>(res) * res * 4;
    t.heights.assign(n, '\0');
    t.splat.assign(n, '\0');
  }
  t.size = size;
  TerrainRect r;
  bool h = false, s = false, f = false;
  terrain_patch_rect(p, &r, &h, &s, &f);
  if (h || s) {
    if (r.i0 < 0 || r.j0 < 0 || r.i1 >= res || r.j1 >= res || r.i0 > r.i1 || r.j0 > r.j1) return false;
    std::string bytes;
    if (h && (!base64_decode(p.str("h"), &bytes) || !unpack_rect(t.heights, res, r, 4, bytes))) return false;
    if (s && (!base64_decode(p.str("s"), &bytes) || !unpack_rect(t.splat, res, r, 4, bytes))) return false;
  }
  if (f) t.foliage = p["f"];
  return true;
}

std::string write_compact(const Value& v) {
  std::string o;
  write_into(o, v);
  return o;
}

bool same_json(const Value& a, const Value& b) { return write_compact(a) == write_compact(b); }

Op Op::terrain(std::string path, std::uint64_t entity, Value patch) {
  Op op;
  op.kind = Kind::Terrain;
  op.key = std::move(path);
  op.id = entity;
  op.v = std::move(patch);
  return op;
}

Op Op::put(std::uint64_t id, Value e) {
  Op op;
  op.kind = Kind::Put;
  op.id = id;
  op.v = std::move(e);
  return op;
}
Op Op::del(std::uint64_t id) {
  Op op;
  op.kind = Kind::Del;
  op.id = id;
  return op;
}
Op Op::order(std::vector<std::uint64_t> ids) {
  Op op;
  op.kind = Kind::Order;
  op.ids = std::move(ids);
  return op;
}
Op Op::set(std::string key, Value v) {
  Op op;
  op.kind = Kind::Set;
  op.key = std::move(key);
  op.v = std::move(v);
  return op;
}
Op Op::asset(std::string path, std::string sha256, std::uint64_t size) {
  Op op;
  op.kind = Kind::Asset;
  op.key = std::move(path);
  op.sha256 = std::move(sha256);
  op.size = size;
  return op;
}

std::string Op::touch_key() const {
  switch (kind) {
    case Kind::Put:
    case Kind::Del: return "e:" + std::to_string(id);
    case Kind::Order: return "order";
    case Kind::Set: return "p:" + key;
    case Kind::Asset: return "a:" + key;
    case Kind::Terrain: return "t:" + key;
  }
  return {};
}

int SceneDoc::index_of(std::uint64_t id) const {
  for (std::size_t i = 0; i < entities.size(); ++i) {
    if (entities[i].first == id) return static_cast<int>(i);
  }
  return -1;
}

const Value* SceneDoc::entity(std::uint64_t id) const {
  const int i = index_of(id);
  return i < 0 ? nullptr : &entities[static_cast<std::size_t>(i)].second;
}

std::uint64_t SceneDoc::max_id() const {
  std::uint64_t m = 0;
  for (const auto& [id, v] : entities) m = std::max(m, id);
  return m;
}

std::vector<std::uint64_t> SceneDoc::order() const {
  std::vector<std::uint64_t> out;
  out.reserve(entities.size());
  for (const auto& [id, v] : entities) out.push_back(id);
  return out;
}

void SceneDoc::apply(const Op& op) {
  switch (op.kind) {
    case Op::Kind::Put: {
      if (op.id == 0) return;
      const int i = index_of(op.id);
      if (i >= 0) entities[static_cast<std::size_t>(i)].second = op.v;
      else entities.emplace_back(op.id, op.v);
      break;
    }
    case Op::Kind::Del: {
      const int i = index_of(op.id);
      if (i >= 0) entities.erase(entities.begin() + i);
      break;
    }
    case Op::Kind::Order: {
      // Listed ids first, in that order; anything not listed keeps its
      // relative order after them (it was created after the reorder).
      std::vector<std::pair<std::uint64_t, Value>> out;
      out.reserve(entities.size());
      std::vector<bool> used(entities.size(), false);
      std::unordered_map<std::uint64_t, std::size_t> at;
      for (std::size_t i = 0; i < entities.size(); ++i) at[entities[i].first] = i;
      for (std::uint64_t id : op.ids) {
        auto it = at.find(id);
        if (it == at.end() || used[it->second]) continue;
        used[it->second] = true;
        out.push_back(std::move(entities[it->second]));
      }
      for (std::size_t i = 0; i < entities.size(); ++i) {
        if (!used[i]) out.push_back(std::move(entities[i]));
      }
      entities = std::move(out);
      break;
    }
    case Op::Kind::Set:
      if (op.v.is_null()) props.erase(op.key);
      else props[op.key] = op.v;
      break;
    case Op::Kind::Asset:
      if (op.sha256.empty()) assets.erase(op.key);
      else assets[op.key] = AssetRef{op.sha256, op.size};
      break;
    case Op::Kind::Terrain:
      if (op.v.is_null()) {
        terrains.erase(op.key);
      } else {
        TerrainState t = terrains.count(op.key) ? terrains[op.key] : TerrainState{};
        if (apply_terrain_patch(t, op.v)) terrains[op.key] = std::move(t);
      }
      break;
  }
}

std::string SceneDoc::canonical() const { return write_compact(doc_to_json(*this, true)); }

std::vector<Op> diff(const SceneDoc& from, const SceneDoc& to) {
  std::vector<Op> ops;
  // Deletions first, then puts (in the target order), then the order.
  for (const auto& [id, v] : from.entities) {
    if (to.index_of(id) < 0) ops.push_back(Op::del(id));
  }
  SceneDoc work = from;
  for (const Op& op : ops) work.apply(op);
  for (const auto& [id, v] : to.entities) {
    const Value* old = from.entity(id);
    if (!old || !same_json(*old, v)) {
      ops.push_back(Op::put(id, v));
      work.apply(ops.back());
    }
  }
  if (work.order() != to.order()) ops.push_back(Op::order(to.order()));
  for (const auto& [k, v] : from.props) {
    if (!to.props.count(k)) ops.push_back(Op::set(k, Value::null()));
  }
  for (const auto& [k, v] : to.props) {
    auto it = from.props.find(k);
    if (it == from.props.end() || !same_json(it->second, v)) ops.push_back(Op::set(k, v));
  }
  for (const auto& [k, a] : from.assets) {
    if (!to.assets.count(k)) ops.push_back(Op::asset(k, "", 0));
  }
  for (const auto& [k, a] : to.assets) {
    auto it = from.assets.find(k);
    if (it == from.assets.end() || !(it->second == a)) ops.push_back(Op::asset(k, a.sha256, a.size));
  }
  // Terrains: whole, keyed to the entity that owns the data file.
  auto owner = [&](const std::string& path) -> std::uint64_t {
    for (const auto& [id, e] : to.entities) {
      if (e["terrain"].str("data") == path) return id;
    }
    return 0;
  };
  for (const auto& [k, t] : from.terrains) {
    if (!to.terrains.count(k)) ops.push_back(Op::terrain(k, 0, Value::null()));
  }
  for (const auto& [k, t] : to.terrains) {
    auto it = from.terrains.find(k);
    if (it == from.terrains.end() || it->second.hash() != t.hash()) {
      ops.push_back(Op::terrain(k, owner(k), terrain_full(t)));
    }
  }
  return ops;
}

Value op_to_json(const Op& op) {
  Value o = Value::object();
  switch (op.kind) {
    case Op::Kind::Put:
      o.set("k", Value::string("put"));
      o.set("id", id_value(op.id));
      o.set("e", op.v);
      break;
    case Op::Kind::Del:
      o.set("k", Value::string("del"));
      o.set("id", id_value(op.id));
      break;
    case Op::Kind::Order:
      o.set("k", Value::string("order"));
      o.set("ids", ids_value(op.ids));
      break;
    case Op::Kind::Set:
      o.set("k", Value::string("set"));
      o.set("key", Value::string(op.key));
      o.set("v", op.v);
      break;
    case Op::Kind::Asset:
      o.set("k", Value::string("asset"));
      o.set("path", Value::string(op.key));
      o.set("sha256", Value::string(op.sha256));
      o.set("size", Value::number(static_cast<double>(op.size)));
      break;
    case Op::Kind::Terrain:
      o.set("k", Value::string("terrain"));
      o.set("path", Value::string(op.key));
      if (op.id) o.set("id", id_value(op.id));
      o.set("v", op.v);
      break;
  }
  return o;
}

bool op_from_json(const Value& v, Op* out) {
  if (!v.is_object()) return false;
  const std::string k = v.str("k");
  if (k == "put") {
    *out = Op::put(id_of(v["id"]), v["e"]);
    return out->id != 0 && out->v.is_object();
  }
  if (k == "del") {
    *out = Op::del(id_of(v["id"]));
    return out->id != 0;
  }
  if (k == "order") {
    *out = Op::order(ids_from(v["ids"]));
    return true;
  }
  if (k == "set") {
    *out = Op::set(v.str("key"), v["v"]);
    return !out->key.empty();
  }
  if (k == "asset") {
    *out = Op::asset(v.str("path"), v.str("sha256"),
                     static_cast<std::uint64_t>(v.num("size", 0.0)));
    return !out->key.empty();
  }
  if (k == "terrain") {
    *out = Op::terrain(v.str("path"), id_of(v["id"]), v["v"]);
    return !out->key.empty();
  }
  return false;
}

Value batch_to_json(const Batch& b) {
  Value o = Value::object();
  if (b.seq) o.set("seq", Value::number(static_cast<double>(b.seq)));
  if (b.from) o.set("from", Value::number(b.from));
  o.set("cid", Value::number(static_cast<double>(b.cid)));
  o.set("scene", Value::string(b.scene));
  o.set("label", Value::string(b.label));
  Value ops = Value::array();
  for (const Op& op : b.ops) ops.push(op_to_json(op));
  o.set("ops", std::move(ops));
  return o;
}

bool batch_from_json(const Value& v, Batch* out) {
  if (!v.is_object()) return false;
  Batch b;
  b.seq = static_cast<std::uint64_t>(v.num("seq", 0.0));
  b.from = static_cast<std::uint32_t>(v.num("from", 0.0));
  b.cid = static_cast<std::uint64_t>(v.num("cid", 0.0));
  b.scene = v.str("scene");
  b.label = v.str("label");
  for (const Value& o : v["ops"].items()) {
    Op op;
    if (!op_from_json(o, &op)) return false;
    b.ops.push_back(std::move(op));
  }
  *out = std::move(b);
  return true;
}

Value doc_to_json(const SceneDoc& d, bool hashes_only) {
  Value o = Value::object();
  Value ents = Value::array();
  for (const auto& [id, v] : d.entities) {
    Value e = Value::object();
    e.set("id", id_value(id));
    e.set("e", v);
    ents.push(std::move(e));
  }
  o.set("entities", std::move(ents));
  Value props = Value::object();
  for (const auto& [k, v] : d.props) props.set(k, v);
  o.set("props", std::move(props));
  Value assets = Value::object();
  for (const auto& [k, a] : d.assets) {
    Value e = Value::object();
    e.set("sha256", Value::string(a.sha256));
    e.set("size", Value::number(static_cast<double>(a.size)));
    assets.set(k, std::move(e));
  }
  o.set("assets", std::move(assets));
  if (!d.terrains.empty()) {
    Value ts = Value::object();
    for (const auto& [k, t] : d.terrains) {
      if (hashes_only) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(t.hash()));
        ts.set(k, Value::string(buf));
      } else {
        ts.set(k, terrain_full(t));
      }
    }
    o.set("terrains", std::move(ts));
  }
  return o;
}

bool doc_from_json(const Value& v, SceneDoc* out) {
  if (!v.is_object()) return false;
  SceneDoc d;
  for (const Value& e : v["entities"].items()) {
    const std::uint64_t id = id_of(e["id"]);
    if (!id || !e["e"].is_object()) return false;
    d.entities.emplace_back(id, e["e"]);
  }
  for (const auto& [k, p] : v["props"].members()) d.props[k] = p;
  for (const auto& [k, a] : v["assets"].members()) {
    d.assets[k] = AssetRef{a.str("sha256"), static_cast<std::uint64_t>(a.num("size", 0.0))};
  }
  for (const auto& [k, p] : v["terrains"].members()) {
    TerrainState t;
    if (!apply_terrain_patch(t, p)) return false;
    d.terrains[k] = std::move(t);
  }
  *out = std::move(d);
  return true;
}

Value docs_to_json(const Docs& docs) {
  Value o = Value::object();
  for (const auto& [k, d] : docs) o.set(k, doc_to_json(d));
  return o;
}

bool docs_from_json(const Value& v, Docs* out) {
  if (!v.is_object()) return false;
  Docs docs;
  for (const auto& [k, d] : v.members()) {
    if (!doc_from_json(d, &docs[k])) return false;
  }
  *out = std::move(docs);
  return true;
}

std::string entity_lock(const std::string& scene, std::uint64_t id) {
  return "e:" + scene + ":" + std::to_string(id);
}
std::string asset_lock(const std::string& path) { return "a:" + path; }

}  // namespace team
}  // namespace tombstone
}  // namespace ts
