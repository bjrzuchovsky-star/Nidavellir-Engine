#pragma once

// Render hardware interface. One device, one frame's command list, and the
// resources the forward renderer needs. OpenGL 3.3, Vulkan and (on Windows)
// Direct3D 12 implement it. The classic GlRenderer3D still draws on the GL
// device's context when a scene asks for the classic lighting preset, so
// existing scenes stay on the path they already have.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace rhi {

enum class Api { OpenGL, Vulkan, D3D12 };

enum class BufferKind { Vertex, Index, Uniform, Readback };
enum class TextureFormat { RGBA8, Depth };
enum class Filter { Nearest, Linear };
enum class Wrap { Repeat, Clamp };
enum class Topology { Triangles, Lines };
enum class Cull { None, Back };
enum class Blend { Off, Alpha, Additive };
enum class Compare { Less, Always };

struct DeviceDesc {
  Api api = Api::OpenGL;
  void* window = nullptr;  // GLFWwindow*. Null: the device creates one.
  bool offscreen = false;
  bool visible = true;
  int width = 1280;
  int height = 720;
  const char* title = "Nidavellir";
};

struct BufferDesc {
  BufferKind kind = BufferKind::Vertex;
  std::size_t size = 0;
};

struct TextureDesc {
  int width = 1;
  int height = 1;
  TextureFormat format = TextureFormat::RGBA8;
  bool sampled = true;
  bool render = false;
  bool mips = false;
};

struct SamplerDesc {
  Filter filter = Filter::Linear;
  Wrap wrap = Wrap::Repeat;
  bool mips = false;
};

struct VertexAttr {
  std::uint32_t location = 0;
  std::uint32_t buffer = 0;
  std::uint32_t offset = 0;
  std::uint32_t components = 3;
  bool instance = false;
};

struct PipelineDesc {
  const char* vs = nullptr;  // baked shader name, e.g. "mesh.vert"
  const char* fs = nullptr;
  VertexAttr attrs[8]{};
  int attr_count = 0;
  std::uint32_t strides[2] = {0, 0};
  Topology topology = Topology::Triangles;
  Cull cull = Cull::Back;
  Blend blend = Blend::Off;
  bool depth_test = true;
  bool depth_write = true;
  Compare depth_cmp = Compare::Less;
  bool polygon_offset = false;
  bool ray_query = false;
};

struct TargetDesc {
  int width = 1;
  int height = 1;
  bool color = true;
  bool depth = true;
  bool sample_depth = true;
};

struct PassDesc {
  class RenderTarget* target = nullptr;  // null draws into the swapchain
  bool clear_color = true;
  bool clear_depth = true;
  bool color = true;
  float rgba[4] = {0, 0, 0, 1};
};

class Resource {
 public:
  virtual ~Resource() = default;
};
class Buffer : public Resource {};
class Texture : public Resource {};
class Sampler : public Resource {};
class Pipeline : public Resource {};
class RenderTarget : public Resource {};

struct RtGeometry {
  Buffer* vertices = nullptr;
  std::uint32_t stride = 12;
  std::uint32_t vertex_count = 0;
  Buffer* indices = nullptr;
  std::uint32_t index_count = 0;
};
struct RtInstance {
  float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // row-major 3x4
  std::uint32_t blas = 0;
};

class CommandList {
 public:
  virtual ~CommandList() = default;
  virtual void begin_pass(const PassDesc& pass) = 0;
  virtual void end_pass() = 0;
  virtual void set_viewport(int x, int y, int w, int h) = 0;
  virtual void set_pipeline(Pipeline* pipeline) = 0;
  virtual void set_vertex_buffer(int slot, Buffer* buffer) = 0;
  virtual void set_index_buffer(Buffer* buffer) = 0;
  virtual void set_uniform(int slot, Buffer* buffer, std::size_t offset, std::size_t size) = 0;
  virtual void set_texture(int slot, Texture* texture, Sampler* sampler) = 0;
  virtual void set_tlas(std::uint32_t tlas) = 0;
  virtual void draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t first) = 0;
  virtual void draw_indexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first) = 0;
};

