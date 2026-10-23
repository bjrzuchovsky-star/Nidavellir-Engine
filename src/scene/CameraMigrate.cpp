#include "scene/CameraMigrate.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {
namespace camera_migrate {

int migrate_2d(int version, std::vector<Entity2D>* entities, std::vector<std::string>* log) {
  if (version >= 13 || !entities) return 0;
  const Entity2D* rider = nullptr;
  std::uint64_t max_id = 0;
  bool has_cam = false;
  for (const Entity2D& e : *entities) {
    max_id = std::max(max_id, e.id);
    has_cam = has_cam || e.camera.has_value();
    if (!rider && e.player && e.player->slot == 0) rider = &e;
  }
  if (has_cam || !rider) return 0;
  Entity2D cam;
  cam.id = max_id + 1;
  cam.name = "Camera";
  cam.w = 48.0f;
  cam.h = 32.0f;
  cam.x = rider->center_x() - cam.w * 0.5f;
  cam.y = rider->center_y() - cam.h * 0.5f;
  cam.color[0] = 0.95f;
  cam.color[1] = 0.75f;
  cam.color[2] = 0.25f;
  cam.color[3] = 1.0f;
  cam.layer = 10;
  Camera2DData c;
  c.target = rider->id;
  c.smoothing = 0.0f;  // the old implicit camera was locked on
  cam.camera = c;
  const std::string rider_name = rider->name;
  entities->push_back(cam);
  if (log) log->push_back("Cameras: added Camera (follows " + rider_name + ", locked on) for the old built-in camera");
  return 1;
}

namespace {
std::string now_utc() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}
}  // namespace

std::string backup_scene(const std::string& claim_dir, const std::string& scene_file,
                         std::string* error) {
  std::error_code ec;
  const fs::path src(scene_file);
  const fs::path dir = fs::path(claim_dir) / ".tombstone" / "backup";
  fs::create_directories(dir, ec);
  const std::string base = src.filename().string() + ".pre-cameras";
  fs::path dst = dir / base;
  for (int n = 2; fs::exists(dst, ec); ++n) dst = dir / (base + "-" + std::to_string(n));
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + src.string();
    return "";
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::ofstream out(dst, std::ios::binary);
  out << text;
  if (!out) {
    if (error) *error = "cannot write " + dst.string();
    return "";
  }
  return fs::relative(dst, claim_dir, ec).generic_string();
}

void append_log(const std::string& claim_dir, const std::vector<std::string>& lines) {
  std::error_code ec;
  const fs::path p = fs::path(claim_dir) / ".tombstone" / "migrations.log";
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::app);
  for (const std::string& l : lines) out << now_utc() << "  " << l << '\n';
}

}  // namespace camera_migrate
}  // namespace tombstone
}  // namespace ts
