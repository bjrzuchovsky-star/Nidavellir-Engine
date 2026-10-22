#pragma once

// v18 camera migration for 3D scenes (docs/camera.md). Scenes from before
// v18 had a built-in rider follow camera and camera entities that only
// counted without a rider (or as first-person eyes). This turns that into
// real camera entities that look exactly the same in Play:
//   * third-person rider: a "Main Camera" (Orbit rig following the rider,
//     its camera_distance / camera_height, collision on);
//   * first-person rider with a child camera: that camera becomes the
//     FirstPerson rig of the rider;
//   * first-person rider without one: a "Main Camera" FirstPerson rig;
//   * no rider: nothing to add (the first camera entity leads, as before).
// Other camera entities a rider scene had (ignored before) stay, under the
// new camera (it gets priority 10).

#include "render3d/Scene3D.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace scene3d {

// Changes the scene (and clears legacy_cameras). Returns how many entities
// were added or changed; `log` gets one line per change. No-op (0) for v18
// scenes.
int migrate_cameras(Scene3D* scene, std::vector<std::string>* log);

// A fresh camera entity (Fixed, looking down -Z) named `name`.
Entity3D make_camera_entity(std::uint64_t id, const std::string& name);

}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
