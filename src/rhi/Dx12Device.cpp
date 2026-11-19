#include "rhi/Rhi.h"

#if !defined(_WIN32)

namespace ts {
namespace tombstone {
namespace rhi {
// Direct3D 12 is built on Windows only. create_dx12_device is not linked here.
}
}  // namespace tombstone
}  // namespace ts

#else

#include "rhi/Internal.h"
#include "rhi/ShaderLib.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace rhi {

template <class T>
struct Com {
  T* p = nullptr;
  ~Com() { reset(); }
  void reset() { if (p) { p->Release(); p = nullptr; } }
  T* operator->() const { return p; }
  T** out() { reset(); return &p; }
  operator T*() const { return p; }
};

struct DxBuffer : Buffer {
  ID3D12Resource* res = nullptr;
  BufferKind kind = BufferKind::Vertex;
  std::size_t size = 0;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  void* map = nullptr;
};
struct DxTexture : Texture {
  ID3D12Resource* res = nullptr;
  int width = 1;
  int height = 1;
  bool depth = false;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
  bool has_srv = false;
  std::uint64_t imgui = 0;
};
struct DxSampler : Sampler { SamplerDesc desc; };
struct DxPipeline : Pipeline {
  ID3D12PipelineState* pso = nullptr;
  bool depth_only = false;
  bool ray = false;
  std::uint32_t stride0 = 0;
  std::uint32_t stride1 = 0;
  bool lines = false;
};
struct DxTarget : RenderTarget {
  DxTexture* color = nullptr;
  DxTexture* depth = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  int width = 1;
  int height = 1;
  bool color_on = true;
};

class DxDevice;

class DxCmd final : public CommandList {
 public:
  DxDevice* dev = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  DxPipeline* pipe = nullptr;
  DxBuffer* vb[2] = {};
  DxBuffer* ib = nullptr;
  DxBuffer* ubo[3] = {};
  std::size_t ubo_off[3] = {};
  DxTexture* tex[16] = {};
  bool pass = false;
  void begin_pass(const PassDesc& p) override;
  void end_pass() override;
  void set_viewport(int x, int y, int w, int h) override;
  void set_pipeline(Pipeline* pipeline) override;
  void set_vertex_buffer(int slot, Buffer* buffer) override;
  void set_index_buffer(Buffer* buffer) override;
  void set_uniform(int slot, Buffer* buffer, std::size_t offset, std::size_t size) override;
  void set_texture(int slot, Texture* texture, Sampler*) override;
  void set_tlas(std::uint32_t) override;
  void draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t first) override;
  void draw_indexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first) override;
  DxTarget* bound = nullptr;
};

class DxDevice final : public Device {
 public:
  ~DxDevice() override { shutdown(); }
  Api api() const override { return Api::D3D12; }
  const char* adapter_name() const override { return name_.c_str(); }
  bool raytracing() const override { return raytracing_; }
  bool bottom_up() const override { return false; }
  void clip_params(float o[4]) const override { o[0] = 1; o[1] = 0.5f; o[2] = 0.5f; o[3] = 0; }
  bool shadow_flip_v() const override { return true; }
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
  Texture* target_color(RenderTarget* target) override { return target ? static_cast<DxTarget*>(target)->color : nullptr; }
  Texture* target_depth(RenderTarget* target) override { return target ? static_cast<DxTarget*>(target)->depth : nullptr; }
  void begin_frame() override;
  void end_frame() override;
  CommandList* commands() override { return depth_ > 0 ? &cmd_ : nullptr; }
  void wait_idle() override { wait_fence(fence_value_); }
  bool read_color(RenderTarget* target, std::vector<std::uint8_t>* rgba, int* w, int* h) override;
  bool read_depth(RenderTarget* target, std::vector<float>* depth, int* w, int* h) override;
  std::uint32_t rt_create_blas(const RtGeometry& geo, std::string* error) override;
  std::uint32_t rt_create_tlas(const RtInstance* instances, std::uint32_t count, std::string* error) override;
  void rt_reset() override;
  bool dx_info(DxInfo* out) const override;
  int debug_error_count() const override;
  bool read_swapchain(std::vector<std::uint8_t>* rgba, int* w, int* h) override;
  void blit_swapchain(Texture* color) override;
  bool has_swapchain() const { return static_cast<bool>(swap_); }
  void bind_swapchain(const PassDesc& pass);
  void create_backbuffers();
  void resize_swapchain(int w, int h);
  void deliver_swapchain_read();

