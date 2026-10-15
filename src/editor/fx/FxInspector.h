#pragma once
// Editor widgets for particles and 2D lighting, shared by the 2D and 3D
// editors: the Particles inspector (with a live preview), the Light 2D
// inspector and the scene Lighting settings panel.

#include "fx/Light2D.h"
#include "fx/Particles.h"

namespace ts {
namespace tombstone {
namespace editor {

// Live = a drag in flight (apply without an undo step), Commit = one undo
// step, Remove = the component's Remove button.
enum class FxEdit { None, Live, Commit, Remove };

FxEdit draw_particle_fields(fx::EmitterDesc& d, fx::Space space);
FxEdit draw_light_fields(fx::Light2DData& l);
FxEdit draw_lighting_fields(fx::Lighting2D& l);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
