#include "rhi/Internal.h"
#include "rhi/ShaderLib.h"

#include <volk.h>
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>
#ifdef None
#undef None
#endif
#ifdef Always
#undef Always
#endif
#ifdef Bool
#undef Bool
#endif

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace rhi {

void vk_check(VkResult r, const char* what, std::string* error) {
  if (r == VK_SUCCESS || !error || !error->empty()) return;
  *error = std::string(what) + " failed (" + std::to_string(static_cast<int>(r)) + ")";
}

struct VkBufferR : Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* map = nullptr;
  BufferKind kind = BufferKind::Vertex;
};
struct VkTextureR : Texture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  int width = 1;
  int height = 1;
  bool depth = false;
  std::uint64_t imgui = 0;
};
struct VkSamplerR : Sampler {
  VkSampler sampler = VK_NULL_HANDLE;
  SamplerDesc desc;
};
struct VkPipelineR : Pipeline {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  bool depth_only = false;
  bool ray = false;
  PipelineDesc desc;
};
struct VkTargetR : RenderTarget {
  VkFramebuffer fb = VK_NULL_HANDLE;
  VkTextureR* color = nullptr;
  VkTextureR* depth = nullptr;
  int width = 1;
  int height = 1;
  bool color_on = true;
};

struct AsObj {
  VkAccelerationStructureKHR as = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceAddress address = 0;
};

class VkDeviceR;

struct Bind {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
  VkImageView view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
};

class VkCmd final : public CommandList {
 public:
  VkDeviceR* dev = nullptr;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  bool pass = false;
  VkPipelineR* pipe = nullptr;
  VkBufferR* vb[2] = {};
  VkBufferR* ib = nullptr;
  Bind ubo[3]{};
  Bind tex[16]{};
  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
  void begin_pass(const PassDesc& p) override;
  void end_pass() override;
  void set_viewport(int x, int y, int w, int h) override;
  void set_pipeline(Pipeline* pipeline) override;
  void set_vertex_buffer(int slot, Buffer* buffer) override;
  void set_index_buffer(Buffer* buffer) override;
  void set_uniform(int slot, Buffer* buffer, std::size_t offset, std::size_t size) override;
  void set_texture(int slot, Texture* texture, Sampler* sampler) override;
  void set_tlas(std::uint32_t id) override;
  void draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t first) override;
  void draw_indexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first) override;
};

class VkDeviceR final : public Device {
 public:
  ~VkDeviceR() override { shutdown(); }
  Api api() const override { return Api::Vulkan; }
  const char* adapter_name() const override { return name_.c_str(); }
  bool raytracing() const override { return raytracing_; }
  bool bottom_up() const override { return false; }
  void clip_params(float o[4]) const override { o[0] = 1; o[1] = 0.5f; o[2] = 0.5f; o[3] = 0; }
  bool shadow_flip_v() const override { return false; }
  void* window() const override { return window_; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  bool open(const DeviceDesc& desc, std::string* error);
  void shutdown();

  Buffer* create_buffer(const BufferDesc& desc, const void* data) override;
  void update_buffer(Buffer* buffer, const void* data, std::size_t size, std::size_t offset) override;
  void destroy_buffer(Buffer* buffer) override;
  Texture* create_texture(const TextureDesc& desc, const void* rgba) override;
  void update_texture(Texture* texture, const void* rgba, int w, int h) override;
  void destroy_texture(Texture* texture) override;
  std::uint64_t imgui_texture(Texture* texture) override;
  Sampler* create_sampler(const SamplerDesc& desc) override;
  void destroy_sampler(Sampler* sampler) override;
  Pipeline* create_pipeline(const PipelineDesc& desc, std::string* error) override;
  void destroy_pipeline(Pipeline* pipeline) override;
  RenderTarget* create_target(const TargetDesc& desc, std::string* error) override;
  void destroy_target(RenderTarget* target) override;
  Texture* target_color(RenderTarget* target) override { return target ? static_cast<VkTargetR*>(target)->color : nullptr; }
  Texture* target_depth(RenderTarget* target) override { return target ? static_cast<VkTargetR*>(target)->depth : nullptr; }
  void begin_frame() override;
  void end_frame() override;
  CommandList* commands() override { return depth_ > 0 ? &cmd_ : nullptr; }
  void wait_idle() override { if (device_) vkDeviceWaitIdle(device_); }
  bool read_color(RenderTarget* target, std::vector<std::uint8_t>* rgba, int* w, int* h) override;
  std::uint32_t rt_create_blas(const RtGeometry& geo, std::string* error) override;
  std::uint32_t rt_create_tlas(const RtInstance* instances, std::uint32_t count, std::string* error) override;
  void rt_reset() override;
  bool vk_info(VkInfo* out) const override;

  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::uint32_t family_ = 0;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkRenderPass color_pass_ = VK_NULL_HANDLE;
  VkRenderPass depth_pass_ = VK_NULL_HANDLE;
  VkRenderPass swap_pass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout rt_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout rt_pipe_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
  VkSampler linear_wrap_ = VK_NULL_HANDLE;
  VkSampler linear_clamp_ = VK_NULL_HANDLE;
  VkTextureR* white_ = nullptr;
  VkTextureR* flat_ = nullptr;
  VkTextureR* far_depth_ = nullptr;
  bool raytracing_ = false;
  std::vector<AsObj> ases_;

  std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;
  VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem, VkDeviceMemory* out_mem, void** map);
  void destroy_buffer_raw(VkBuffer b, VkDeviceMemory m);
  void one_shot(const std::function<void(VkCommandBuffer)>& fn);
  void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, bool depth);
  VkSampler sampler_of(const SamplerDesc& d);
  VkDescriptorSet write_set(VkCmd& c, bool ray);
  VkFramebuffer current_swap_fb() const {
    return image_index_ < swap_fbs_.size() ? swap_fbs_[image_index_] : VK_NULL_HANDLE;
  }
  VkBufferR* dummy_ = nullptr;
  VkFormat color_format_ = VK_FORMAT_R8G8B8A8_UNORM;
  VkFormat swap_format_ = VK_FORMAT_R8G8B8A8_UNORM;
  std::uint32_t image_count_ = 0;
  std::uint32_t image_index_ = 0;
  std::vector<VkImageView> swap_views_;
  std::vector<VkFramebuffer> swap_fbs_;
  VkFence acquire_fence_ = VK_NULL_HANDLE;

 private:
  DeviceDesc desc_{};
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swap_ = VK_NULL_HANDLE;
  VkImage swap_image_ = VK_NULL_HANDLE;
  VkImageView swap_view_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  GLFWwindow* window_ = nullptr;
  bool owned_window_ = false;
  bool owned_glfw_ = false;
  std::string name_ = "Vulkan";
  int width_ = 1;
  int height_ = 1;
  int depth_ = 0;
  bool recording_ = false;
  VkCmd cmd_;
};