  ID3D12Device* device() const { return removed_ ? nullptr : device_; }
  bool device_lost() const { return removed_; }
  ID3D12GraphicsCommandList* list() const { return list_; }
  ID3D12RootSignature* root(bool) const { return root_; }
  void barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
  D3D12_CPU_DESCRIPTOR_HANDLE alloc_cpu();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_of(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;
  ID3D12DescriptorHeap* srv_heap() const { return srv_; }
  D3D12_CPU_DESCRIPTOR_HANDLE white_cpu() const { return white_ ? white_->cpu : D3D12_CPU_DESCRIPTOR_HANDLE{}; }
  DxTexture* white_ = nullptr;

  ID3D12Device* device_ = nullptr;
  // list_ is whichever list barrier() records into. It is the frame list
  // except during an immediate copy, which has its own list.
  ID3D12GraphicsCommandList* list_ = nullptr;
  bool raytracing_ = false;

 private:
  void wait_fence(UINT64 value);
  UINT64 signal_fence();
  void collect();
  void retire(IUnknown* obj);
  bool begin_copy();
  void end_copy();
  void explain(std::string* error, const std::string& head, HRESULT hr);
  void note_removed(const char* where, HRESULT hr);
  bool failed(HRESULT hr, const char* where);
  D3D12_CPU_DESCRIPTOR_HANDLE alloc_visible();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_visible(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;
  DeviceDesc desc_{};
  Com<IDXGIFactory6> factory_;
  Com<ID3D12CommandQueue> queue_;
  Com<IDXGISwapChain3> swap_;
  // One allocator per frame. Reset only after the fence that covered its
  // last ExecuteCommandLists has completed.
  Com<ID3D12CommandAllocator> frame_alloc_[2];
  UINT64 frame_fence_[2] = {};
  int frame_slot_ = 0;
  ID3D12GraphicsCommandList* frame_list_ = nullptr;
  bool list_open_ = false;
  // Immediate uploads and readbacks. Never the frame list: resetting that
  // while Project Manager -> Editor3D is recording closes it and the rest
  // of the frame (hundreds of thousands of draws) hits a closed list.
  Com<ID3D12CommandAllocator> copy_alloc_;
  ID3D12GraphicsCommandList* copy_list_ = nullptr;
  UINT64 copy_fence_ = 0;
  Com<ID3D12Fence> fence_;
  struct Grave {
    UINT64 fence = 0;
    IUnknown* obj = nullptr;
  };
  std::vector<Grave> grave_;
  Com<ID3D12RootSignature> root_;
  Com<ID3D12RootSignature> root_rt_;
  Com<ID3D12DescriptorHeap> rtv_heap_;
  Com<ID3D12DescriptorHeap> dsv_heap_;
  Com<ID3D12DescriptorHeap> srv_;       // shader-visible: ImGui + per-draw table
  Com<ID3D12DescriptorHeap> srv_cpu_;   // not shader-visible: copy sources
  UINT srv_stride_ = 0;
  UINT rtv_stride_ = 0;
  UINT dsv_stride_ = 0;
  UINT srv_used_ = 64;       // shader-visible, 0..63 reserved for Dear ImGui
  UINT srv_cpu_used_ = 0;    // staging heap
  bool removed_ = false;
  mutable int debug_errors_ = 0;
  UINT rtv_used_ = 0;
  UINT dsv_used_ = 0;
  ID3D12Resource* back_[2] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE back_rtv_[2] = {};
  D3D12_RESOURCE_STATES back_state_[2] = {D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT};
  UINT frame_index_ = 0;
  bool swap_drawn_ = false;
  ID3D12Resource* swap_read_ = nullptr;
  std::vector<std::uint8_t>* swap_read_dst_ = nullptr;
  int* swap_read_w_ = nullptr;
  int* swap_read_h_ = nullptr;
  HANDLE fence_event_ = nullptr;
  UINT64 fence_value_ = 0;
  GLFWwindow* window_ = nullptr;
  bool owned_window_ = false;
  bool owned_glfw_ = false;
  std::string name_ = "Direct3D 12";
  int width_ = 1;
  int height_ = 1;
  int depth_ = 0;
  DxCmd cmd_;
  std::vector<ID3D12Resource*> rt_keep_;
};

void DxDevice::barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  if (!res || from == to || !list_) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  list_->ResourceBarrier(1, &b);
}
D3D12_CPU_DESCRIPTOR_HANDLE DxDevice::alloc_cpu() {
  // CopyDescriptorsSimple refuses a shader-visible source (debug error 654).
  D3D12_CPU_DESCRIPTOR_HANDLE h = srv_cpu_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += static_cast<SIZE_T>(srv_cpu_used_++) * srv_stride_;
  return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE DxDevice::alloc_visible() {
  if (srv_used_ >= 500) srv_used_ = 64;
  D3D12_CPU_DESCRIPTOR_HANDLE h = srv_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += static_cast<SIZE_T>(srv_used_++) * srv_stride_;
  return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE DxDevice::gpu_of(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const {
  return gpu_visible(cpu);
}
D3D12_GPU_DESCRIPTOR_HANDLE DxDevice::gpu_visible(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const {
  D3D12_GPU_DESCRIPTOR_HANDLE g = srv_->GetGPUDescriptorHandleForHeapStart();
  g.ptr += cpu.ptr - srv_->GetCPUDescriptorHandleForHeapStart().ptr;
  return g;
}

void DxCmd::begin_pass(const PassDesc& p) {
  end_pass();
  auto* t = static_cast<DxTarget*>(p.target);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  int w = dev->width();
  int h = dev->height();
  if (t) {
    w = t->width;
    h = t->height;
    if (t->color) {
      dev->barrier(t->color->res, t->color->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      t->color->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
      if (p.clear_color) {
        const float c[4] = {p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]};
        list->ClearRenderTargetView(t->rtv, c, 0, nullptr);
      }
    }
    if (t->depth) {
      dev->barrier(t->depth->res, t->depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      t->depth->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
      if (p.clear_depth) list->ClearDepthStencilView(t->dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
    rtv = t->color_on ? t->rtv : D3D12_CPU_DESCRIPTOR_HANDLE{};
    dsv = t->depth ? t->dsv : D3D12_CPU_DESCRIPTOR_HANDLE{};
    list->OMSetRenderTargets(t->color_on ? 1 : 0, t->color_on ? &rtv : nullptr, FALSE, t->depth ? &dsv : nullptr);
  } else if (dev->has_swapchain()) {
    dev->bind_swapchain(p);
  }
  bound = t;
  pass = true;
  set_viewport(0, 0, w, h);
  ID3D12DescriptorHeap* heaps[] = {dev->srv_heap()};
  list->SetDescriptorHeaps(1, heaps);
}
void DxCmd::end_pass() {
  if (!pass) return;
  // The shadow map (and any later sample of this target) is an SRV. Leave
  // the resources in shader-read so the lighting pass can sample the depth.
  if (bound && bound->depth && bound->depth->res && bound->depth->state == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
    dev->barrier(bound->depth->res, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound->depth->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  }
  if (bound && bound->color && bound->color->res && bound->color->state == D3D12_RESOURCE_STATE_RENDER_TARGET) {
    dev->barrier(bound->color->res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bound->color->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  }
  bound = nullptr;
  pass = false;
}
void DxCmd::set_viewport(int x, int y, int w, int h) {
  D3D12_VIEWPORT vp{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h), 0, 1};
  D3D12_RECT sc{x, y, x + w, y + h};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &sc);
}
void DxCmd::set_pipeline(Pipeline* pipeline) {
  pipe = static_cast<DxPipeline*>(pipeline);
  if (!pipe) return;
  list->SetGraphicsRootSignature(dev->root(pipe->ray));
  list->SetPipelineState(pipe->pso);
}
void DxCmd::set_vertex_buffer(int slot, Buffer* buffer) {
  if (slot >= 0 && slot < 2) vb[slot] = static_cast<DxBuffer*>(buffer);
}
void DxCmd::set_index_buffer(Buffer* buffer) { ib = static_cast<DxBuffer*>(buffer); }
void DxCmd::set_uniform(int slot, Buffer* buffer, std::size_t offset, std::size_t) {
  if (slot < 0 || slot > 2) return;
  ubo[slot] = static_cast<DxBuffer*>(buffer);
  ubo_off[slot] = offset;
}
void DxCmd::set_texture(int slot, Texture* texture, Sampler*) {
  if (slot >= 0 && slot < 16) tex[slot] = static_cast<DxTexture*>(texture);
}
void DxCmd::set_tlas(std::uint32_t) {}
void DxCmd::draw(std::uint32_t count, std::uint32_t instances, std::uint32_t first) {
  if (!pipe || !pass || !dev->device()) return;
  for (int i = 0; i < 3; ++i) {
    if (!ubo[i]) continue;
    list->SetGraphicsRootConstantBufferView(static_cast<UINT>(i), ubo[i]->gpu + ubo_off[i]);
  }
  if (dev->srv_heap()) {
    D3D12_CPU_DESCRIPTOR_HANDLE dst = dev->srv_heap()->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = dev->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dst.ptr += static_cast<SIZE_T>(512) * stride;
    D3D12_CPU_DESCRIPTOR_HANDLE base = dst;
    for (int i = 0; i < 13; ++i) {
      DxTexture* t = tex[i + 3];
      D3D12_CPU_DESCRIPTOR_HANDLE src = (t && t->has_srv) ? t->cpu : dev->white_cpu();
      dev->device()->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      dst.ptr += stride;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = dev->srv_heap()->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<SIZE_T>(512) * stride;
    (void)base;
    list->SetGraphicsRootDescriptorTable(3, gpu);
  }
  D3D12_VERTEX_BUFFER_VIEW views[2]{};
  int n = 0;
  if (vb[0]) {
    views[n].BufferLocation = vb[0]->gpu;
    views[n].SizeInBytes = static_cast<UINT>(vb[0]->size);
    views[n].StrideInBytes = pipe ? 0 : 0;
    ++n;
  }
  (void)n;
  list->IASetPrimitiveTopology(pipe->lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (vb[0] && pipe->stride0) {
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = vb[0]->gpu;
    v.SizeInBytes = static_cast<UINT>(vb[0]->size);
    v.StrideInBytes = pipe->stride0;
    list->IASetVertexBuffers(0, 1, &v);
  }
  if (vb[1] && pipe->stride1) {
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = vb[1]->gpu;
    v.SizeInBytes = static_cast<UINT>(vb[1]->size);
    v.StrideInBytes = pipe->stride1;
    list->IASetVertexBuffers(1, 1, &v);
  }
  if (ib) {
    D3D12_INDEX_BUFFER_VIEW iv{};
    iv.BufferLocation = ib->gpu;
    iv.SizeInBytes = static_cast<UINT>(ib->size);
    iv.Format = DXGI_FORMAT_R32_UINT;
    list->IASetIndexBuffer(&iv);
    list->DrawIndexedInstanced(count, instances ? instances : 1, first, 0, 0);
  } else {
    list->DrawInstanced(count, instances ? instances : 1, first, 0);
  }
}
void DxCmd::draw_indexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first) {
  draw(indices, instances, first);
}

void DxDevice::explain(std::string* error, const std::string& head, HRESULT hr) {
  if (!error) return;
  char hex[16];
  std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(hr));
  *error = head + " (" + hex + ")";
  if (!device_) return;
  Com<ID3D12InfoQueue> queue;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(queue.out())))) return;
  const UINT64 count = queue->GetNumStoredMessages();
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T bytes = 0;
    if (FAILED(queue->GetMessage(i, nullptr, &bytes)) || bytes == 0) continue;
    std::vector<unsigned char> mem(bytes);
    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(mem.data());
    if (FAILED(queue->GetMessage(i, msg, &bytes)) || !msg->pDescription) continue;
    if (gfx_debug()) std::fprintf(stderr, "D3D12: %s\n", msg->pDescription);
    *error += "\nD3D12: ";
    *error += msg->pDescription;
  }
  queue->ClearStoredMessages();
}

void DxDevice::note_removed(const char* where, HRESULT hr) {
  if (removed_) return;
  removed_ = true;
  const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
  std::fprintf(stderr, "D3D12 device removed during %s (0x%08X, reason 0x%08X)\n", where ? where : "a call",
               static_cast<unsigned>(hr), static_cast<unsigned>(reason));
  Com<ID3D12DeviceRemovedExtendedData> dred;
  if (!device_ || FAILED(device_->QueryInterface(IID_PPV_ARGS(dred.out())))) return;
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
  if (FAILED(dred->GetAutoBreadcrumbsOutput(&crumbs)) || !crumbs.pHeadAutoBreadcrumbNode) return;
  const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode;
  const D3D12_AUTO_BREADCRUMB_NODE* last = node;
  while (node) {
    last = node;
    node = node->pNext;
  }
  if (last && last->pCommandListDebugNameA) {
    std::fprintf(stderr, "D3D12 DRED: last breadcrumb list \"%s\" (%u ops)\n", last->pCommandListDebugNameA,
                 last->BreadcrumbCount);
  } else if (last) {
    std::fprintf(stderr, "D3D12 DRED: last breadcrumb list has %u ops\n", last->BreadcrumbCount);
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
    std::fprintf(stderr, "D3D12 DRED: page fault at 0x%llX\n", static_cast<unsigned long long>(fault.PageFaultVA));
  }
}

bool DxDevice::failed(HRESULT hr, const char* where) {
  if (!FAILED(hr)) return false;
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET) {
    note_removed(where, hr);
  } else {
    std::fprintf(stderr, "D3D12 %s failed (0x%08X)\n", where ? where : "call", static_cast<unsigned>(hr));
    ++debug_errors_;
  }
  return true;
}

namespace {

std::string adapter_text(const DXGI_ADAPTER_DESC1& ad) {
  char name[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
  return name;
}

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool software_or_virtual(IDXGIAdapter1* adapter, const DXGI_ADAPTER_DESC1& ad) {
  if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return true;
  Com<IDXGIAdapter4> newer;
  if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(newer.out())))) {
    DXGI_ADAPTER_DESC3 d3{};
    if (SUCCEEDED(newer->GetDesc3(&d3)) &&
        (d3.Flags & (DXGI_ADAPTER_FLAG3_SOFTWARE | DXGI_ADAPTER_FLAG3_REMOTE))) {
      return true;
    }
  }
  const std::string n = lower_copy(adapter_text(ad));
  const char* banned[] = {"microsoft basic", "basic render", "remote", "virtual", "indirect", "parsec", "idd sample"};
  for (const char* word : banned) {
    if (n.find(word) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

bool DxDevice::open(const DeviceDesc& desc, std::string* error) {
  desc_ = desc;
  set_uniform_align(256);
  UINT factory_flags = 0;
  if (gfx_debug()) {
    Com<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dbg.out())))) {
      dbg->EnableDebugLayer();
      factory_flags = DXGI_CREATE_FACTORY_DEBUG;
    }
  }
  // Breadcrumbs have to be turned on before the device exists, or a later
  // removal has nothing to report.
  Com<ID3D12DeviceRemovedExtendedDataSettings> dred_settings;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dred_settings.out())))) {
    dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  }
  if (CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(factory_.out())) != S_OK &&
      (factory_flags == 0 || CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.out())) != S_OK)) {
    if (error) *error = "CreateDXGIFactory2 failed";
    return false;
  }
  const bool warp_only = gfx_warp();
  if (!warp_only) {
    // HIGH_PERFORMANCE lists the discrete GPU first. Dedicated video memory
    // breaks a tie if a virtual display or the iGPU is enumerated ahead of it.
    Com<IDXGIAdapter1> best;
    SIZE_T best_mem = 0;
    bool have = false;
    std::string best_name;
    Com<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory_->EnumAdapterByGpuPreference(
                         i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.out())) == S_OK;
         ++i) {
      DXGI_ADAPTER_DESC1 ad{};
      adapter->GetDesc1(&ad);
      if (software_or_virtual(adapter, ad)) continue;
      Com<ID3D12Device> trial;
      if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(trial.out())))) continue;
      if (have && ad.DedicatedVideoMemory <= best_mem) continue;
      best.reset();
      adapter->QueryInterface(IID_PPV_ARGS(best.out()));
      best_mem = ad.DedicatedVideoMemory;
      best_name = adapter_text(ad);
      have = true;
    }
    if (have && FAILED(D3D12CreateDevice(best, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
      if (device_) { device_->Release(); device_ = nullptr; }
    }
    if (device_) name_ = best_name;
  }
  if (!device_) {
    Com<IDXGIAdapter> warp;
    factory_->EnumWarpAdapter(IID_PPV_ARGS(warp.out()));
    HRESULT hr = warp ? D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)) : E_FAIL;
    if (FAILED(hr)) {
      explain(error, warp_only ? "D3D12 WARP device failed" : "D3D12CreateDevice failed", hr);
      return false;
    }
    name_ = "D3D12 WARP";
  }
  if (gfx_debug()) {
    Com<ID3D12InfoQueue> queue;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(queue.out())))) {
      queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
      queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
      queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
    }
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
  if (device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)) == S_OK) {
    raytracing_ = opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  device_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.out()));
  for (int i = 0; i < 2; ++i) {
    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(frame_alloc_[i].out()));
  }
  device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame_alloc_[0], nullptr, IID_PPV_ARGS(&frame_list_));
  frame_list_->Close();
  list_ = frame_list_;
  device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(copy_alloc_.out()));
  device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy_alloc_, nullptr, IID_PPV_ARGS(&copy_list_));
  copy_list_->Close();
  device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.out()));
  fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 13;
  range.BaseShaderRegister = 3;
  D3D12_ROOT_PARAMETER params[4]{};
  for (int i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable.NumDescriptorRanges = 1;
  params[3].DescriptorTable.pDescriptorRanges = &range;
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC samps[12]{};
  for (int i = 0; i < 12; ++i) {
    samps[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    const bool shadow = i == 3;  // register s6, the shadow map
    samps[i].AddressU = samps[i].AddressV = samps[i].AddressW =
        shadow ? D3D12_TEXTURE_ADDRESS_MODE_BORDER : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samps[i].ShaderRegister = static_cast<UINT>(3 + i);
    samps[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samps[i].MaxLOD = 16;
    // A zero ComparisonFunc is not NEVER (that enum starts at 1). The runtime
    // rejects the root signature. The shadow sample is a manual compare of
    // R32_FLOAT, so this stays a normal sampler. Out-of-range depth is 1
    // (far, unshadowed) rather than 0, which would shadow the whole frame.
    samps[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samps[i].MaxAnisotropy = 1;
    samps[i].BorderColor = shadow ? D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE
                                  : D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  }
  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 4;
  rsd.pParameters = params;
  rsd.NumStaticSamplers = 12;
  rsd.pStaticSamplers = samps;
  rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Com<ID3DBlob> blob, errb;
  HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, blob.out(), errb.out());
  if (FAILED(hr) || !blob) {
    std::string head = "D3D12SerializeRootSignature failed";
    if (errb && errb->GetBufferPointer()) head += std::string(": ") + static_cast<const char*>(errb->GetBufferPointer());
    explain(error, head, hr);
    return false;
  }
  hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root_.out()));
  if (FAILED(hr) || !root_) {
    explain(error, "CreateRootSignature failed", hr);
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 64;
  device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtv_heap_.out()));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsv_heap_.out()));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 2048;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(srv_cpu_.out()));
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(srv_.out()));
  if (!srv_cpu_ || !srv_) {
    if (error) *error = "D3D12 descriptor heap failed";
    return false;
  }
  srv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

  if (!desc.offscreen) {
    if (!desc.window) {
      if (!glfwInit()) {
        if (error) *error = "glfwInit failed";
        return false;
      }
      owned_glfw_ = true;
      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
      window_ = glfwCreateWindow(desc.width, desc.height, desc.title ? desc.title : "Nidavellir", nullptr, nullptr);
      owned_window_ = true;
    } else window_ = static_cast<GLFWwindow*>(desc.window);
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = static_cast<UINT>(desc.width);
    sc.Height = static_cast<UINT>(desc.height);
    sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Com<IDXGISwapChain1> swap1;
    if (factory_->CreateSwapChainForHwnd(queue_, glfwGetWin32Window(window_), &sc, nullptr, nullptr, swap1.out()) != S_OK) {
      if (error) *error = "CreateSwapChainForHwnd failed";
      return false;
    }
    swap1->QueryInterface(IID_PPV_ARGS(swap_.out()));
    width_ = desc.width;
    height_ = desc.height;
    create_backbuffers();
  } else {
    width_ = desc.width;
    height_ = desc.height;
  }
  cmd_.dev = this;
  cmd_.list = list_;
  TextureDesc white_desc;
  white_desc.width = white_desc.height = 1;
  const unsigned char white_px[4] = {255, 255, 255, 255};
  white_ = static_cast<DxTexture*>(create_texture(white_desc, white_px));
  return true;
}

