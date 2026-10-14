#include "plugins/shipyard/Sample.h"

#include "plugins/shipyard/ShipSim.h"
#include "scene/SceneJson.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace shipyard {

namespace {

namespace fs = std::filesystem;

constexpr const char* kSky = R"({
  "player_design": "shipyard/designs/sloop.ship.json",
  "crew": true,
  "respawn_seconds": 4,
  "reinforce_seconds": 25,
  "enemies": [
    {"design": "shipyard/designs/marauder.ship.json", "x": 560, "y": -380},
    {"design": "shipyard/designs/marauder.ship.json", "x": -620, "y": -300}
  ]
}
)";

constexpr const char* kBountySource = R"lua(-- Bounty board: pays out for every pirate ship knocked out of the sky
-- (Airship Works' `ships` table).
props = {
  bounty = 50,
}

local pirates = 0
local purse = 0

function on_start()
  pirates = #ships.list("pirates")
  toast("Sky pirates sighted: " .. pirates .. ". $" .. props.bounty .. " a hull.", 4)
end

function on_tick(dt)
  local now = #ships.list("pirates")
  if now < pirates then
    purse = purse + (pirates - now) * props.bounty
    local lost, splits = ships.stats()
    toast("Bounty paid! Purse $" .. purse .. " (" .. lost .. " parts shot off, " .. splits .. " wrecks)", 3)
    log("bounty purse now " .. purse)
  end
  pirates = now
end
)lua";

bool write_if_missing(const fs::path& p, const std::string& text, std::string* error) {
  std::error_code ec;
  if (fs::exists(p, ec)) return true;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out) {
    if (error) *error = "could not write " + p.string();
    return false;
  }
  return true;
}

std::string now_stamp() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

bool write_sky_pirates_sample(const std::string& dir, const std::string& name, std::string* error) {
  const fs::path root(dir);
  if (!write_if_missing(root / "project.json",
                        "{\"name\": \"" + escape(name) + "\", \"dimension\": \"2D\", \"created\": \"" +
                            now_stamp() + "\", \"plugins\": \"ts.shipyard\"}\n",
                        error)) {
    return false;
  }
  const fs::path yard(shipyard_dir(dir));
  if (!write_if_missing(yard / "parts.json", default_parts_json(), error)) return false;
  if (!write_if_missing(yard / "sky.json", kSky, error)) return false;
  if (!write_if_missing(root / kSloopPath, write_design(ShipSim::sloop_design()), error)) return false;
  if (!write_if_missing(root / kMarauderPath, write_design(ShipSim::marauder_design()), error)) return false;
  if (!write_if_missing(root / kBountyScript, kBountySource, error)) return false;

  const fs::path scene = scene_json::scene_path_for_project(dir);
  std::error_code ec;
  if (fs::exists(scene, ec)) return true;
  scene_json::SceneDoc doc;
  Entity2D cam;
  cam.id = 1;
  cam.name = "Camera";
  cam.w = cam.h = 16.0f;
  cam.color[3] = 0.0f;
  // The captain: hidden while flying, pinned to the ship's centre of mass
  // (speed 0 so a posse client's prediction leaves it to the server).
  Entity2D captain;
  captain.id = 2;
  captain.name = "Captain";
  captain.x = -12.0f;
  captain.y = -12.0f;
  captain.w = 24.0f;
  captain.h = 24.0f;
  captain.color[0] = 0.95f;
  captain.color[1] = 0.72f;
  captain.color[2] = 0.32f;
  captain.color[3] = 1.0f;
  captain.layer = 5;
  captain.player = PlayerControllerData{};
  captain.player->speed = 0.0f;
  Camera2DData follow;
  follow.target = captain.id;
  follow.zoom = 1.0f;
  cam.camera = follow;
  Entity2D board;
  board.id = 3;
  board.name = "BountyBoard";
  board.x = -2000.0f;  // out of the way; it only runs the script
  board.y = -2000.0f;
  board.w = 8.0f;
  board.h = 8.0f;
  board.color[3] = 0.0f;
  ScriptData sd;
  sd.path = kBountyScript;
  board.script = sd;
  doc.entities = {cam, captain, board};
  doc.zoom = 1.0f;
  return scene_json::save_file(scene.string(), doc, error);
}

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
