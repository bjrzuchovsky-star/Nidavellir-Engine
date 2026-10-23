#include "scene/Prefab.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace ts {
namespace tombstone {
namespace prefab {

namespace fs = std::filesystem;
using json::Value;

namespace {

bool excluded(Dim dim, const std::string& k) {
  if (k == "id" || k == "parent" || k == "prefab" || k == "locked") return true;
  if (dim == Dim::D2) return k == "hidden" || k == "x" || k == "y";
  return k == "editor_hidden" || k == "position";
}

// Instance-local keys copied from the instance on every sync.
bool instance_local(Dim dim, const std::string& k) {
  return k == "locked" || (dim == Dim::D2 ? k == "hidden" : k == "editor_hidden");
}

Value num(std::uint64_t v) { return Value::number(static_cast<double>(v)); }

const Value* asset_entity(const Asset& a, std::uint64_t local) {
  for (const Value& e : a.entities) {
    if (json_id(e) == local) return &e;
  }
  return nullptr;
}

std::uint64_t first_root(const Asset& a) {
  return a.entities.empty() ? 0 : json_id(a.entities.front());
}

struct Group {
  std::uint64_t instance = 0;
  std::vector<std::size_t> idx;  // scene indices
};

// Instances of one asset, in scene order.
std::vector<Group> groups_of(const std::vector<Value>& scene, const std::string& rel) {
  std::vector<Group> out;
  for (std::size_t i = 0; i < scene.size(); ++i) {
    Link l;
    if (!json_link(scene[i], &l) || l.asset != rel) continue;
    auto it = std::find_if(out.begin(), out.end(),
                           [&](const Group& g) { return g.instance == l.instance; });
    if (it == out.end()) {
      out.push_back({l.instance, {}});
      it = out.end() - 1;
    }
    it->idx.push_back(i);
  }
  return out;
}

void origin_of(Dim dim, const std::vector<Value>& scene, const Group& g,
               const Asset& a, double origin[3]) {
  origin[0] = origin[1] = origin[2] = 0.0;
  if (g.idx.empty()) return;
  std::size_t pick = g.idx.front();
  const std::uint64_t root = first_root(a);
  for (std::size_t i : g.idx) {
    Link l;
    json_link(scene[i], &l);
    if (l.source == root) {
      pick = i;
      break;
    }
  }
  Link l;
  json_link(scene[pick], &l);
  double p[3], rel[3] = {0, 0, 0};
  get_pos(dim, scene[pick], p);
  if (const Value* ae = asset_entity(a, l.source)) get_pos(dim, *ae, rel);
  for (int k = 0; k < 3; ++k) origin[k] = p[k] - rel[k];
}

bool near_pos(const double a[3], const double b[3]) {
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(a[k] - b[k]) > 1e-3 * std::max(1.0, std::fabs(b[k]))) return false;
  }
  return true;
}

std::vector<std::string> keys_union(const Value& a, const Value& b) {
  std::vector<std::string> out;
  for (const auto& [k, v] : a.members()) out.push_back(k);
  for (const auto& [k, v] : b.members()) {
    if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
  }
  return out;
}

// The synced entity: asset fields, overrides and instance-local keys from
// the instance, ids / parent mapped.
Value synced(Dim dim, const Value& inst, const Value& asset_e, const Link& link,
             std::uint64_t id, std::uint64_t parent, const double origin[3],
             bool is_origin) {
  Value o = Value::object();
  o.set("id", num(id));
  for (const std::string& k : keys_union(asset_e, inst)) {
    if (k == "id" || k == "prefab" || k == "parent") continue;
    if (dim == Dim::D2 && (k == "x" || k == "y")) {
      if (!o.has("x")) {
        o.set("x", Value::number(0));
        o.set("y", Value::number(0));
      }
      continue;
    }
    if (dim == Dim::D3 && k == "position") {
      o.set("position", Value::number(0));
      continue;
    }
    const bool from_inst = instance_local(dim, k) || link.overridden(k);
    const Value& src = from_inst ? inst : asset_e;
    if (src.has(k)) o.set(k, src[k]);
  }
  if (parent != 0) o.set("parent", num(parent));
  double p[3];
  if (is_origin || link.overridden(kPositionKey)) {
    get_pos(dim, inst, p);
  } else {
    double rel[3];
    get_pos(dim, asset_e, rel);
    for (int k = 0; k < 3; ++k) p[k] = origin[k] + rel[k];
  }
  o = set_pos(dim, o, p);
  o.set("prefab", link_to_json(link));
  return o;
}

void sort_unique(std::vector<std::string>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

}  // namespace

bool Link::overridden(const std::string& key) const {
  return std::find(overrides.begin(), overrides.end(), key) != overrides.end();
}

Value link_to_json(const Link& link) {
  Value o = Value::object();
  o.set("asset", Value::string(link.asset));
  o.set("instance", num(link.instance));
  o.set("source", num(link.source));
  Value arr = Value::array();
  for (const std::string& k : link.overrides) arr.push(Value::string(k));
  o.set("overrides", std::move(arr));
  return o;
}

bool link_from_json(const Value& v, Link* out) {
  if (!v.is_object() || !v["asset"].is_string()) return false;
  Link l;
  l.asset = v.str("asset");
  l.instance = static_cast<std::uint64_t>(std::max(0.0, v.num("instance", 0)) + 0.5);
  l.source = static_cast<std::uint64_t>(std::max(0.0, v.num("source", 0)) + 0.5);
  if (l.asset.empty() || l.source == 0) return false;
  for (const Value& k : v["overrides"].items()) {
    if (k.is_string() && !k.as_string().empty()) l.overrides.push_back(k.as_string());
  }
  sort_unique(l.overrides);
  *out = std::move(l);
  return true;
}

bool parse_asset(const std::string& text, Asset* out, std::string* error) {
  Value root;
  std::string err;
  if (!json::parse(text, &root, &err)) {
    if (error) *error = err;
    return false;
  }
  if (!root.is_object() || root.num("prefab", 0) < 1 || !root["entities"].is_array()) {
    if (error) *error = "not a .prefab.json (needs \"prefab\": 1 and \"entities\")";
    return false;
  }
  if (root.num("prefab", 1) > 1) {
    if (error) *error = "prefab format is newer than this engine";
    return false;
  }
  Asset a;
  a.dim = root.str("dimension", "2d") == "3d" ? Dim::D3 : Dim::D2;
  a.name = root.str("name", "Prefab");
  std::set<std::uint64_t> seen;
  for (const Value& e : root["entities"].items()) {
    const std::uint64_t id = json_id(e);
    if (!e.is_object() || id == 0 || !seen.insert(id).second) {
      if (error) *error = "prefab entity without a unique id";
      return false;
    }
    a.entities.push_back(e);
  }
  if (a.entities.empty()) {
    if (error) *error = "prefab has no entities";
    return false;
  }
  *out = std::move(a);
  return true;
}

std::string write_asset(const Asset& asset) {
  Value root = Value::object();
  root.set("prefab", Value::number(1));
  root.set("dimension", Value::string(asset.dim == Dim::D3 ? "3d" : "2d"));
  root.set("name", Value::string(asset.name));
  Value arr = Value::array();
  for (const Value& e : asset.entities) arr.push(e);
  root.set("entities", std::move(arr));
  return json::write(root);
}

bool load_asset(const std::string& path, Asset* out, std::string* error) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_asset(ss.str(), out, error);
}

bool save_asset(const std::string& path, const Asset& asset, std::string* error) {
  std::error_code ec;
  const fs::path p = fs::u8path(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "cannot write " + path;
      return false;
    }
    out << write_asset(asset);
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    if (error) *error = "cannot replace " + path + ": " + ec.message();
    return false;
  }
  return true;
}

std::string default_asset_rel(const std::string& name) {
  std::string slug;
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      slug += static_cast<char>(std::tolower(u));
    } else if (!slug.empty() && slug.back() != '_') {
      slug += '_';
    }
  }
  while (!slug.empty() && slug.back() == '_') slug.pop_back();
  if (slug.empty()) slug = "prefab";
  return std::string(kFolder) + "/" + slug + kExtension;
}

std::string name_from_rel(const std::string& rel) {
  std::string n = rel.substr(rel.find_last_of("/\\") == std::string::npos
                                 ? 0
                                 : rel.find_last_of("/\\") + 1);
  const std::string ext = kExtension;
  if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) {
    n.resize(n.size() - ext.size());
  }
  return n;
}

std::vector<std::string> scan_assets(const std::string& project_dir) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path root = fs::u8path(project_dir);
  if (!fs::is_directory(root, ec)) return out;
  const std::string ext = kExtension;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       it != end; it.increment(ec)) {
    if (ec) break;
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec) && (name == "build" || name == ".git" || name == "export" || name == ".tombstone" || name == ".vault")) {
      it.disable_recursion_pending();
      continue;
    }
    if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
      out.push_back(fs::relative(it->path(), root, ec).generic_string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::uint64_t json_id(const Value& e) {
  const double d = e.num("id", 0);
  return d > 0 ? static_cast<std::uint64_t>(d + 0.5) : 0;
}

std::uint64_t json_parent(const Value& e) {
  const double d = e.num("parent", 0);
  return d > 0 ? static_cast<std::uint64_t>(d + 0.5) : 0;
}

bool json_link(const Value& e, Link* out) {
  return e.is_object() && e.has("prefab") && link_from_json(e["prefab"], out);
}

Value with_key(const Value& o, const std::string& key, Value v) {
  Value r = o;
  r.set(key, std::move(v));
  return r;
}

Value without_key(const Value& o, const std::string& key) {
  Value r = Value::object();
  for (const auto& [k, v] : o.members()) {
    if (k != key) r.set(k, v);
  }
  return r;
}

void get_pos(Dim dim, const Value& e, double out[3]) {
  out[0] = out[1] = out[2] = 0.0;
  if (dim == Dim::D2) {
    out[0] = e.num("x", 0);
    out[1] = e.num("y", 0);
  } else {
    const Value& p = e["position"];
    for (std::size_t k = 0; k < 3 && k < p.size(); ++k) out[k] = p[k].as_number(0);
  }
}

Value set_pos(Dim dim, const Value& e, const double p[3]) {
  Value r = e;
  if (dim == Dim::D2) {
    r.set("x", Value::number(static_cast<float>(p[0])));
    r.set("y", Value::number(static_cast<float>(p[1])));
  } else {
    const float f[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]),
                        static_cast<float>(p[2])};
    r.set("position", json::floats(f, 3));
  }
  return r;
}

bool json_equal(const Value& a, const Value& b) {
  if (a.type() != b.type()) return false;
  switch (a.type()) {
    case Value::Type::Null: return true;
    case Value::Type::Bool: return a.as_bool() == b.as_bool();
    case Value::Type::Number:
      return static_cast<float>(a.as_number()) == static_cast<float>(b.as_number());
    case Value::Type::String: return a.as_string() == b.as_string();
    case Value::Type::Array:
      if (a.size() != b.size()) return false;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (!json_equal(a[i], b[i])) return false;
      }
      return true;
    case Value::Type::Object:
      if (a.members().size() != b.members().size()) return false;
      for (const auto& [k, v] : a.members()) {
        if (!b.has(k) || !json_equal(v, b[k])) return false;
      }
      return true;
  }
  return false;
}

Asset make_asset(Dim dim, const std::string& name, const std::vector<Value>& entities,
                 std::map<std::uint64_t, std::uint64_t>* local_ids) {
  Asset a;
  a.dim = dim;
  a.name = name.empty() ? "Prefab" : name;
  std::set<std::uint64_t> in_set;
  for (const Value& e : entities) in_set.insert(json_id(e));
  // First root first (it becomes local id 1 and the anchor).
  std::vector<const Value*> order;
  for (const Value& e : entities) {
    if (!in_set.count(json_parent(e))) {
      order.push_back(&e);
      break;
    }
  }
  for (const Value& e : entities) {
    if (order.empty() || &e != order.front()) order.push_back(&e);
  }
  std::map<std::uint64_t, std::uint64_t> local;
  std::uint64_t next = 1;
  for (const Value* e : order) local[json_id(*e)] = next++;
  if (local_ids) *local_ids = local;
  double anchor[3] = {0, 0, 0};
  if (!order.empty()) get_pos(dim, *order.front(), anchor);
  for (const Value* e : order) {
    Value o = Value::object();
    o.set("id", num(local[json_id(*e)]));
    for (const auto& [k, v] : e->members()) {
      if (k == "id" || k == "parent" || k == "prefab" || instance_local(dim, k)) continue;
      o.set(k, v);
    }
    const auto pit = local.find(json_parent(*e));
    if (pit != local.end()) o.set("parent", num(pit->second));
    double p[3];
    get_pos(dim, *e, p);
    for (int k = 0; k < 3; ++k) p[k] -= anchor[k];
    a.entities.push_back(set_pos(dim, o, p));
  }
  return a;
}

std::vector<Value> instantiate(const Asset& asset, const std::string& asset_rel,
                               const double pos[3], std::uint64_t instance_id,
                               std::uint64_t* next_id, std::uint64_t parent) {
  std::map<std::uint64_t, std::uint64_t> ids;
  for (const Value& e : asset.entities) ids[json_id(e)] = (*next_id)++;
  std::vector<Value> out;
  for (const Value& e : asset.entities) {
    Link l;
    l.asset = asset_rel;
    l.instance = instance_id;
    l.source = json_id(e);
    const auto pit = ids.find(json_parent(e));
    const std::uint64_t par = pit != ids.end() ? pit->second : parent;
    double origin[3] = {pos[0], pos[1], pos[2]};
    out.push_back(synced(asset.dim, e, e, l, ids[l.source], par, origin, false));
  }
  return out;
}

std::uint64_t max_instance(const std::vector<Value>& scene) {
  std::uint64_t m = 0;
  for (const Value& e : scene) {
    Link l;
    if (json_link(e, &l)) m = std::max(m, l.instance);
  }
  return m;
}

bool refresh_overrides(Dim dim, std::vector<Value>& scene, const AssetLookup& lookup) {
  bool changed = false;
  std::uint64_t next_instance = max_instance(scene) + 1;
  std::set<std::string> rels;
  for (const Value& e : scene) {
    Link l;
    if (json_link(e, &l)) rels.insert(l.asset);
  }
  for (const std::string& rel : rels) {
    const Asset* asset = lookup ? lookup(rel) : nullptr;
    // Split duplicated entities into their own instances first.
    for (Group& g : groups_of(scene, rel)) {
      std::vector<std::set<std::uint64_t>> buckets;
      std::vector<std::uint64_t> bucket_instance;
      for (std::size_t i : g.idx) {
        Link l;
        json_link(scene[i], &l);
        std::size_t b = 0;
        while (b < buckets.size() && buckets[b].count(l.source)) ++b;
        if (b == buckets.size()) {
          buckets.emplace_back();
          bucket_instance.push_back(b == 0 ? g.instance : next_instance++);
        }
        buckets[b].insert(l.source);
        if (bucket_instance[b] != l.instance) {
          l.instance = bucket_instance[b];
          scene[i] = with_key(scene[i], "prefab", link_to_json(l));
          changed = true;
        }
      }
    }
    if (!asset) continue;
    const std::uint64_t root = first_root(*asset);
    for (const Group& g : groups_of(scene, rel)) {
      double origin[3];
      origin_of(dim, scene, g, *asset, origin);
      for (std::size_t i : g.idx) {
        Link l;
        json_link(scene[i], &l);
        const Value* ae = asset_entity(*asset, l.source);
        if (!ae) continue;  // removed from the asset; sync drops it
        std::vector<std::string> ov;
        for (const std::string& k : keys_union(*ae, scene[i])) {
          if (excluded(dim, k)) continue;
          const bool a_has = ae->has(k), i_has = scene[i].has(k);
          if (a_has != i_has || (a_has && !json_equal((*ae)[k], scene[i][k]))) {
            ov.push_back(k);
          }
        }
        if (l.source != root) {
          double p[3], rel_p[3], want[3];
          get_pos(dim, scene[i], p);
          get_pos(dim, *ae, rel_p);
          for (int k = 0; k < 3; ++k) want[k] = origin[k] + rel_p[k];
          if (!near_pos(p, want)) ov.push_back(kPositionKey);
        }
        sort_unique(ov);
        if (ov != l.overrides) {
          l.overrides = std::move(ov);
          scene[i] = with_key(scene[i], "prefab", link_to_json(l));
          changed = true;
        }
      }
    }
  }
  return changed;
}

std::size_t sync_instances(Dim dim, std::vector<Value>& scene, const std::string& asset_rel,
                           const Asset& asset, std::uint64_t* next_id) {
  std::size_t changed = 0;
  const std::uint64_t root = first_root(asset);
  for (const Group& g : groups_of(scene, asset_rel)) {
    double origin[3];
    origin_of(dim, scene, g, asset, origin);
    std::map<std::uint64_t, std::size_t> by_source;
    for (std::size_t i : g.idx) {
      Link l;
      json_link(scene[i], &l);
      by_source.emplace(l.source, i);
    }
    // Entity ids for every asset entity (new ones for additions).
    std::map<std::uint64_t, std::uint64_t> ids;
    for (const Value& ae : asset.entities) {
      const auto it = by_source.find(json_id(ae));
      ids[json_id(ae)] = it != by_source.end() ? json_id(scene[it->second]) : (*next_id)++;
    }
    std::uint64_t root_parent = 0;
    if (const auto it = by_source.find(root); it != by_source.end()) {
      root_parent = json_parent(scene[it->second]);
    }
    std::vector<Value> added;
    for (const Value& ae : asset.entities) {
      const std::uint64_t src = json_id(ae);
      const auto it = by_source.find(src);
      const Value& inst = it != by_source.end() ? scene[it->second] : ae;
      Link l;
      if (it == by_source.end() || !json_link(inst, &l)) {
        l = Link{};
        l.asset = asset_rel;
        l.instance = g.instance;
        l.source = src;
      }
      const auto pit = ids.find(json_parent(ae));
      std::uint64_t par = pit != ids.end() ? pit->second
                          : (it != by_source.end() ? json_parent(inst) : root_parent);
      Value out = synced(dim, inst, ae, l, ids[src], par, origin,
                         src == root && it != by_source.end());
      if (it == by_source.end()) {
        added.push_back(std::move(out));
        ++changed;
      } else if (!json_equal(out, scene[it->second])) {
        scene[it->second] = std::move(out);
        ++changed;
      }
    }
    // Entities no longer in the asset go away.
    std::set<std::uint64_t> drop;
    for (const auto& [src, i] : by_source) {
      if (!asset_entity(asset, src)) drop.insert(json_id(scene[i]));
    }
    if (!drop.empty()) {
      changed += drop.size();
      scene.erase(std::remove_if(scene.begin(), scene.end(),
                                 [&](const Value& e) { return drop.count(json_id(e)) > 0; }),
                  scene.end());
    }
    for (Value& v : added) scene.push_back(std::move(v));
  }
  return changed;
}

namespace {

// Scene indices the operation covers.
std::vector<std::size_t> targets(const std::vector<Value>& scene, std::uint64_t entity_id,
                                 bool whole_instance, Link* link_out) {
  std::vector<std::size_t> out;
  Link link;
  bool found = false;
  for (std::size_t i = 0; i < scene.size(); ++i) {
    if (json_id(scene[i]) == entity_id && json_link(scene[i], &link)) {
      found = true;
      if (!whole_instance) out.push_back(i);
      break;
    }
  }
  if (!found) return {};
  if (whole_instance) {
    for (std::size_t i = 0; i < scene.size(); ++i) {
      Link l;
      if (json_link(scene[i], &l) && l.asset == link.asset && l.instance == link.instance) {
        out.push_back(i);
      }
    }
  }
  if (link_out) *link_out = link;
  return out;
}

}  // namespace

bool apply_overrides(Dim dim, std::vector<Value>& scene, std::uint64_t entity_id,
                     const std::string& key, bool whole_instance, Asset* asset) {
  Link link;
  const std::vector<std::size_t> idx = targets(scene, entity_id, whole_instance, &link);
  if (idx.empty() || !asset) return false;
  Group g;
  g.instance = link.instance;
  for (std::size_t i : targets(scene, entity_id, true, nullptr)) g.idx.push_back(i);
  double origin[3];
  origin_of(dim, scene, g, *asset, origin);
  bool any = false;
  for (std::size_t i : idx) {
    Link l;
    json_link(scene[i], &l);
    auto ait = std::find_if(asset->entities.begin(), asset->entities.end(),
                            [&](const Value& e) { return json_id(e) == l.source; });
    if (ait == asset->entities.end()) continue;
    std::vector<std::string> keys;
    if (key.empty()) {
      keys = l.overrides;
    } else if (l.overridden(key)) {
      keys.push_back(key);
    }
    for (const std::string& k : keys) {
      if (k == kPositionKey) {
        double p[3];
        get_pos(dim, scene[i], p);
        for (int c = 0; c < 3; ++c) p[c] -= origin[c];
        *ait = set_pos(dim, *ait, p);
      } else if (scene[i].has(k)) {
        *ait = with_key(*ait, k, scene[i][k]);
      } else {
        *ait = without_key(*ait, k);
      }
      l.overrides.erase(std::remove(l.overrides.begin(), l.overrides.end(), k),
                        l.overrides.end());
      any = true;
    }
    scene[i] = with_key(scene[i], "prefab", link_to_json(l));
  }
  return any;
}

bool revert_overrides(Dim dim, std::vector<Value>& scene, std::uint64_t entity_id,
                      const std::string& key, bool whole_instance, const Asset& asset,
                      std::uint64_t* next_id) {
  Link link;
  const std::vector<std::size_t> idx = targets(scene, entity_id, whole_instance, &link);
  bool any = false;
  for (std::size_t i : idx) {
    Link l;
    json_link(scene[i], &l);
    const std::size_t before = l.overrides.size();
    if (key.empty()) {
      l.overrides.clear();
    } else {
      l.overrides.erase(std::remove(l.overrides.begin(), l.overrides.end(), key),
                        l.overrides.end());
    }
    if (l.overrides.size() != before) {
      scene[i] = with_key(scene[i], "prefab", link_to_json(l));
      any = true;
    }
  }
  if (any) sync_instances(dim, scene, link.asset, asset, next_id);
  return any;
}

std::size_t unpack_instance(std::vector<Value>& scene, std::uint64_t entity_id) {
  std::size_t n = 0;
  for (std::size_t i : targets(scene, entity_id, true, nullptr)) {
    scene[i] = without_key(scene[i], "prefab");
    ++n;
  }
  return n;
}

}  // namespace prefab
}  // namespace tombstone
}  // namespace ts
