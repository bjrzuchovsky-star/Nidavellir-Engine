#include "GpuCheck.h"

#include "render3d/HiddenGl.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ts {
namespace tombstone {
namespace game {

std::string last_glfw_error() {
  const char* desc = nullptr;
  glfwGetError(&desc);
  return desc ? desc : "";
}

void show_gpu_error(const std::string& game_name, const std::string& detail) {
  const std::string msg =
      game_name + " needs a graphics card that supports OpenGL 3.3, and this PC's graphics "
      "driver did not provide it.\n\n"
      "Please install the latest driver for your graphics card (NVIDIA, AMD or Intel) and "
      "try again. Remote desktop sessions and basic display adapters often lack OpenGL 3.3." +
      (detail.empty() ? std::string() : "\n\nDetails: " + detail);
  std::cerr << "[ts_game] " << msg << '\n';
#if defined(_WIN32)
  const char* nd = std::getenv("TS_NO_DIALOGS");
  if (nd && nd[0] == '1') return;
  const int n = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n : 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, w.data(), n);
  const std::string title = game_name + " - graphics card not supported";
  const int tn = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
  std::wstring wt(tn > 0 ? tn : 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wt.data(), tn);
  MessageBoxW(nullptr, w.c_str(), wt.c_str(), MB_OK | MB_ICONWARNING);
#endif
}

int run_check_gpu(bool quiet) {
  render3d::HiddenGl gl;
  std::string why;
  if (!gl.open(&why)) {
    if (why.empty()) why = last_glfw_error();
    std::cout << "[ts_game] GPU check: no OpenGL 3.3 core context (" << why << ")\n";
    if (!quiet) show_gpu_error("This game", why);
    return 3;
  }
  const std::string v = gl.version();
  int major = 0, minor = 0;
  std::sscanf(v.c_str(), "%d.%d", &major, &minor);
  if (major < 3 || (major == 3 && minor < 3)) {
    std::cout << "[ts_game] GPU check: OpenGL " << v << " is older than 3.3\n";
    if (!quiet) show_gpu_error("This game", "OpenGL " + v);
    return 3;
  }
  std::cout << "[ts_game] GPU check OK: OpenGL " << v << '\n';
  return 0;
}

}  // namespace game
}  // namespace tombstone
}  // namespace ts
