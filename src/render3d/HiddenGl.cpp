#include "render3d/HiddenGl.h"

#include "render3d/GlApi.h"

namespace ts {
namespace tombstone {
namespace render3d {

void hint_gl33_core() {
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
}

bool HiddenGl::open(std::string* why) {
  if (window_) return true;
  // glfwInit is idempotent; remember whether this call did the work so a
  // host that already runs GLFW keeps it.
  previous_ = nullptr;
  const bool was_up = glfwGetCurrentContext() != nullptr;
  if (!was_up) {
    if (glfwInit() != GLFW_TRUE) {
      const char* desc = nullptr;
      glfwGetError(&desc);
      if (why) *why = std::string("glfwInit failed: ") + (desc ? desc : "no display?");
      return false;
    }
    we_initialised_ = true;
  } else {
    previous_ = glfwGetCurrentContext();
  }
  glfwDefaultWindowHints();
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  hint_gl33_core();
  window_ = glfwCreateWindow(64, 64, "TombStone offscreen", nullptr, nullptr);
  glfwDefaultWindowHints();
  if (!window_) {
    const char* desc = nullptr;
    glfwGetError(&desc);
    if (why) *why = std::string("no OpenGL 3.3 core context: ") + (desc ? desc : "?");
    close();
    return false;
  }
  glfwMakeContextCurrent(window_);
  return true;
}

void HiddenGl::close() {
  if (window_) {
    glfwMakeContextCurrent(previous_);
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  if (we_initialised_) {
    glfwTerminate();
    we_initialised_ = false;
  }
}

std::string HiddenGl::version() const {
  if (!window_) return {};
  const GLubyte* v = glGetString(GL_VERSION);
  const GLubyte* r = glGetString(GL_RENDERER);
  return std::string(v ? reinterpret_cast<const char*>(v) : "?") + " / " +
         (r ? reinterpret_cast<const char*>(r) : "?");
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
