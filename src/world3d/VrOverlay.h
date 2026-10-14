#pragma once

// What a VR frame draws on top of the scene: controller models (simple
// primitives), the held-object highlight, teleport marker, comfort-menu
// buttons and the comfort vignette (as extra entities), plus the lasers
// and the teleport arc (as lines). Used by every eye and by the editor's
// mirror; no GL here.

#include "render3d/Renderer3D.h"
#include "world3d/World3D.h"

#include <vector>

namespace ts {
namespace tombstone {
namespace world3d {

// Ids of overlay entities start here (never collide with scene ids).
inline constexpr std::uint64_t kVrOverlayIdBase = 0x7F00000000000000ull;

// Append the overlay entities for the current state of `w`.
// `vignette` adds the comfort shell around the head (eye views only).
void vr_overlay_entities(const World3D& w, bool vignette, std::vector<scene3d::Entity3D>* out);

// A flat UI panel floating in the world: the in-game UI in VR. `image` is
// a core/MemImages key (the UI raster), shown unlit on a plane whose
// front faces the player; u runs right, v runs down.
struct VrUiPanel {
  bool visible = false;
  Vec3 center;
  render3d::Quat rotation;
  float width = 0.64f, height = 0.36f;  // metres
  std::string image = "mem:ui/vr_panel";
};
// Put the panel `distance` m in front of the head, a little below eye
// level, turned to face it (yaw only).
void vr_place_ui_panel(const World3D& w, float distance, VrUiPanel* p);
void vr_ui_panel_entity(const VrUiPanel& p, std::vector<scene3d::Entity3D>* out);
// Where hand `hand`'s laser meets the panel, 0..1 from its top-left; false
// when it misses (or points away).
bool vr_ui_panel_hit(const World3D& w, const VrUiPanel& p, int hand, float* u, float* v);

// Lasers and the teleport arc (call between SceneView::begin and end).
void vr_overlay_lines(const World3D& w, render3d::Renderer3D* r);

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
