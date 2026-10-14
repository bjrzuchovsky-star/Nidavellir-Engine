#include "editor/ui/QuadBlend.h"

#include <GLFW/glfw3.h>

namespace ts {
namespace tombstone {
namespace editor {
namespace quad_blend {
namespace {

void additive(const ImDrawList*, const ImDrawCmd*) { glBlendFunc(GL_SRC_ALPHA, GL_ONE); }
void multiply2x(const ImDrawList*, const ImDrawCmd*) { glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); }

}  // namespace

void Blender::set(std::uint8_t mode) {
  if (mode == mode_) return;
  mode_ = mode;
  if (mode == 1) {
    draw_->AddCallback(additive, nullptr);
  } else if (mode == 2) {
    draw_->AddCallback(multiply2x, nullptr);
  } else {
    // The backend restores its own blend state (alpha).
    draw_->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  }
}

void smooth(const std::string& key, std::uint64_t handle) {
  if (handle == 0 || key.rfind("mem:fx/", 0) != 0) return;
  GLint prev = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(handle));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
}

}  // namespace quad_blend
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