void VkCmd::begin_pass(const PassDesc& p) {
  end_pass();
  VkTargetR* t = static_cast<VkTargetR*>(p.target);
  VkRenderPass rp = t ? (t->color_on ? dev->color_pass_ : dev->depth_pass_) : dev->swap_pass_;
  VkFramebuffer fb = t ? t->fb : dev->current_swap_fb();
  int w = t ? t->width : dev->width();
  int h = t ? t->height : dev->height();
  VkClearValue clears[2]{};
  clears[0].color.float32[0] = p.rgba[0];
  clears[0].color.float32[1] = p.rgba[1];
  clears[0].color.float32[2] = p.rgba[2];
  clears[0].color.float32[3] = p.rgba[3];
  clears[1].depthStencil.depth = 1.0f;
  VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  info.renderPass = rp;
  info.framebuffer = fb;
  info.renderArea.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
  info.clearValueCount = t && !t->color_on ? 1 : 2;
  info.pClearValues = t && !t->color_on ? &clears[1] : clears;
  if (!p.clear_color && !p.clear_depth) {
    // Still begin; load ops are CLEAR in our passes. Callers clear.
  }
  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  pass = true;
  set_viewport(0, 0, w, h);
}
void VkCmd::end_pass() {
  if (!pass) return;
  vkCmdEndRenderPass(cmd);
  pass = false;
}
void VkCmd::set_viewport(int x, int y, int w, int h) {
  VkViewport vp{};
  vp.x = static_cast<float>(x);
  vp.y = static_cast<float>(y + h);
  vp.width = static_cast<float>(w);
  vp.height = -static_cast<float>(h);
  vp.minDepth = 0;
  vp.maxDepth = 1;
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D sc{{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
  vkCmdSetScissor(cmd, 0, 1, &sc);
}
void VkCmd::set_pipeline(Pipeline* pipeline) {
  pipe = static_cast<VkPipelineR*>(pipeline);
  if (pipe && pass) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
}
void VkCmd::set_vertex_buffer(int slot, Buffer* buffer) {
  if (slot >= 0 && slot < 2) vb[slot] = static_cast<VkBufferR*>(buffer);
}
void VkCmd::set_index_buffer(Buffer* buffer) { ib = static_cast<VkBufferR*>(buffer); }
void VkCmd::set_uniform(int slot, Buffer* buffer, std::size_t offset, std::size_t size) {
  if (slot < 0 || slot > 2) return;
  auto* b = static_cast<VkBufferR*>(buffer);
  ubo[slot].buffer = b ? b->buffer : VK_NULL_HANDLE;
  ubo[slot].offset = offset;
  ubo[slot].size = size;
}
void VkCmd::set_texture(int slot, Texture* texture, Sampler* sampler) {
  if (slot < 0 || slot > 15) return;
  auto* t = static_cast<VkTextureR*>(texture);
  auto* s = static_cast<VkSamplerR*>(sampler);
  tex[slot].view = t ? t->view : VK_NULL_HANDLE;
  tex[slot].sampler = s ? s->sampler : VK_NULL_HANDLE;
}
void VkCmd::set_tlas(std::uint32_t id) {
  tlas = (id > 0 && id <= dev->ases_.size()) ? dev->ases_[id - 1].as : VK_NULL_HANDLE;
}
void VkCmd::draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t first) {
  if (!pipe || !pass) return;
  VkDescriptorSet set = dev->write_set(*this, pipe->ray);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->ray ? dev->rt_pipe_layout_ : dev->pipe_layout_, 0, 1,
                          &set, 0, nullptr);
  VkBuffer buffers[2] = {};
  VkDeviceSize offsets[2] = {};
  int n = 0;
  if (vb[0]) { buffers[n] = vb[0]->buffer; ++n; }
  if (n && vb[1]) { buffers[n] = vb[1]->buffer; ++n; }
  if (n) vkCmdBindVertexBuffers(cmd, 0, static_cast<uint32_t>(n), buffers, offsets);
  if (ib) {
    vkCmdBindIndexBuffer(cmd, ib->buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, vertices, instances ? instances : 1, first, 0, 0);
  } else {
    vkCmdDraw(cmd, vertices, instances ? instances : 1, first, 0);
  }
}
void VkCmd::draw_indexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first) {
  draw(indices, instances, first);
}

// Device method bodies follow in VkDeviceMethods.cpp via inclusion so this
// translation unit stays the only one that sees VkDeviceR.
#include "rhi/VkDeviceMethods.inc"

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts
