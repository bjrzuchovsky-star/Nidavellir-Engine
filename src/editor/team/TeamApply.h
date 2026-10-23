#pragma once

// Shared by the 2D and 3D team bindings: entity-level ops on a vector of
// the editor's own entity type, and the live-vs-shadow diff.

#include "team/TeamDoc.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace team_apply {

// Apply one entity op (Put / Del / Order) to `ents`. from_json(v, &e)
// decodes an entity; returns false (ents unchanged) on a bad payload.
template <typename Entity, typename FromJson>
bool apply_entity_op(std::vector<Entity>& ents, const team::Op& op, FromJson from_json) {
  using K = team::Op::Kind;
  auto find = [&](std::uint64_t id) {
    return std::find_if(ents.begin(), ents.end(), [&](const Entity& e) { return e.id == id; });
  };
  switch (op.kind) {
    case K::Put: {
      Entity e;
      if (!from_json(op.v, &e)) return false;
      e.id = op.id;
      auto it = find(op.id);
      if (it != ents.end()) *it = std::move(e);
      else ents.push_back(std::move(e));
      return true;
    }
    case K::Del: {
      auto it = find(op.id);
      if (it != ents.end()) ents.erase(it);
      return true;
    }
    case K::Order: {
      std::unordered_map<std::uint64_t, std::size_t> at;
      for (std::size_t i = 0; i < ents.size(); ++i) at[ents[i].id] = i;
      std::vector<bool> used(ents.size(), false);
      std::vector<Entity> out;
      out.reserve(ents.size());
      for (std::uint64_t id : op.ids) {
        auto it = at.find(id);
        if (it == at.end() || used[it->second]) continue;
        used[it->second] = true;
        out.push_back(std::move(ents[it->second]));
      }
      for (std::size_t i = 0; i < ents.size(); ++i) {
        if (!used[i]) out.push_back(std::move(ents[i]));
      }
      ents = std::move(out);
      return true;
    }
    default: return false;
  }
}

// Ops turning `shadow` into `live` (entities only).
template <typename Entity, typename ToJson>
std::vector<team::Op> diff_entities(const std::vector<Entity>& shadow,
                                    const std::vector<Entity>& live, ToJson to_json) {
  std::vector<team::Op> ops;
  std::unordered_map<std::uint64_t, const Entity*> old;
  for (const Entity& e : shadow) old[e.id] = &e;
  std::unordered_map<std::uint64_t, bool> now;
  for (const Entity& e : live) now[e.id] = true;
  std::vector<std::uint64_t> kept;  // shadow order without the deleted ones
  for (const Entity& e : shadow) {
    if (!now.count(e.id)) ops.push_back(team::Op::del(e.id));
    else kept.push_back(e.id);
  }
  std::vector<std::uint64_t> after = kept;  // order once the puts land
  for (const Entity& e : live) {
    auto it = old.find(e.id);
    if (it == old.end()) {
      ops.push_back(team::Op::put(e.id, to_json(e)));
      after.push_back(e.id);
    } else if (!(*it->second == e)) {
      ops.push_back(team::Op::put(e.id, to_json(e)));
    }
  }
  std::vector<std::uint64_t> order;
  order.reserve(live.size());
  for (const Entity& e : live) order.push_back(e.id);
  if (after != order) ops.push_back(team::Op::order(std::move(order)));
  return ops;
}

// Mid-drag preview: puts for entities that exist in `shadow` and differ
// in `live`, skipping `skip` (entities under someone else's preview).
template <typename Entity, typename ToJson, typename Skip>
std::vector<team::Op> preview_puts(const std::vector<Entity>& shadow, const std::vector<Entity>& live,
                                   ToJson to_json, const Skip& skip) {
  std::vector<team::Op> ops;
  std::unordered_map<std::uint64_t, const Entity*> old;
  for (const Entity& e : shadow) old[e.id] = &e;
  for (const Entity& e : live) {
    if (skip.count(e.id)) continue;
    auto it = old.find(e.id);
    if (it != old.end() && !(*it->second == e)) ops.push_back(team::Op::put(e.id, to_json(e)));
  }
  return ops;
}

// Show another member's transient puts in `live` (existing entities only).
template <typename Entity, typename FromJson, typename Set>
void apply_preview(std::vector<Entity>& live, const std::vector<team::Op>& puts, FromJson from_json,
                   Set& previewed) {
  for (const team::Op& op : puts) {
    if (op.kind != team::Op::Kind::Put) continue;
    auto it = std::find_if(live.begin(), live.end(), [&](const Entity& e) { return e.id == op.id; });
    if (it == live.end()) continue;
    Entity e;
    if (!from_json(op.v, &e)) continue;
    e.id = op.id;
    *it = std::move(e);
    previewed.insert(op.id);
  }
}

// Put previewed entities back to the shadow.
template <typename Entity, typename Set>
void end_preview(std::vector<Entity>& live, const std::vector<Entity>& shadow,
                 const std::vector<std::uint64_t>& ids, Set& previewed) {
  for (std::uint64_t id : ids) {
    if (!previewed.erase(id)) continue;
    auto it = std::find_if(live.begin(), live.end(), [&](const Entity& e) { return e.id == id; });
    auto sh = std::find_if(shadow.begin(), shadow.end(), [&](const Entity& e) { return e.id == id; });
    if (it != live.end() && sh != shadow.end()) *it = *sh;
  }
}

// Leave entities under a remote preview out of our outgoing ops.
template <typename Set>
void drop_previewed(std::vector<team::Op>& ops, const Set& previewed) {
  if (previewed.empty()) return;
  ops.erase(std::remove_if(ops.begin(), ops.end(),
                           [&](const team::Op& op) {
                             return op.kind == team::Op::Kind::Put && previewed.count(op.id);
                           }),
            ops.end());
}

}  // namespace team_apply
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
