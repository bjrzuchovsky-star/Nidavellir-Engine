// Strongbox bridge: a Town Hall character <-> a Strongbox save slot of the
// same project. The slot uses the sections the story runtime reads and
// writes ("vars", "quests", "player" with "position") plus an "mmo" section
// (rider, zone, inventory), so a single-player build of the game can load a
// character's story, and a designer can seed a character from a save.

#include "mmo/MmoServer.h"
#include "mmo/MmoServerState.h"

#include "scene/JsonTree.h"
#include "story/SaveGame.h"

namespace ts {
namespace tombstone {
namespace mmo {

bool MmoServer::export_to_strongbox(const std::string& name, const std::string& slot, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (!story::valid_slot_name(slot)) return fail("slot names are letters, digits, '-' and '_'");
  CharacterRow row;
  if (Character* c = find_character(name)) {
    row = row_of(*c);
  } else if (auto r = store_.character_by_name(name)) {
    row = *r;
  } else {
    return fail("no rider " + name);
  }
  json::Value st;
  std::string err;
  if (!json::parse(row.story, &st, &err) || !st.is_object()) st = json::Value::object();
  story::SaveStore ss(config_.project_dir);
  ss.scene = row.zone;
  ss.playtime = row.playtime;
  ss.provide({"vars", [&] { return st["vars"].is_object() ? st["vars"] : json::Value::object(); }, nullptr});
  ss.provide({"quests", [&] { return st["quests"].is_null() ? json::Value::object() : st["quests"]; }, nullptr});
  ss.provide({"player",
              [&] {
                json::Value p = json::Value::object();
                const float pos[3] = {row.x, row.y, 0.0f};
                p.set("position", json::floats(pos, 3));
                p.set("zone", json::Value::string(row.zone));
                return p;
              },
              nullptr});
  ss.provide({"mmo",
              [&] {
                json::Value m = json::Value::object();
                m.set("character", json::Value::string(row.name));
                m.set("zone", json::Value::string(row.zone));
                json::Value inv = json::Value::object();
                for (const auto& [k, n] : row.inventory) inv.set(k, json::Value::number(n));
                m.set("inventory", inv);
                return m;
              },
              nullptr});
  if (!ss.save(slot, "Town Hall: " + row.name, &err)) return fail(err);
  log("exported " + row.name + " to Strongbox slot " + slot);
  return true;
}

bool MmoServer::import_from_strongbox(const std::string& name, const std::string& slot, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  Character* live = find_character(name);
  std::optional<CharacterRow> stored;
  if (!live) {
    stored = store_.character_by_name(name);
    if (!stored) return fail("no rider " + name);
  }
  json::Value vars, quests, player, mmo;
  story::SaveStore ss(config_.project_dir);
  ss.provide({"vars", nullptr, [&](const json::Value& v) { vars = v; }});
  ss.provide({"quests", nullptr, [&](const json::Value& v) { quests = v; }});
  ss.provide({"player", nullptr, [&](const json::Value& v) { player = v; }});
  ss.provide({"mmo", nullptr, [&](const json::Value& v) { mmo = v; }});
  std::string err;
  if (!ss.load(slot, &err)) return fail(err.empty() ? "no slot " + slot : err);
  float pos[3] = {0, 0, 0};
  const bool has_pos = player.is_object() && player.floats("position", pos, 3);
  std::string zone = player.is_object() ? player.str("zone") : std::string();
  if (zone.empty() && mmo.is_object()) zone = mmo.str("zone");
  if (!zone.empty() && !cfg_.zone(zone)) zone.clear();
  std::map<std::string, int> inv;
  const bool has_inv = mmo.is_object() && mmo["inventory"].is_object();
  if (has_inv)
    for (const auto& [k, v] : mmo["inventory"].members())
      if (v.as_number() > 0) inv[k] = static_cast<int>(v.as_number());
  if (live) {
    if (vars.is_object()) live->vars.from_json(vars);
    if (!quests.is_null()) live->quests.from_json(quests);
    if (has_inv) {
      live->inventory = inv;
      send_inventory(*live);
    }
    if (has_pos) change_zone(*live, zone.empty() ? live->zone : zone, pos[0], pos[1]);
    live->dirty = true;
  } else {
    CharacterRow& r = *stored;
    json::Value st;
    if (!json::parse(r.story, &st, &err) || !st.is_object()) st = json::Value::object();
    if (vars.is_object()) st.set("vars", vars);
    if (!quests.is_null()) st.set("quests", quests);
    r.story = json::write(st);
    if (has_inv) r.inventory = inv;
    if (has_pos) {
      r.x = pos[0];
      r.y = pos[1];
    }
    if (!zone.empty()) r.zone = zone;
    if (!store_.save_characters({r})) return fail(store_.last_error());
  }
  log("imported Strongbox slot " + slot + " into " + name);
  return true;
}

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
