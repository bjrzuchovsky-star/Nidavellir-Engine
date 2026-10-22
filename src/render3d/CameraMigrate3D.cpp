#include "render3d/CameraMigrate3D.h"

#include "render3d/Attach3D.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace scene3d {

Entity3D make_camera_entity(std::uint64_t id, const std::string& name) {
  Entity3D e;
  e.id = id;
  e.name = name;
  e.camera = PlayCamera3D{};
  return e;
}

int migrate_cameras(Scene3D* scene, std::vector<std::string>* log) {
  if (!scene || !scene->legacy_cameras) return 0;
  scene->legacy_cameras = false;
  auto say = [&](const std::string& s) {
    if (log) log->push_back(s);
  };
  std::vector<Entity3D>& ents = scene->entities;
  const Entity3D* rider = nullptr;
  for (const Entity3D& e : ents) {
    if (e.player) {
      rider = &e;
      break;
    }
  }
  if (!rider) {
    say("Cameras: no rider in this scene; camera entities keep leading as before");
    return 0;
  }
  const std::uint64_t rider_id = rider->id;
  const Player3D cfg = *rider->player;
  const Transform rider_t = rider->transform;
  const std::string rider_name = rider->name;
  const bool others = std::any_of(ents.begin(), ents.end(), [](const Entity3D& e) { return e.camera.has_value(); });
  const int priority = others ? 10 : 0;
  if (cfg.view == Player3D::View::FirstPerson) {
    for (Entity3D& c : ents) {
      if (c.parent == rider_id && c.camera) {
        c.camera->rig = PlayCamera3D::Rig::FirstPerson;
        c.camera->follow = rider_id;
        c.camera->priority = priority;
        c.camera->active = true;
        say("Cameras: " + c.name + " is now the first-person camera of " + rider_name);
        return 1;
      }
    }
  }
  Entity3D cam = make_camera_entity(scene->next_id(), "Main Camera");
  PlayCamera3D& pc = *cam.camera;
  pc.follow = rider_id;
  pc.priority = priority;
  if (cfg.view == Player3D::View::FirstPerson) {
    pc.rig = PlayCamera3D::Rig::FirstPerson;
    cam.transform.position = rider_t.position + Vec3{0.0f, cfg.eye_height - 1.0f, 0.0f};
    cam.transform.rotation = look_rotation(Vec3{0.0f, 0.0f, -1.0f});
    say("Cameras: added Main Camera (first person, eyes of " + rider_name + ")");
  } else {
    pc.rig = PlayCamera3D::Rig::Orbit;
    pc.distance = cfg.camera_distance;
    pc.height = cfg.camera_height;
    pc.collision = true;
    const float yaw = render3d::radians(scene->editor_camera.yaw);
    const Vec3 target = rider_t.position + Vec3{0.0f, 1.0f, 0.0f};
    cam.transform.position = target + Vec3{std::sin(yaw) * cfg.camera_distance, cfg.camera_height - 1.0f,
                                           std::cos(yaw) * cfg.camera_distance};
    cam.transform.rotation = look_rotation(target - cam.transform.position);
    say("Cameras: added Main Camera (orbit behind " + rider_name + ", " +
        std::to_string(static_cast<int>(std::lround(cfg.camera_distance * 10.0f)) / 10) + "." +
        std::to_string(static_cast<int>(std::lround(cfg.camera_distance * 10.0f)) % 10) +
        " m back, collision on) from the old follow camera");
  }
  if (others) say("Cameras: other camera entities stay, under Main Camera (priority 10)");
  ents.push_back(std::move(cam));
  return 1;
}

}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
