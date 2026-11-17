#include "Smoke3D.h"

#include "GfxHost.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke-window] FAILED: " << what << '\n';
  return 1;
}

bool center_is(const std::vector<unsigned char>& px, int w, int h, float r, float g, float b) {
  if (w < 2 || h < 2 || px.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u) return false;
  const std::size_t i =
      (static_cast<std::size_t>(h / 2) * static_cast<std::size_t>(w) + static_cast<std::size_t>(w / 2)) * 4u;
  auto near = [](int v, float t) { return std::abs(v - static_cast<int>(t * 255.0f + 0.5f)) <= 3; };
  return near(px[i], r) && near(px[i + 1], g) && near(px[i + 2], b);
}

}  // namespace

// The editor's real window. --smoke-realm stays hidden, which is the path
// that never hung. This one is shown, cleared, and closed.
int run_window_smoke() {
  ts::tombstone::admin_gfx::Host host;
  if (!host.open(640, 360, "smoke-window")) return fail("window did not open");
  const std::string api = host.api_name();
  if (!host.window || glfwGetWindowAttrib(host.window, GLFW_VISIBLE) != GLFW_TRUE) {
    host.shutdown();
    return fail(api + " window is not visible");
  }
  int code = 0;
  const bool gl = api == "OpenGL";
  if (gl) {
    glfwMakeContextCurrent(host.window);
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(host.window, &fw, &fh);
    if (fw < 2 || fh < 2) {
      code = fail("OpenGL framebuffer " + std::to_string(fw) + "x" + std::to_string(fh));
    } else {
      // 1.1 entry points. glfwGetProcAddress does not return these.
      const float rgba[3] = {0.15f, 0.55f, 0.25f};
      glViewport(0, 0, fw, fh);
      glClearColor(rgba[0], rgba[1], rgba[2], 1);
      glClear(GL_COLOR_BUFFER_BIT);
      std::vector<unsigned char> px(static_cast<std::size_t>(fw) * static_cast<std::size_t>(fh) * 4u);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadBuffer(GL_BACK);
      glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      const std::size_t i =
          (static_cast<std::size_t>(fh / 2) * static_cast<std::size_t>(fw) + static_cast<std::size_t>(fw / 2)) * 4u;
      std::cout << "[smoke-window] OpenGL visible " << fw << "x" << fh << " rgb(" << static_cast<int>(px[i]) << ","
                << static_cast<int>(px[i + 1]) << "," << static_cast<int>(px[i + 2]) << ")\n";
      if (!center_is(px, fw, fh, rgba[0], rgba[1], rgba[2])) code = fail("visible window is not the clear color");
    }
  }
  if (!code) {
    for (int frame = 0; frame < 3; ++frame) {
      if (glfwWindowShouldClose(host.window)) {
        code = fail("window closed itself");
        break;
      }
      glfwPollEvents();
      host.new_frame();
      ImGui::NewFrame();
      host.present(0.15f, 0.55f, 0.25f);
    }
  }
  host.shutdown();
  if (!code) {
    std::cout << "[smoke-window] " << api << " visible window opened, drew, and closed\n";
    std::cout << "[smoke-window] OK\n";
  }
  return code;
}
