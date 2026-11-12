#include "GfxHost.h"

#include "editor/settings/Settings.h"
#include "gfx/GlTextureUploader.h"
#include "gfx/TextureCache.h"
#include "rhi/Rhi.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <volk.h>
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#include <imgui_impl_vulkan.h>

#if defined(_WIN32)
#include <imgui_impl_dx12.h>
#include <d3d12.h>
#endif

#include <iostream>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace admin_gfx {

namespace {
rhi::Device* g_device = nullptr;
bool g_gl = true;

class RhiUploader final : public gfx::TextureUploader {
 public:
  explicit RhiUploader(rhi::Device* device) : device_(device) {}
  std::uint64_t upload_rgba(const unsigned char* pixels, int width, int height) override {
    if (!device_ || !pixels) return 0;
    rhi::TextureDesc td;
    td.width = width;
    td.height = height;
    rhi::Texture* tex = device_->create_texture(td, pixels);
    if (!tex) return 0;
    const std::uint64_t id = device_->imgui_texture(tex);
    live_[id] = tex;
    return id;
  }
  void destroy(std::uint64_t handle) override {
    auto it = live_.find(handle);
    if (it == live_.end()) return;
    device_->destroy_texture(it->second);
    live_.erase(it);
  }
  rhi::Device* device_ = nullptr;
  std::unordered_map<std::uint64_t, rhi::Texture*> live_;
};

RhiUploader* g_uploader = nullptr;
gfx::GlTextureUploader g_gl_uploader;

std::uint64_t add_vk_texture(rhi::Api, void* view, void*) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
      ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(view), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)));
}
void remove_vk_texture(rhi::Api, std::uint64_t id, void*) {
  ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(id)));
}

void check_vk(VkResult err) {
  if (err < 0) std::cerr << "Vulkan error " << static_cast<int>(err) << "\n";
}
}  // namespace

bool Host::open(int width, int height, const char* title) {
  editor::Settings boot = editor::SettingsStore::load(nullptr);
  rhi::Api want = rhi::resolve_api(boot.graphics_api);
  std::string err;
  if (want != rhi::Api::OpenGL) {
    rhi::DeviceDesc desc;
    desc.api = want;
    desc.width = width;
    desc.height = height;
    desc.title = title;
    auto dev = rhi::create_device(desc, &err);
    if (!dev) {
      std::cerr << rhi::api_name(want) << " did not start (" << err << "). Falling back to OpenGL.\n";
    } else {
      g_device = dev.release();
      rhi::set_active(g_device);
      window = static_cast<GLFWwindow*>(g_device->window());
      g_gl = false;
    }
  }
  if (!window) {
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window) return false;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    g_gl = true;
    rhi::DeviceDesc desc;
    desc.api = rhi::Api::OpenGL;
    desc.window = window;
    desc.width = width;
    desc.height = height;
    auto dev = rhi::create_device(desc, &err);
    if (dev) {
      g_device = dev.release();
      rhi::set_active(g_device);
    }
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  if (g_gl) {
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    gfx::set_texture_uploader(&g_gl_uploader);
  } else if (g_device && g_device->api() == rhi::Api::Vulkan) {
    rhi::Device::VkInfo info;
    g_device->vk_info(&info);
    volkLoadInstance(static_cast<VkInstance>(info.instance));
    ImGui_ImplVulkan_LoadFunctions(info.api_version,
                                   [](const char* name, void* user) {
                                     return vkGetInstanceProcAddr(static_cast<VkInstance>(user), name);
                                   },
                                   info.instance);
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = info.api_version;
    init.Instance = static_cast<VkInstance>(info.instance);
    init.PhysicalDevice = static_cast<VkPhysicalDevice>(info.physical);
    init.Device = static_cast<VkDevice>(info.device);
    init.QueueFamily = info.family;
    init.Queue = static_cast<VkQueue>(info.queue);
    init.DescriptorPoolSize = 128;
    init.MinImageCount = 2;
    init.ImageCount = info.images < 2 ? 2 : info.images;
    init.PipelineInfoMain.RenderPass = static_cast<VkRenderPass>(info.render_pass);
    init.PipelineInfoMain.Subpass = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoForViewports.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = check_vk;
    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_Init(&init);
    rhi::set_texture_registrar(add_vk_texture, remove_vk_texture, nullptr);
    g_uploader = new RhiUploader(g_device);
    gfx::set_texture_uploader(g_uploader);
  }
#if defined(_WIN32)
  else if (g_device && g_device->api() == rhi::Api::D3D12) {
    rhi::Device::DxInfo info;
    g_device->dx_info(&info);
    ImGui_ImplDX12_InitInfo init{};
    init.Device = static_cast<ID3D12Device*>(info.device);
    init.CommandQueue = static_cast<ID3D12CommandQueue*>(info.queue);
    init.NumFramesInFlight = info.frames;
    init.RTVFormat = static_cast<DXGI_FORMAT>(info.format);
    init.SrvDescriptorHeap = static_cast<ID3D12DescriptorHeap*>(info.srv_heap);
    ImGui_ImplGlfw_InitForOther(window, true);
    init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
      static UINT next = 0;
      auto* heap = static_cast<ID3D12DescriptorHeap*>(info->SrvDescriptorHeap);
      auto* dev = static_cast<ID3D12Device*>(info->Device);
      const UINT stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      *cpu = heap->GetCPUDescriptorHandleForHeapStart();
      cpu->ptr += static_cast<SIZE_T>(next) * stride;
      *gpu = heap->GetGPUDescriptorHandleForHeapStart();
      gpu->ptr += static_cast<SIZE_T>(next) * stride;
      ++next;
    };
    init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
    ImGui_ImplDX12_Init(&init);
    g_uploader = new RhiUploader(g_device);
    gfx::set_texture_uploader(g_uploader);
  }
#endif
  std::cout << "Graphics API: " << api_name() << "\n";
  return window != nullptr;
}

const char* Host::api_name() const {
  if (!g_device) return "OpenGL";
  return rhi::api_name(g_device->api());
}

void Host::new_frame() {
  if (g_device) g_device->begin_frame();
  if (g_gl) ImGui_ImplOpenGL3_NewFrame();
  else if (g_device && g_device->api() == rhi::Api::Vulkan) ImGui_ImplVulkan_NewFrame();
#if defined(_WIN32)
  else if (g_device && g_device->api() == rhi::Api::D3D12) ImGui_ImplDX12_NewFrame();
#endif
  ImGui_ImplGlfw_NewFrame();
}

void Host::present(float r, float g, float b) {
  ImGui::Render();
  ImGuiIO& io = ImGui::GetIO();
  if (g_gl) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      // Same GL profile as the main window, or the shared context fails to create.
      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
      glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
      glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
      glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
      GLFWwindow* backup = glfwGetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      glfwMakeContextCurrent(backup);
    }
    if (g_device) g_device->end_frame();
    else glfwSwapBuffers(window);
    return;
  }
  if (g_device && g_device->api() == rhi::Api::Vulkan) {
    rhi::Device::VkInfo info;
    g_device->vk_info(&info);
    rhi::CommandList* cmd = g_device->commands();
    if (cmd) {
      rhi::PassDesc pass;
      pass.rgba[0] = r;
      pass.rgba[1] = g;
      pass.rgba[2] = b;
      pass.rgba[3] = 1;
      cmd->begin_pass(pass);
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(info.command));
      cmd->end_pass();
    }
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
    }
    g_device->end_frame();
    return;
  }
#if defined(_WIN32)
  if (g_device && g_device->api() == rhi::Api::D3D12) {
    rhi::Device::DxInfo info;
    g_device->dx_info(&info);
    auto* list = static_cast<ID3D12GraphicsCommandList*>(info.list);
    if (list && info.srv_heap) {
      ID3D12DescriptorHeap* heaps[] = {static_cast<ID3D12DescriptorHeap*>(info.srv_heap)};
      list->SetDescriptorHeaps(1, heaps);
      ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
    }
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
    }
    g_device->end_frame();
  }
#endif
}

void Host::shutdown() {
  gfx::set_texture_uploader(nullptr);
  delete g_uploader;
  g_uploader = nullptr;
  if (g_gl) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
  } else if (g_device && g_device->api() == rhi::Api::Vulkan) {
    g_device->wait_idle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    rhi::set_texture_registrar(nullptr, nullptr, nullptr);
  }
#if defined(_WIN32)
  else if (g_device && g_device->api() == rhi::Api::D3D12) {
    g_device->wait_idle();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplGlfw_Shutdown();
  }
#endif
  ImGui::DestroyContext();
  rhi::set_active(nullptr);
  delete g_device;
  g_device = nullptr;
  if (g_gl && window) {
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  window = nullptr;
}

}  // namespace admin_gfx
}  // namespace tombstone
}  // namespace ts