void DxDevice::shutdown() {
  if (list_open_ && frame_list_) {
    frame_list_->Close();
    list_open_ = false;
  }
  wait_fence(fence_value_);
  for (Grave& g : grave_) {
    if (g.obj) g.obj->Release();
    g.obj = nullptr;
  }
  grave_.clear();
  for (ID3D12Resource*& b : back_) {
    if (b) { b->Release(); b = nullptr; }
  }
  if (swap_read_) { swap_read_->Release(); swap_read_ = nullptr; }
  rt_reset();
  if (frame_list_) { frame_list_->Release(); frame_list_ = nullptr; }
  if (copy_list_) { copy_list_->Release(); copy_list_ = nullptr; }
  list_ = nullptr;
  if (fence_event_) CloseHandle(fence_event_);
  if (owned_window_ && window_) glfwDestroyWindow(window_);
  if (owned_glfw_) glfwTerminate();
  if (device_) { device_->Release(); device_ = nullptr; }
}

void DxDevice::wait_fence(UINT64 value) {
  if (!fence_ || !fence_event_ || value == 0) return;
  if (fence_->GetCompletedValue() >= value) return;
  fence_->SetEventOnCompletion(value, fence_event_);
  WaitForSingleObject(fence_event_, INFINITE);
}

UINT64 DxDevice::signal_fence() {
  if (removed_ || !queue_ || !fence_) return fence_value_;
  const UINT64 v = ++fence_value_;
  queue_->Signal(fence_, v);
  return v;
}

