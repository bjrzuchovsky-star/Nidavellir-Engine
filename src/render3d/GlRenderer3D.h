#pragma once

// OpenGL 3.3 core forward renderer (needs a current 3.3 core context; the
// admin's and ts_game's 3D windows ask GLFW for one).
//
// Per frame: sun shadow map (2048^2 depth, 3x3 PCF) -> gradient sky ->
// opaque meshes with a PBR-lite shader (GGX specular, Lambert diffuse,
// metal / rough, albedo + normal + metal-rough textures, emissive, up to
// 8 point lights, hemisphere ambient, Reinhard tone map, gamma 2.2) ->
// debug lines (depth-tested, then overlay). Optionally into its own
// offscreen color + depth target that callers read back or show in ImGui.
// GL state the caller had (framebuffer, viewport, program, VAO, enables)
// is restored after end_frame().

#include "render3d/Renderer3D.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace render3d {

// "gl" backend; init() fails (with the reason) without a usable context.
std::unique_ptr<Renderer3D> make_gl_renderer3d();

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
