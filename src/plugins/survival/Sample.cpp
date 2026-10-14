#include "plugins/survival/Sample.h"

#include "plugins/survival/Data.h"
#include "scene/SampleRider.h"
#include "scene/SceneJson.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace survival {

namespace {

namespace fs = std::filesystem;

constexpr const char* kCrateSource = R"lua(-- Supply crate: the first rider to bump into it gets a starter kit
-- (Frontier Survival's `frontier` table), then the crate is gone.
props = {
  wood = 12,
  stone = 6,
}

function on_trigger_enter(other)
  if not other:is_player() then
    return
  end
  local slot = other:slot()
  frontier.give(slot, "wood", props.wood)
  frontier.give(slot, "stone", props.stone)
  frontier.give(slot, "jerky", 2)
  other:toast("Supply crate: " .. props.wood .. " logs, " .. props.stone .. " stone, jerky. Ride safe.", 3)
  log("P" .. slot .. " opened the supply crate (" .. frontier.count(slot, "wood") .. " logs now)")
  self:destroy()
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

bool write_frontier_sample(const std::string& dir, const std::string& name, std::string* error) {
  const fs::path root(dir);
  if (!write_if_missing(root / "project.json",
                        "{\"name\": \"" + escape(name) + "\", \"dimension\": \"2D\", \"created\": \"" +
                            now_stamp() + "\", \"plugins\": \"ts.frontier\"}\n",
                        error)) {
    return false;
  }
  if (!sample_rider::write_into_project(dir, error)) return false;
  if (!write_if_missing(root / kCrateScript, kCrateSource, error)) return false;
  if (!write_default_data(dir, false, error)) return false;

  const fs::path scene = scene_json::scene_path_for_project(dir);
  std::error_code ec;
  if (fs::exists(scene, ec)) return true;
  scene_json::SceneDoc doc;
  Entity2D cam;
  cam.id = 1;
  cam.name = "Camera";
  cam.w = cam.h = 16.0f;
  cam.color[3] = 0.0f;
  Entity2D rider;
  rider.id = 2;
  rider.name = "Rider";
  rider.x = 16.0f - 16.0f;
  rider.y = 16.0f - 40.0f;
  rider.w = 32.0f;
  rider.h = 48.0f;
  for (float& c : rider.color) c = 1.0f;
  rider.layer = 5;
  rider.player = PlayerControllerData{};
  rider.player->speed = 150.0f;
  ColliderData col;
  col.offset_x = 6.0f;
  col.offset_y = 30.0f;
  col.w = 20.0f;
  col.h = 16.0f;
  col.dynamic = true;
  rider.collider = col;
  rider.sprite = SpriteData{};
  rider.sprite->path = sample_rider::kImage;
  rider.sprite->use_src_rect = true;
  rider.sprite->src_w = sample_rider::kFrameW;
  rider.sprite->src_h = sample_rider::kFrameH;
  rider.animator = AnimatorData{};
  rider.animator->set = sample_rider::kSet;
  Camera2DData follow;
  follow.target = rider.id;
  follow.zoom = 1.5f;
  cam.camera = follow;
  Entity2D crate;
  crate.id = 3;
  crate.name = "SupplyCrate";
  crate.x = 96.0f;
  crate.y = -16.0f;
  crate.w = 28.0f;
  crate.h = 24.0f;
  crate.color[0] = 0.62f;
  crate.color[1] = 0.42f;
  crate.color[2] = 0.22f;
  crate.color[3] = 1.0f;
  crate.layer = 4;
  ColliderData trig;
  trig.w = crate.w;
  trig.h = crate.h;
  trig.trigger = true;
  crate.collider = trig;
  ScriptData sd;
  sd.path = kCrateScript;
  crate.script = sd;
  doc.entities = {cam, rider, crate};
  doc.zoom = 1.0f;
  return scene_json::save_file(scene.string(), doc, error);
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