void DxDevice::collect() {
  const UINT64 done = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
  std::size_t w = 0;
  for (std::size_t i = 0; i < grave_.size(); ++i) {
    if (grave_[i].fence <= done) {
      if (grave_[i].obj) grave_[i].obj->Release();
    } else {
      grave_[w++] = grave_[i];
    }
  }
  grave_.resize(w);
}

void DxDevice::retire(IUnknown* obj) {
  if (!obj) return;
  // An open frame list may still name this object. The signal at end_frame
  // is fence_value_ + 1, which has not been issued yet.
  const UINT64 when = list_open_ ? fence_value_ + 1 : fence_value_;
  grave_.push_back({when, obj});
  collect();
}

bool DxDevice::begin_copy() {
  if (removed_ || !copy_list_ || !copy_alloc_) return false;
  wait_fence(copy_fence_);
  if (failed(copy_alloc_->Reset(), "copy allocator Reset") ||
      failed(copy_list_->Reset(copy_alloc_, nullptr), "copy list Reset")) {
    return false;
  }
  list_ = copy_list_;
  return true;
}

void DxDevice::end_copy() {
  if (!copy_list_ || !queue_) {
    list_ = frame_list_;
    return;
  }
  copy_list_->Close();
  ID3D12CommandList* lists[] = {copy_list_};
  queue_->ExecuteCommandLists(1, lists);
  copy_fence_ = signal_fence();
  wait_fence(copy_fence_);
  list_ = frame_list_;
  collect();
}

Buffer* DxDevice::create_buffer(const BufferDesc& desc, const void* data) {
  if (removed_ || !device_) return nullptr;
  auto* b = new DxBuffer();
  b->kind = desc.kind;
  b->size = std::max<std::size_t>(desc.size, 256);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = b->size;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&b->res));
  if (failed(hr, "CreateCommittedResource(buffer)") || !b->res) {
    delete b;
    return nullptr;
  }
  b->gpu = b->res->GetGPUVirtualAddress();
  D3D12_RANGE range{0, 0};
  if (FAILED(b->res->Map(0, &range, &b->map))) b->map = nullptr;
  if (data && b->map) std::memcpy(b->map, data, desc.size);
  return b;
}
void DxDevice::update_buffer(Buffer* buffer, const void* data, std::size_t size, std::size_t offset) {
  auto* b = static_cast<DxBuffer*>(buffer);
  if (b && b->map && data) std::memcpy(static_cast<char*>(b->map) + offset, data, size);
}
void DxDevice::destroy_buffer(Buffer* buffer) {
  if (!buffer) return;
  auto* b = static_cast<DxBuffer*>(buffer);
  if (b->res) {
    b->res->Unmap(0, nullptr);
    b->map = nullptr;
    // The open frame list may still hold this buffer's GPU address.
    retire(b->res);
    b->res = nullptr;
  }
  delete b;
}
Texture* DxDevice::create_texture(const TextureDesc& desc, const void* rgba) {
  auto* t = new DxTexture();
  t->width = desc.width;
  t->height = desc.height;
  t->depth = desc.format == TextureFormat::Depth;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = static_cast<UINT64>(desc.width);
  rd.Height = static_cast<UINT>(desc.height);
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  // A depth resource that is also sampled has to be typeless. D32_FLOAT
  // cannot back an R32_FLOAT shader resource view, so the shadow map
  // would sample as white and every pixel would look unshadowed.
  rd.Format = t->depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.Flags = t->depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = t->depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  // Color and DepthStencil share a union. Writing both would clobber the
  // depth clear of 1, and the depth pass would warn the other way.
  if (t->depth) {
    clear.DepthStencil.Depth = 1;
    clear.DepthStencil.Stencil = 0;
  } else {
    clear.Color[0] = desc.clear_rgba[0];
    clear.Color[1] = desc.clear_rgba[1];
    clear.Color[2] = desc.clear_rgba[2];
    clear.Color[3] = desc.clear_rgba[3];
  }
  t->state = D3D12_RESOURCE_STATE_COMMON;
  const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, t->state,
                                                      t->depth || !rgba ? &clear : nullptr, IID_PPV_ARGS(&t->res));
  if (failed(hr, "CreateCommittedResource(texture)") || !t->res) {
    delete t;
    return nullptr;
  }
  if (!t->depth) {
    t->cpu = alloc_cpu();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(t->res, &sv, t->cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE visible = alloc_visible();
    device_->CopyDescriptorsSimple(1, visible, t->cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    t->gpu = gpu_visible(visible);
    t->has_srv = true;
    t->imgui = t->gpu.ptr;
    if (rgba) update_texture(t, rgba, desc.width, desc.height);
  }
  return t;
}
void DxDevice::update_texture(Texture* texture, const void* rgba, int w, int h) {
  auto* t = static_cast<DxTexture*>(texture);
  if (!t || !rgba || t->depth) return;
  const std::size_t row = static_cast<std::size_t>(w) * 4u;
  const std::size_t pitch = (row + 255u) & ~255u;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = pitch * static_cast<UINT64>(h);
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* up = nullptr;
  if (failed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&up)),
             "CreateCommittedResource(upload)") ||
      !up) {
    return;
  }
  void* map = nullptr;
  if (FAILED(up->Map(0, nullptr, &map)) || !map) {
    up->Release();
    return;
  }
  for (int y = 0; y < h; ++y) {
    std::memcpy(static_cast<char*>(map) + static_cast<std::size_t>(y) * pitch, static_cast<const char*>(rgba) + static_cast<std::size_t>(y) * row, row);
  }
  up->Unmap(0, nullptr);
  // Own list and fence. The frame list stays open across Project Manager ->
  // Editor3D, which uploads meshes while ImGui's frame is recording.
  if (!begin_copy()) {
    up->Release();
    return;
  }
  barrier(t->res, t->state, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = t->res;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = up;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
  src.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
  copy_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  t->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  end_copy();
  retire(up);
}
void DxDevice::destroy_texture(Texture* texture) {
  if (!texture) return;
  auto* t = static_cast<DxTexture*>(texture);
  if (t->res) {
    retire(t->res);
    t->res = nullptr;
  }
  delete t;
}
std::uint64_t DxDevice::imgui_texture(Texture* texture) {
  auto* t = static_cast<DxTexture*>(texture);
  return t && t->has_srv ? t->gpu.ptr : 0;
}
Sampler* DxDevice::create_sampler(const SamplerDesc& desc) {
  auto* s = new DxSampler();
  s->desc = desc;
  return s;
}
void DxDevice::destroy_sampler(Sampler* sampler) { delete static_cast<DxSampler*>(sampler); }

Pipeline* DxDevice::create_pipeline(const PipelineDesc& desc, std::string* error) {
  const ShaderCode* vs = find_shader(desc.vs);
  const ShaderCode* fs = find_shader(desc.fs);
  if (!vs || !fs || !vs->dxil_bytes || !fs->dxil_bytes) {
    if (error) *error = "missing DXIL shader";
    return nullptr;
  }
  const bool depth_only = desc.fs && std::strcmp(desc.fs, "depth.frag") == 0;
  D3D12_INPUT_ELEMENT_DESC elems[8]{};
  for (int i = 0; i < desc.attr_count; ++i) {
    elems[i].SemanticName = "TEXCOORD";
    elems[i].SemanticIndex = desc.attrs[i].location;
    elems[i].Format = desc.attrs[i].components == 4   ? DXGI_FORMAT_R32G32B32A32_FLOAT
                      : desc.attrs[i].components == 2 ? DXGI_FORMAT_R32G32_FLOAT
                      : desc.attrs[i].components == 1 ? DXGI_FORMAT_R32_FLOAT
                                                      : DXGI_FORMAT_R32G32B32_FLOAT;
    elems[i].InputSlot = desc.attrs[i].buffer;
    elems[i].AlignedByteOffset = desc.attrs[i].offset;
    elems[i].InputSlotClass = desc.attrs[i].instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                     : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    elems[i].InstanceDataStepRate = desc.attrs[i].instance ? 1u : 0u;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = root_;
  pso.VS = {vs->dxil, vs->dxil_bytes};
  pso.PS = {fs->dxil, fs->dxil_bytes};
  pso.InputLayout = {elems, static_cast<UINT>(desc.attr_count)};
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = desc.cull == Cull::None ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
  // CCW meshes (the same winding OpenGL and Vulkan keep) are counter-clockwise
  // on this render target. Treating them as back faces culled the ground, so
  // the shadow map only received the cube and the color target showed the sky
  // where the ground should be. The cube's far side was what survived.
  pso.RasterizerState.FrontCounterClockwise = TRUE;
  // DepthBias for a float depth buffer is scaled by a tiny ulp. A bias of 2
  // does not move a D32 shadow and the whole ground self-shadows. Scale it
  // up, then clamp so a real shadow (a few centimetres of depth) stays.
  pso.RasterizerState.DepthBias = desc.polygon_offset ? 100000 : 0;
  pso.RasterizerState.SlopeScaledDepthBias = desc.polygon_offset ? 1.5f : 0.0f;
  pso.RasterizerState.DepthBiasClamp = desc.polygon_offset ? 0.005f : 0.0f;
  pso.RasterizerState.DepthClipEnable = TRUE;
  if (desc.blend != Blend::Off) {
    auto& b = pso.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    b.DestBlend = desc.blend == Blend::Additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  }
  pso.DepthStencilState.DepthEnable = desc.depth_test ? TRUE : FALSE;
  pso.DepthStencilState.DepthWriteMask = desc.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc = desc.depth_cmp == Compare::Always ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_LESS;
  // Zero is not a legal stencil op (KEEP is 1). CreateGraphicsPipelineState
  // rejects the whole PSO when these are left at the zero a `{}` gives them,
  // even with the stencil test off.
  pso.DepthStencilState.StencilReadMask = 0xff;
  pso.DepthStencilState.StencilWriteMask = 0xff;
  pso.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = desc.topology == Topology::Lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                               : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  // Every slot past NumRenderTargets must be UNKNOWN. A depth-only shadow
  // pass has no color target, so all eight stay unknown. The color and
  // shadow targets both bind a D32 depth buffer, including draws that do
  // not test depth (sky, post), so the DSV format stays D32.
  pso.NumRenderTargets = depth_only ? 0 : 1;
  for (int i = 0; i < 8; ++i) pso.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
  if (depth_only) {
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
  } else {
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  pso.SampleDesc.Count = 1;
  pso.SampleDesc.Quality = 0;
  ID3D12PipelineState* p = nullptr;
  const HRESULT phr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&p));
  if (FAILED(phr)) {
    const char* vs = desc.vs ? desc.vs : "?";
    const char* fs = desc.fs ? desc.fs : "?";
    explain(error, std::string("CreateGraphicsPipelineState failed for ") + vs + " / " + fs, phr);
    return nullptr;
  }
  auto* out = new DxPipeline();
  out->pso = p;
  out->depth_only = depth_only;
  out->ray = desc.ray_query;
  out->stride0 = desc.strides[0];
  out->stride1 = desc.strides[1];
  out->lines = desc.topology == Topology::Lines;
  return out;
}
void DxDevice::destroy_pipeline(Pipeline* pipeline) {
  if (!pipeline) return;
  auto* p = static_cast<DxPipeline*>(pipeline);
  if (p->pso) {
    retire(p->pso);
    p->pso = nullptr;
  }
  delete p;
}
RenderTarget* DxDevice::create_target(const TargetDesc& desc, std::string* error) {
  auto* t = new DxTarget();
  t->width = desc.width;
  t->height = desc.height;
  t->color_on = desc.color;
  TextureDesc td;
  td.width = desc.width;
  td.height = desc.height;
  if (desc.color) {
    td.clear_rgba[0] = desc.clear_rgba[0];
    td.clear_rgba[1] = desc.clear_rgba[1];
    td.clear_rgba[2] = desc.clear_rgba[2];
    td.clear_rgba[3] = desc.clear_rgba[3];
    t->color = static_cast<DxTexture*>(create_texture(td, nullptr));
    if (!t->color || !t->color->res) {
      delete t;
      return nullptr;
    }
    t->rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    t->rtv.ptr += static_cast<SIZE_T>(rtv_used_++) * rtv_stride_;
    device_->CreateRenderTargetView(t->color->res, nullptr, t->rtv);
  }
  if (desc.depth) {
    td.format = TextureFormat::Depth;
    t->depth = static_cast<DxTexture*>(create_texture(td, nullptr));
    if (!t->depth || !t->depth->res) {
      destroy_texture(t->color);
      delete t;
      return nullptr;
    }
    t->dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    t->dsv.ptr += static_cast<SIZE_T>(dsv_used_++) * dsv_stride_;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(t->depth->res, &dv, t->dsv);
    t->depth->cpu = alloc_cpu();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(t->depth->res, &sv, t->depth->cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE visible = alloc_visible();
    device_->CopyDescriptorsSimple(1, visible, t->depth->cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    t->depth->gpu = gpu_visible(visible);
    t->depth->has_srv = true;
  }
  (void)error;
  return t;
}
void DxDevice::destroy_target(RenderTarget* target) {
  if (!target) return;
  auto* t = static_cast<DxTarget*>(target);
  destroy_texture(t->color);
  destroy_texture(t->depth);
  delete t;
}
void DxDevice::begin_frame() {
  if (depth_++ > 0) return;
  if (removed_ || !device_) return;
  if (window_) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w > 0 && h > 0 && swap_ && (w != width_ || h != height_)) resize_swapchain(w, h);
    if (w > 0 && h > 0) { width_ = w; height_ = h; }
  }
  // This allocator's last submit has to be done. Resetting one the GPU is
  // still reading is COMMAND_ALLOCATOR_CANNOT_RESET.
  wait_fence(frame_fence_[frame_slot_]);
  if (failed(frame_alloc_[frame_slot_]->Reset(), "frame allocator Reset") ||
      failed(frame_list_->Reset(frame_alloc_[frame_slot_], nullptr), "frame list Reset")) {
    return;
  }
  list_ = frame_list_;
  cmd_.list = frame_list_;
  cmd_.dev = this;
  cmd_.pass = false;
  list_open_ = true;
  swap_drawn_ = false;
}
void DxDevice::end_frame() {
  if (depth_ == 0) return;
  if (--depth_ > 0) return;
  if (removed_ || !device_ || !frame_list_) return;
  if (!list_open_) return;
  list_ = frame_list_;
  if (swap_ && frame_index_ < 2 && back_[frame_index_] && back_state_[frame_index_] != D3D12_RESOURCE_STATE_PRESENT) {
    barrier(back_[frame_index_], back_state_[frame_index_], D3D12_RESOURCE_STATE_PRESENT);
    back_state_[frame_index_] = D3D12_RESOURCE_STATE_PRESENT;
  }
  frame_list_->Close();
  list_open_ = false;
  ID3D12CommandList* lists[] = {frame_list_};
  queue_->ExecuteCommandLists(1, lists);
  frame_fence_[frame_slot_] = signal_fence();
  wait_fence(frame_fence_[frame_slot_]);
  frame_slot_ = (frame_slot_ + 1) % 2;
  collect();
  deliver_swapchain_read();
  if (swap_ && swap_drawn_ && !removed_) {
    const HRESULT phr = swap_->Present(1, 0);
    if (FAILED(phr)) failed(phr, "Present");
  }
  swap_drawn_ = false;
}

