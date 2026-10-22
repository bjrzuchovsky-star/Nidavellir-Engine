#pragma once

// The "Empty" claim templates: clean starting points written by the engine
// (no sample art, no optional plugins). Each writer fills a project folder
// with project.json, scene.json, README.md and, when there is a player, a
// starter scripts/player.lua. Everything is engine-made (the VR hands are
// the engine's own CC0 hand.gltf).
//
//   empty-2d              camera, one empty tilemap layer, 2D lighting off
//   empty-2d-topdown      top-down player, follow camera, empty tilemap
//   empty-2d-platformer   platformer player (gravity, jump), solid ground
//                         tiles, follow camera
//   empty-3d              camera, sun, sky, ground plane
//   empty-3d-fps          first-person controller + its camera, ground
//   empty-3d-tps          third-person controller with the orbit camera
//   empty-vr              3D with VR on: XR rig with hands, teleport + snap
//                         turn, ground; rides flat on the desktop too
//   truly-blank-2d / -3d  no entities at all

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace empty_templates {

// Generator ids handled here (TemplateStore::run_generator forwards them).
const std::vector<std::string>& ids();
bool handles(const std::string& id);

// Writes the claim into `dir` (created if missing). False with the reason.
bool write(const std::string& id, const std::string& dir, const std::string& name,
           std::string* error);

}  // namespace empty_templates
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
