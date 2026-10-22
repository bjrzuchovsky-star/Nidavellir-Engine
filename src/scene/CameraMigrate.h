#pragma once

// v13 camera migration for 2D scenes and the file side of both camera
// migrations (docs/camera.md). A v12- 2D scene with a rider and no Camera2D
// played through an implicit, rigid camera on player 0; the migration adds
// that camera as a real "Camera" entity (follows the rider, smoothing 0) so
// it is visible, selectable and editable. Scenes that had a Camera2D, or no
// rider, are unchanged.

#include "scene/SceneData.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace camera_migrate {

// `version` is the file's version as read. Returns the entities added.
int migrate_2d(int version, std::vector<Entity2D>* entities, std::vector<std::string>* log);

// Copy `scene_file` to <claim>/.tombstone/backup/<name>.pre-cameras (first
// free -N suffix) before the migrated scene is written. Returns the backup
// path relative to the claim ("" + *error on failure).
std::string backup_scene(const std::string& claim_dir, const std::string& scene_file,
                         std::string* error);
// Append lines to <claim>/.tombstone/migrations.log (UTC stamps).
void append_log(const std::string& claim_dir, const std::vector<std::string>& lines);

}  // namespace camera_migrate
}  // namespace tombstone
}  // namespace ts
