#include "rhi/Rhi.h"

#include <algorithm>
#include <cstring>

namespace ts {
namespace tombstone {
namespace rhi {

namespace {
Device* g_active = nullptr;
bool g_cli = false;
Api g_cli_api = Api::OpenGL;
std::size_t g_align = 256;
TextureRegistrar g_add = nullptr;
TextureUnregistrar g_remove = nullptr;
void* g_reg_user = nullptr;
}  // namespace

std::uint32_t Device::rt_create_blas(const RtGeometry&, std::string* error) {
  if (error) *error = "this device has no ray tracing";
  return 0;
}
std::uint32_t Device::rt_create_tlas(const RtInstance*, std::uint32_t, std::string* error) {
  if (error) *error = "this device has no ray tracing";
  return 0;
}

void set_texture_registrar(TextureRegistrar add, TextureUnregistrar remove, void* user) {
  g_add = add;
  g_remove = remove;
  g_reg_user = user;
}
TextureRegistrar texture_registrar() { return g_add; }
TextureUnregistrar texture_unregistrar() { return g_remove; }
void* texture_registrar_user() { return g_reg_user; }

std::size_t uniform_align() { return g_align; }
void set_uniform_align(std::size_t align) { g_align = std::max<std::size_t>(align, 256); }

Device* active() { return g_active; }
void set_active(Device* device) { g_active = device; }

void set_cli_api(Api api) {
  g_cli = true;
  g_cli_api = api;
}
bool cli_api(Api* out) {
  if (!g_cli) return false;
  if (out) *out = g_cli_api;
  return true;
}

Api parse_api(const std::string& text, bool* ok) {
  std::string s;
  s.reserve(text.size());
  for (char c : text) {
    if (c == ' ' || c == '\t') continue;
    s.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  Api api = Api::OpenGL;
  bool good = true;
  if (s == "gl" || s == "opengl" || s == "opengl3") api = Api::OpenGL;
  else if (s == "vk" || s == "vulkan") api = Api::Vulkan;
  else if (s == "dx12" || s == "d3d12" || s == "dx" || s == "directx" || s == "directx12") api = Api::D3D12;
  else good = false;
  if (ok) *ok = good;
  return api;
}

const char* api_name(Api api) {
  switch (api) {
    case Api::Vulkan: return "Vulkan";
    case Api::D3D12: return "Direct3D 12";
    case Api::OpenGL: return "OpenGL";
  }
  return "OpenGL";
}
const char* api_flag(Api api) {
  switch (api) {
    case Api::Vulkan: return "vk";
    case Api::D3D12: return "dx12";
    case Api::OpenGL: return "gl";
  }
  return "gl";
}
bool api_built(Api api) {
#if !defined(_WIN32)
  if (api == Api::D3D12) return false;
#else
  (void)api;
#endif
  return true;
}

Api resolve_api(const std::string& settings_api) {
  if (g_cli) return g_cli_api;
  bool ok = false;
  Api api = parse_api(settings_api.empty() ? "gl" : settings_api, &ok);
  return ok ? api : Api::OpenGL;
}

std::unique_ptr<Device> create_gl_device(const DeviceDesc&, std::string*);
std::unique_ptr<Device> create_vk_device(const DeviceDesc&, std::string*);
#if defined(_WIN32)
std::unique_ptr<Device> create_dx12_device(const DeviceDesc&, std::string*);
#endif

std::unique_ptr<Device> create_device(const DeviceDesc& desc, std::string* error) {
  if (!api_built(desc.api)) {
    if (error) *error = std::string(api_name(desc.api)) + " is not built on this platform";
    return nullptr;
  }
  switch (desc.api) {
    case Api::OpenGL: return create_gl_device(desc, error);
    case Api::Vulkan: return create_vk_device(desc, error);
    case Api::D3D12:
#if defined(_WIN32)
      return create_dx12_device(desc, error);
#else
      if (error) *error = "Direct3D 12 is Windows-only";
      return nullptr;
#endif
  }
  if (error) *error = "unknown graphics API";
  return nullptr;
}

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts
