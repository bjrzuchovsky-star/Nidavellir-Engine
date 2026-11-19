#include "Smoke3D.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/projects/ProjectStore.h"
#include "rhi/Rhi.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke-realm] FAILED: " << what << '\n';
  return 1;
}

int open_one(ts::tombstone::rhi::Api api, bool required) {
  using namespace ts::tombstone;
  const char* name = rhi::api_name(api);
  if (!rhi::api_built(api)) {
    std::cout << "[smoke-realm] " << name << " is not in this build\n";
    return required ? 1 : 0;
  }
  const fs::path dir = fs::absolute(fs::path("TombStoneProjects") / (std::string("_realm_") + rhi::api_flag(api)));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  editor::ProjectInfo info;
  info.id = dir.filename().string();
  info.name = info.id;
  info.kind = editor::ProjectKind::ThreeD;
  info.path = dir.string();
  std::string err;
  if (!editor::ProjectStore::write_project_json(info, &err)) return fail(std::string(name) + " project: " + err);

  rhi::DeviceDesc desc;
  desc.api = api;
  desc.width = 320;
  desc.height = 180;
  desc.visible = false;
  desc.title = "smoke-realm";
  auto device = rhi::create_device(desc, &err);
  if (!device) {
    std::cerr << "[smoke-realm] " << name << " did not start: " << err << '\n';
    return required ? 1 : 0;
  }
  rhi::set_active(device.get());
  int code = 0;
  {
    editor::Editor3DScreen screen(info);
    screen.on_enter();
    render3d::FrameStats stats;
    for (int i = 0; i < 3; ++i) stats = screen.render_frame(320, 180);
    if (stats.draws < 1) {
      code = fail(std::string(name) + " realm rendered " + std::to_string(stats.draws) + " draws");
    }
    if (!code && api == rhi::Api::D3D12 && device->debug_error_count() > 0) {
      code = fail(std::string(name) + " debug layer reported " + std::to_string(device->debug_error_count()) +
                  " error(s)");
    }
    // Meshes go first, while the device is still alive.
    screen.on_exit();
  }
  rhi::set_active(nullptr);
  device.reset();
  if (!code) std::cout << "[smoke-realm] " << name << " opened, drew, and closed\n";
  return code;
}

}  // namespace

int run_realm_smoke() {
  using ts::tombstone::rhi::Api;
  Api only = Api::OpenGL;
  const bool pinned = ts::tombstone::rhi::cli_api(&only);
  int code = 0;
  if (!pinned || only == Api::OpenGL) code |= open_one(Api::OpenGL, pinned);
  if (!pinned || only == Api::Vulkan) code |= open_one(Api::Vulkan, pinned);
  if (!pinned || only == Api::D3D12) code |= open_one(Api::D3D12, pinned);
  if (code == 0) std::cout << "[smoke-realm] OK\n";
  return code == 0 ? 0 : 1;
}
