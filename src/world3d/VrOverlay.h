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

// Lasers and the teleport arc (call between SceneView::begin and end).
void vr_overlay_lines(const World3D& w, render3d::Renderer3D* r);

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