// Turns a backend texture into the integer ImGui::Image wants. Vulkan needs
// Dear ImGui to allocate the descriptor set, so the editor installs this.
using TextureRegistrar = std::uint64_t (*)(Api api, void* image_view, void* user);
using TextureUnregistrar = void (*)(Api api, std::uint64_t id, void* user);
void set_texture_registrar(TextureRegistrar add, TextureUnregistrar remove, void* user);

class Device {
 public:
  virtual ~Device() = default;
  virtual Api api() const = 0;
  virtual const char* adapter_name() const = 0;
  virtual bool raytracing() const = 0;
  // Row 0 of a color target is the bottom of the image (OpenGL's read-back).
  virtual bool bottom_up() const = 0;
  // clip.xyzw uploaded to shaders: y scale, z scale, z bias, hdri flag.
  virtual void clip_params(float out4[4]) const = 0;
  virtual bool shadow_flip_v() const = 0;
  virtual void* window() const = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;

  virtual Buffer* create_buffer(const BufferDesc& desc, const void* data) = 0;
  virtual void update_buffer(Buffer* buffer, const void* data, std::size_t size, std::size_t offset) = 0;
  virtual void destroy_buffer(Buffer* buffer) = 0;
  virtual Texture* create_texture(const TextureDesc& desc, const void* rgba) = 0;
  virtual void update_texture(Texture* texture, const void* rgba, int w, int h) = 0;
  virtual void destroy_texture(Texture* texture) = 0;
  virtual std::uint64_t imgui_texture(Texture* texture) = 0;
  virtual Sampler* create_sampler(const SamplerDesc& desc) = 0;
  virtual void destroy_sampler(Sampler* sampler) = 0;
  virtual Pipeline* create_pipeline(const PipelineDesc& desc, std::string* error) = 0;
  virtual void destroy_pipeline(Pipeline* pipeline) = 0;
  virtual RenderTarget* create_target(const TargetDesc& desc, std::string* error) = 0;
  virtual void destroy_target(RenderTarget* target) = 0;
  virtual Texture* target_color(RenderTarget* target) = 0;
  virtual Texture* target_depth(RenderTarget* target) = 0;

  virtual void begin_frame() = 0;
  virtual void end_frame() = 0;
  virtual CommandList* commands() = 0;
  virtual void wait_idle() = 0;
  virtual bool read_color(RenderTarget* target, std::vector<std::uint8_t>* rgba, int* w, int* h) = 0;

  virtual std::uint32_t rt_create_blas(const RtGeometry& geo, std::string* error);
  virtual std::uint32_t rt_create_tlas(const RtInstance* instances, std::uint32_t count, std::string* error);
  virtual void rt_reset() {}

  // Handles the editor's ImGui glue reads. Pointers are backend objects.
  struct VkInfo {
    void* instance = nullptr;
    void* physical = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    std::uint32_t family = 0;
    void* render_pass = nullptr;
    void* command = nullptr;
    void* framebuffer = nullptr;
    std::uint32_t images = 0;
    std::uint32_t api_version = 0;
    int width = 0;
    int height = 0;
  };
  struct DxInfo {
    void* device = nullptr;
    void* queue = nullptr;
    void* list = nullptr;
    void* srv_heap = nullptr;
    void* rtv_cpu = nullptr;  // pointer to a CPU handle stored by the device
    int frames = 2;
    int width = 0;
    int height = 0;
    int format = 0;  // DXGI_FORMAT
  };
  virtual bool vk_info(VkInfo* out) const { (void)out; return false; }
  virtual bool dx_info(DxInfo* out) const { (void)out; return false; }
};

std::unique_ptr<Device> create_device(const DeviceDesc& desc, std::string* error);

Api parse_api(const std::string& text, bool* ok = nullptr);
const char* api_name(Api api);
const char* api_flag(Api api);  // "gl", "vk", "dx12"
bool api_built(Api api);

// --gfx= wins over the editor setting for this process.
void set_cli_api(Api api);
bool cli_api(Api* out);
Api resolve_api(const std::string& settings_api);

Device* active();
void set_active(Device* device);

// Uniform-buffer alignment the renderer should use for sub-allocations.
std::size_t uniform_align();

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts
