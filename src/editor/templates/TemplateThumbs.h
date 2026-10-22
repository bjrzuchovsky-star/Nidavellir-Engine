#pragma once

// Template thumbnails rendered by the engine (ts_admin --bake-templates,
// --bake-thumbnails). 2D claims go through the runtime's own draw list
// (runtime::World after a few Play ticks) onto a CPU canvas with the
// editor's tile grid; 3D claims through the GL renderer (SceneView) in a
// hidden context, so 3D thumbnails need a display (Xvfb on Linux).

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace template_thumbs {

constexpr int kWidth = 480;
constexpr int kHeight = 250;

// Renders the claim in `project_dir` to `png`. False with the reason.
bool render_project(const std::string& project_dir, const std::string& png,
                    std::string* error);

// For every template under `templates_root` with a project/ folder (or a
// generator), writes <template>/thumbnail.png. Returns how many.
int bake(const std::string& templates_root, std::vector<std::string>* errors);

}  // namespace template_thumbs
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
