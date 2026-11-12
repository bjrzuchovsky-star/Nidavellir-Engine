#pragma once

struct GLFWwindow;

namespace ts {
namespace tombstone {
namespace admin_gfx {

// Opens the editor window on the requested graphics API (settings, or
// --gfx=). Vulkan and Direct3D 12 fall back to OpenGL when they cannot start.
struct Host {
  GLFWwindow* window = nullptr;
  bool open(int width, int height, const char* title);
  void new_frame();
  void present(float r, float g, float b);
  void shutdown();
  const char* api_name() const;
};

}  // namespace admin_gfx
}  // namespace tombstone
}  // namespace ts
