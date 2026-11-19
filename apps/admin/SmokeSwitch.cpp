#include "Smoke3D.h"

#include "GfxHost.h"

#include "editor/AppFlow.h"
#include "editor/AppState.h"
#include "editor/projects/ProjectStore.h"
#include "rhi/Rhi.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke-switch] FAILED: " << what << '\n';
  return 1;
}

void pump(ts::tombstone::admin_gfx::Host& host, ts::tombstone::editor::AppFlow& flow, float dt) {
  if (host.window) glfwPollEvents();
  host.new_frame();
  ImGui::NewFrame();
  if (flow.is_running()) flow.tick(dt);
  host.present(0.12f, 0.11f, 0.10f);
}

int switch_one(ts::tombstone::rhi::Api api, bool required, bool retarget) {
  using namespace ts::tombstone;
  const char* name = rhi::api_name(api);
  if (!rhi::api_built(api)) {
    std::cout << "[smoke-switch] " << name << " is not in this build\n";
    return required ? 1 : 0;
  }
  // Host::open follows --gfx=. Retarget only when the process was not pinned,
  // so one smoke can walk OpenGL then Vulkan.
  if (retarget) rhi::set_cli_api(api);

  const fs::path dir = fs::absolute(fs::path("TombStoneProjects") / (std::string("_switch_") + rhi::api_flag(api)));
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

  admin_gfx::Host host;
  if (!host.open(960, 540, "smoke-switch")) {
    if (api == rhi::Api::OpenGL && !std::getenv("TS_SMOKE_REQUIRE_GL")) {
      std::cout << "[smoke-switch] " << name
                << " SKIPPED (no window; set TS_SMOKE_REQUIRE_GL=1 to fail)\n";
      return 0;
    }
    return fail(std::string(name) + " window did not open");
  }
  if (!rhi::active() || rhi::active()->api() != api) {
    const char* got = rhi::active() ? rhi::api_name(rhi::active()->api()) : "nothing";
    host.shutdown();
    if (!required) {
      std::cout << "[smoke-switch] " << name << " did not start (got " << got << ")\n";
      return 0;
    }
    return fail(std::string(name) + " did not start (got " + got + ")");
  }
  int code = 0;
  {
    editor::AppFlow flow;
    flow.start();
    for (int i = 0; i < 12 && flow.is_running() && flow.current_state() != editor::AppState::ProjectManager; ++i) {
      if (flow.current_state() == editor::AppState::Login) {
        if (host.window) glfwPollEvents();
        host.new_frame();
        ImGui::NewFrame();
        flow.submit_dev_login();
        host.present(0.12f, 0.11f, 0.10f);
      } else {
        pump(host, flow, 0.5f);
      }
    }
    if (flow.current_state() != editor::AppState::ProjectManager) {
      code = fail(std::string(name) + " did not reach the Project Manager");
    }
    int index = -1;
    if (!code) {
      const auto& projects = flow.projects();
      for (std::size_t i = 0; i < projects.size(); ++i) {
        if (projects[i].path == info.path || projects[i].name == info.name) index = static_cast<int>(i);
      }
      if (index < 0) code = fail(std::string(name) + " project is not in the Project Manager");
    }
    if (!code) {
      for (int i = 0; i < 2; ++i) pump(host, flow, 0.016f);
      // Same moment as a double-click: the frame list is open, then Editor3D
      // uploads onto that device.
      if (host.window) glfwPollEvents();
      host.new_frame();
      ImGui::NewFrame();
      const bool opened = flow.select_project(static_cast<std::size_t>(index));
      host.present(0.12f, 0.11f, 0.10f);
      if (!opened || flow.current_state() != editor::AppState::Editor3D) {
        code = fail(std::string(name) + " double-click did not open Editor3D");
      }
    }
    if (!code) {
      for (int i = 0; i < 3 && flow.is_running(); ++i) pump(host, flow, 0.016f);
      if (flow.current_state() != editor::AppState::Editor3D) {
        code = fail(std::string(name) + " left Editor3D");
      }
    }
    if (!code && api == rhi::Api::D3D12 && rhi::gfx_debug() && rhi::active()) {
      const int errors = rhi::active()->debug_error_count();
      if (errors > 0) {
        code = fail(std::string(name) + " debug layer reported " + std::to_string(errors) + " error(s)");
      }
    }
    flow.release_screens();
  }
  host.shutdown();
  if (!code) std::cout << "[smoke-switch] " << name << " Project Manager opened Editor3D\n";
  return code;
}

}  // namespace

int run_switch_smoke() {
  using ts::tombstone::rhi::Api;
  Api only = Api::OpenGL;
  const bool pinned = ts::tombstone::rhi::cli_api(&only);
  int code = 0;
  if (!pinned || only == Api::OpenGL) code |= switch_one(Api::OpenGL, pinned, !pinned);
  if (!pinned || only == Api::Vulkan) code |= switch_one(Api::Vulkan, pinned, !pinned);
  if (!pinned || only == Api::D3D12) code |= switch_one(Api::D3D12, pinned, !pinned);
  if (code == 0) std::cout << "[smoke-switch] OK\n";
  return code == 0 ? 0 : 1;
}