void DxDevice::create_backbuffers() {
  if (!swap_ || !rtv_heap_ || !device_) return;
  if (rtv_used_ < 2) rtv_used_ = 2;
  for (UINT i = 0; i < 2; ++i) {
    if (back_[i]) { back_[i]->Release(); back_[i] = nullptr; }
    if (FAILED(swap_->GetBuffer(i, IID_PPV_ARGS(&back_[i])))) continue;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * rtv_stride_;
    device_->CreateRenderTargetView(back_[i], nullptr, h);
    back_rtv_[i] = h;
    back_state_[i] = D3D12_RESOURCE_STATE_PRESENT;
  }
}

void DxDevice::resize_swapchain(int w, int h) {
  if (!swap_ || w <= 0 || h <= 0) return;
  for (ID3D12Resource*& b : back_) {
    if (b) { b->Release(); b = nullptr; }
  }
  swap_->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0);
  width_ = w;
  height_ = h;
  create_backbuffers();
}

void DxDevice::bind_swapchain(const PassDesc& pass) {
  if (!swap_ || !list_) return;
  frame_index_ = swap_->GetCurrentBackBufferIndex();
  if (frame_index_ > 1 || !back_[frame_index_]) return;
  barrier(back_[frame_index_], back_state_[frame_index_], D3D12_RESOURCE_STATE_RENDER_TARGET);
  back_state_[frame_index_] = D3D12_RESOURCE_STATE_RENDER_TARGET;
  list_->OMSetRenderTargets(1, &back_rtv_[frame_index_], FALSE, nullptr);
  if (pass.clear_color) {
    const float c[4] = {pass.rgba[0], pass.rgba[1], pass.rgba[2], pass.rgba[3]};
    list_->ClearRenderTargetView(back_rtv_[frame_index_], c, 0, nullptr);
  }
  swap_drawn_ = true;
}

void DxDevice::blit_swapchain(Texture* color) {
  auto* src = static_cast<DxTexture*>(color);
  if (!swap_ || !list_ || !src || !src->res || src->depth) return;
  frame_index_ = swap_->GetCurrentBackBufferIndex();
  if (frame_index_ > 1 || !back_[frame_index_]) return;
  barrier(back_[frame_index_], back_state_[frame_index_], D3D12_RESOURCE_STATE_COPY_DEST);
  barrier(src->res, src->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION sl{};
  sl.pResource = src->res;
  sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dl{};
  dl.pResource = back_[frame_index_];
  dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const UINT cw = static_cast<UINT>(std::min(src->width, width_));
  const UINT ch = static_cast<UINT>(std::min(src->height, height_));
  D3D12_BOX box{0, 0, 0, cw, ch, 1};
  list_->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
  barrier(src->res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  src->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier(back_[frame_index_], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  back_state_[frame_index_] = D3D12_RESOURCE_STATE_PRESENT;
  swap_drawn_ = true;
}

bool DxDevice::read_swapchain(std::vector<std::uint8_t>* rgba, int* w, int* h) {
  if (!rgba || !swap_ || !list_ || frame_index_ > 1 || !back_[frame_index_]) return false;
  const std::size_t row = static_cast<std::size_t>(width_) * 4u;
  const std::size_t pitch = (row + 255u) & ~255u;
  if (swap_read_) { swap_read_->Release(); swap_read_ = nullptr; }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = pitch * static_cast<UINT64>(height_);
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&swap_read_)))) {
    return false;
  }
  barrier(back_[frame_index_], back_state_[frame_index_], D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = back_[frame_index_];
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = swap_read_;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(width_);
  dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(height_);
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(back_[frame_index_], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  back_state_[frame_index_] = D3D12_RESOURCE_STATE_PRESENT;
  swap_drawn_ = true;
  swap_read_dst_ = rgba;
  swap_read_w_ = w;
  swap_read_h_ = h;
  return true;
}

void DxDevice::deliver_swapchain_read() {
  if (!swap_read_dst_ || !swap_read_) {
    swap_read_dst_ = nullptr;
    return;
  }
  const std::size_t row = static_cast<std::size_t>(width_) * 4u;
  const std::size_t pitch = (row + 255u) & ~255u;
  void* map = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(pitch * static_cast<std::size_t>(height_))};
  swap_read_->Map(0, &range, &map);
  swap_read_dst_->assign(row * static_cast<std::size_t>(height_), 0);
  if (map) {
    for (int y = 0; y < height_; ++y) {
      std::memcpy(swap_read_dst_->data() + static_cast<std::size_t>(height_ - 1 - y) * row,
                  static_cast<char*>(map) + static_cast<std::size_t>(y) * pitch, row);
    }
  }
  swap_read_->Unmap(0, nullptr);
  swap_read_->Release();
  swap_read_ = nullptr;
  if (swap_read_w_) *swap_read_w_ = width_;
  if (swap_read_h_) *swap_read_h_ = height_;
  swap_read_dst_ = nullptr;
  swap_read_w_ = nullptr;
  swap_read_h_ = nullptr;
}

