#pragma once

// DrawQuad::blend for ImGui draw lists (the 2D Play / posse viewports):
// additive particles and the 2x-multiply lightmap switch the GL blend
// function through draw-list callbacks; smooth() turns on linear filtering
// for the "mem:fx/" images (lightmap, soft particle dot).

#include <imgui.h>

#include <cstdint>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace quad_blend {

// Tracks the current mode per draw list; call end() before other drawing.
class Blender {
 public:
  explicit Blender(ImDrawList* draw) : draw_(draw) {}
  void set(std::uint8_t mode);
  void end() { set(0); }

 private:
  ImDrawList* draw_;
  std::uint8_t mode_ = 0;
};

// Linear filtering for "mem:fx/..." textures (no-op for others).
void smooth(const std::string& key, std::uint64_t handle);

}  // namespace quad_blend
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
