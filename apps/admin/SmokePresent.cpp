#include "Smoke3D.h"

#include "rhi/Rhi.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <string>
#include <vector>

namespace {

int present_one(ts::tombstone::rhi::Api api, bool required) {
  using ts::tombstone::rhi::DeviceDesc;
  if (!ts::tombstone::rhi::api_built(api)) {
    std::cout << "[smoke-present] " << ts::tombstone::rhi::api_name(api) << " is not in this build\n";
    return 0;
  }
  DeviceDesc desc;
  desc.api = api;
  desc.width = 320;
  desc.height = 180;
  desc.visible = false;
  desc.title = "smoke-present";
  std::string err;
  auto device = ts::tombstone::rhi::create_device(desc, &err);
  if (!device) {
    std::cerr << "[smoke-present] " << ts::tombstone::rhi::api_name(api) << " did not start: " << err << '\n';
    return required ? 1 : 0;
  }
  const float clear[3] = {0.15f, 0.55f, 0.25f};
  std::vector<std::uint8_t> px;
  int w = 0, h = 0;
  bool read = false;
  for (int frame = 0; frame < 3; ++frame) {
    device->begin_frame();
    ts::tombstone::rhi::CommandList* cmd = device->commands();
    if (!cmd) {
      device->end_frame();
      std::cerr << "[smoke-present] " << ts::tombstone::rhi::api_name(api) << " has no command list\n";
      return 1;
    }
    ts::tombstone::rhi::PassDesc pass;
    pass.rgba[0] = clear[0];
    pass.rgba[1] = clear[1];
    pass.rgba[2] = clear[2];
    pass.rgba[3] = 1;
    pass.clear_depth = false;
    cmd->begin_pass(pass);
    cmd->end_pass();
    if (frame == 2) read = device->read_swapchain(&px, &w, &h);
    device->end_frame();
  }
  device->wait_idle();
  device.reset();
  if (!read || px.empty() || w < 2 || h < 2) {
    std::cerr << "[smoke-present] " << ts::tombstone::rhi::api_name(api) << " backbuffer read failed\n";
    return 1;
  }
  const auto at = [&](int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u;
    return std::tuple<int, int, int>(px[i], px[i + 1], px[i + 2]);
  };
  const auto [r, g, b] = at(w / 2, h / 2);
  const int want_r = static_cast<int>(clear[0] * 255.0f + 0.5f);
  const int want_g = static_cast<int>(clear[1] * 255.0f + 0.5f);
  const int want_b = static_cast<int>(clear[2] * 255.0f + 0.5f);
  auto near = [](int v, int t) { return std::abs(v - t) <= 3; };
  std::cout << "[smoke-present] " << ts::tombstone::rhi::api_name(api) << " backbuffer " << w << "x" << h
            << " rgb(" << r << "," << g << "," << b << ")\n";
  if (r + g + b < 12 || !near(r, want_r) || !near(g, want_g) || !near(b, want_b)) {
    std::cerr << "[smoke-present] " << ts::tombstone::rhi::api_name(api)
              << " backbuffer is not the clear color (wanted about rgb(" << want_r << "," << want_g << "," << want_b
              << "))\n";
    return 1;
  }
  return 0;
}

}  // namespace

int run_present_smoke() {
  using ts::tombstone::rhi::Api;
  Api only = Api::OpenGL;
  const bool pinned = ts::tombstone::rhi::cli_api(&only);
  int code = 0;
  if (!pinned || only == Api::Vulkan) code |= present_one(Api::Vulkan, pinned);
  if (!pinned || only == Api::D3D12) code |= present_one(Api::D3D12, pinned);
  if (pinned && only == Api::OpenGL) {
    std::cout << "[smoke-present] OpenGL presents through the window framebuffer; nothing to check\n";
  }
  if (code == 0) std::cout << "[smoke-present] OK\n";
  return code == 0 ? 0 : 1;
}
