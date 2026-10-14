#pragma once

// A hidden GLFW window with an OpenGL 3.3 core context, for headless
// renders (--smoke pixel checks, thumbnails). Initialises GLFW when needed
// and terminates it again only if it did the init. Under Linux it needs an
// X display (Xvfb works); open() says why when there is none.

#include <string>

struct GLFWwindow;

namespace ts {
namespace tombstone {
namespace render3d {

class HiddenGl {
 public:
  HiddenGl() = default;
  ~HiddenGl() { close(); }
  HiddenGl(const HiddenGl&) = delete;
  HiddenGl& operator=(const HiddenGl&) = delete;

  bool open(std::string* why);
  void close();
  bool ok() const { return window_ != nullptr; }
  // "4.5 (Core Profile) Mesa ..." etc.
  std::string version() const;

 private:
  GLFWwindow* window_ = nullptr;
  GLFWwindow* previous_ = nullptr;
  bool we_initialised_ = false;
};

// Window hints for a 3.3 core context (shared by the hidden context and
// the 3D windows of ts_game).
void hint_gl33_core();

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