bool DxDevice::read_color(RenderTarget* target, std::vector<std::uint8_t>* rgba, int* w, int* h) {
  auto* t = static_cast<DxTarget*>(target);
  if (!t || !t->color || !rgba) return false;
  const std::size_t row = static_cast<std::size_t>(t->width) * 4u;
  const std::size_t pitch = (row + 255u) & ~255u;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = pitch * static_cast<UINT64>(t->height);
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* rb = nullptr;
  if (failed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&rb)),
             "CreateCommittedResource(readback)") ||
      !rb) {
    return false;
  }
  if (!begin_copy()) {
    rb->Release();
    return false;
  }
  barrier(t->color->res, t->color->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = t->color->res;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(t->width);
  dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(t->height);
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(pitch);
  copy_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(t->color->res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  t->color->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  end_copy();
  void* map = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(rd.Width)};
  rb->Map(0, &range, &map);
  rgba->assign(row * static_cast<std::size_t>(t->height), 0);
  for (int y = 0; y < t->height; ++y) {
    std::memcpy(rgba->data() + static_cast<std::size_t>(t->height - 1 - y) * row,
                static_cast<char*>(map) + static_cast<std::size_t>(y) * pitch, row);
  }
  rb->Unmap(0, nullptr);
  retire(rb);
  if (w) *w = t->width;
  if (h) *h = t->height;
  return true;
}
bool DxDevice::read_depth(RenderTarget* target, std::vector<float>* depth, int* w, int* h) {
  auto* t = static_cast<DxTarget*>(target);
  if (!t || !t->depth || !t->depth->res || !depth) return false;
  const UINT row_bytes = (static_cast<UINT>(t->width) * 4u + 255u) & ~255u;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = static_cast<UINT64>(row_bytes) * static_cast<UINT64>(t->height);
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* rb = nullptr;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&rb)))) {
    return false;
  }
  if (!begin_copy()) {
    rb->Release();
    return false;
  }
  barrier(t->depth->res, t->depth->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = t->depth->res;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
  dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(t->width);
  dst.PlacedFootprint.Footprint.Height = static_cast<UINT>(t->height);
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = row_bytes;
  copy_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(t->depth->res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  t->depth->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  end_copy();
  void* map = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(rd.Width)};
  rb->Map(0, &range, &map);
  depth->assign(static_cast<std::size_t>(t->width) * static_cast<std::size_t>(t->height), 0.0f);
  for (int y = 0; y < t->height; ++y) {
    const float* src_row = reinterpret_cast<const float*>(static_cast<const char*>(map) + static_cast<std::size_t>(y) * row_bytes);
    float* dst_row = depth->data() + static_cast<std::size_t>(t->height - 1 - y) * static_cast<std::size_t>(t->width);
    std::memcpy(dst_row, src_row, static_cast<std::size_t>(t->width) * sizeof(float));
  }
  rb->Unmap(0, nullptr);
  retire(rb);
  if (w) *w = t->width;
  if (h) *h = t->height;
  return true;
}
std::uint32_t DxDevice::rt_create_blas(const RtGeometry&, std::string* error) {
  if (error) *error = raytracing_ ? "Direct3D 12 ray query acceleration structures are built by the forward renderer when a scene asks for them"
                                  : "this GPU does not support DirectX Raytracing tier 1.1";
  return 0;
}
std::uint32_t DxDevice::rt_create_tlas(const RtInstance*, std::uint32_t, std::string* error) {
  if (error) *error = "Direct3D 12 TLAS build is reported by the device capability; the scene falls back to shadow maps";
  return 0;
}
void DxDevice::rt_reset() {}

int DxDevice::debug_error_count() const {
  if (!device_) return debug_errors_;
  Com<ID3D12InfoQueue> queue;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(queue.out())))) return debug_errors_;
  const UINT64 n = queue->GetNumStoredMessages();
  for (UINT64 i = 0; i < n; ++i) {
    SIZE_T bytes = 0;
    if (FAILED(queue->GetMessage(i, nullptr, &bytes)) || bytes == 0) continue;
    std::vector<unsigned char> mem(bytes);
    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(mem.data());
    if (FAILED(queue->GetMessage(i, msg, &bytes))) continue;
    if (msg->Severity != D3D12_MESSAGE_SEVERITY_ERROR && msg->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION) continue;
    ++debug_errors_;
    if (msg->pDescription) std::fprintf(stderr, "D3D12: %s\n", msg->pDescription);
  }
  queue->ClearStoredMessages();
  return debug_errors_;
}

bool DxDevice::dx_info(DxInfo* out) const {
  if (!out) return false;
  out->device = device_;
  out->queue = queue_;
  out->list = list_;
  out->srv_heap = srv_;
  out->frames = 2;
  out->width = width_;
  out->height = height_;
  out->format = DXGI_FORMAT_R8G8B8A8_UNORM;
  return true;
}

std::unique_ptr<Device> create_dx12_device(const DeviceDesc& desc, std::string* error) {
  auto dev = std::make_unique<DxDevice>();
  if (!dev->open(desc, error)) return nullptr;
  return dev;
}

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts

#endif
