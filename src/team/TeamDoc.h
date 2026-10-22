#pragma once

// Live Team Claims (docs/team-claims.md): the shared document and its ops.
//
// A claim's scene as the team layer sees it: an ordered list of entities,
// each held as its scene.json object, plus named scene-level blocks
// ("tile_solidity", "lighting", "environment", "physics") and the project
// files announced through the Vault (path -> SHA-256). The editors convert
// their own scene types to and from this (src/editor/team), so the team
// layer never needs to know a component.
//
// An op batch is one undo step's worth of change. Applying the same
// batches in the same order to the same doc always gives the same doc.

#include "scene/JsonTree.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace team {

using json::Value;

inline constexpr int kProtocolVersion = 1;

struct Op {
  enum class Kind : std::uint8_t { Put, Del, Order, Set, Asset };
  Kind kind = Kind::Put;
  std::uint64_t id = 0;              // Put / Del
  Value v;                           // Put: entity object; Set: the block
  std::vector<std::uint64_t> ids;    // Order
  std::string key;                   // Set: block name; Asset: project path
  std::string sha256;                // Asset
  std::uint64_t size = 0;            // Asset

  static Op put(std::uint64_t id, Value e);
  static Op del(std::uint64_t id);
  static Op order(std::vector<std::uint64_t> ids);
  static Op set(std::string key, Value v);
  static Op asset(std::string path, std::string sha256, std::uint64_t size);

  // What this op touches, as a short key ("e:7", "p:lighting", "order",
  // "a:assets/crate.png"). Used for pending-vs-remote filtering.
  std::string touch_key() const;
};

struct Batch {
  std::uint64_t seq = 0;     // server order (0 = not committed yet)
  std::uint32_t from = 0;    // member id
  std::uint64_t cid = 0;     // the sender's own batch id
  std::string scene;         // e.g. "scene.json"
  std::string label;         // the undo step's label
  std::vector<Op> ops;
};

struct AssetRef {
  std::string sha256;
  std::uint64_t size = 0;
  bool operator==(const AssetRef& o) const = default;
};

struct SceneDoc {
  std::vector<std::pair<std::uint64_t, Value>> entities;  // in scene order
  std::map<std::string, Value> props;
  std::map<std::string, AssetRef> assets;

  int index_of(std::uint64_t id) const;  // -1 when absent
  const Value* entity(std::uint64_t id) const;
  void apply(const Op& op);
  void apply(const Batch& b) {
    for (const Op& op : b.ops) apply(op);
  }
  std::uint64_t max_id() const;
  // Deterministic text of the whole doc (equality, hashes, smokes).
  std::string canonical() const;
  bool same_as(const SceneDoc& o) const { return canonical() == o.canonical(); }
  std::vector<std::uint64_t> order() const;
};

using Docs = std::map<std::string, SceneDoc>;

// Ops that turn `from` into `to` (puts for new / changed entities, dels,
// an order op when the order differs, sets for changed blocks, assets).
std::vector<Op> diff(const SceneDoc& from, const SceneDoc& to);

// Compact JSON text (one line, no spaces); numbers as the scene files
// write them.
std::string write_compact(const Value& v);
bool same_json(const Value& a, const Value& b);

Value op_to_json(const Op& op);
bool op_from_json(const Value& v, Op* out);
Value batch_to_json(const Batch& b);
bool batch_from_json(const Value& v, Batch* out);
Value doc_to_json(const SceneDoc& d);
bool doc_from_json(const Value& v, SceneDoc* out);
Value docs_to_json(const Docs& d);
bool docs_from_json(const Value& v, Docs* out);

// Lock keys: an entity in a scene, or a project asset path.
std::string entity_lock(const std::string& scene, std::uint64_t id);
std::string asset_lock(const std::string& path);

// Ids as JSON numbers (exact below 2^53).
inline Value id_value(std::uint64_t id) { return Value::number(static_cast<double>(id)); }
inline std::uint64_t id_of(const Value& v) {
  const double d = v.as_number(0.0);
  return d > 0.0 ? static_cast<std::uint64_t>(d) : 0;
}

}  // namespace team
}  // namespace tombstone
}  // namespace ts
