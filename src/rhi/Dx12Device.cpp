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
#include <dxgi1_6.h>

#include <algorithm>
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
  void wait_idle() override { if (queue_) { end_frame_wait(); } }
  bool read_color(RenderTarget* target, std::vector<std::uint8_t>* rgba, int* w, int* h) override;
  std::uint32_t rt_create_blas(const RtGeometry& geo, std::string* error) override;
  std::uint32_t rt_create_tlas(const RtInstance* instances, std::uint32_t count, std::string* error) override;
  void rt_reset() override;
  bool dx_info(DxInfo* out) const override;

  ID3D12Device* device() const { return device_; }
  ID3D12GraphicsCommandList* list() const { return list_; }
  ID3D12RootSignature* root(bool) const { return root_; }
  void barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
  D3D12_CPU_DESCRIPTOR_HANDLE alloc_cpu();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_of(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;
  ID3D12DescriptorHeap* srv_heap() const { return srv_; }
  D3D12_CPU_DESCRIPTOR_HANDLE white_cpu() const { return white_ ? white_->cpu : D3D12_CPU_DESCRIPTOR_HANDLE{}; }
  DxTexture* white_ = nullptr;

  ID3D12Device* device_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  bool raytracing_ = false;

 private:
  void end_frame_wait();
  ID3D12Resource* upload(const void* data, std::size_t size, ID3D12Resource** keep);
  DeviceDesc desc_{};
  Com<IDXGIFactory6> factory_;
  Com<ID3D12CommandQueue> queue_;
  Com<IDXGISwapChain3> swap_;
  Com<ID3D12CommandAllocator> alloc_;
  Com<ID3D12Fence> fence_;
  Com<ID3D12RootSignature> root_;
  Com<ID3D12RootSignature> root_rt_;
  Com<ID3D12DescriptorHeap> rtv_heap_;
  Com<ID3D12DescriptorHeap> dsv_heap_;
  Com<ID3D12DescriptorHeap> srv_;
  UINT srv_stride_ = 0;
  UINT rtv_stride_ = 0;
  UINT dsv_stride_ = 0;
  UINT srv_used_ = 64;  // 0..63 reserved for Dear ImGui
  UINT rtv_used_ = 0;
  UINT dsv_used_ = 0;
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
  D3D12_CPU_DESCRIPTOR_HANDLE h = srv_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += static_cast<SIZE_T>(srv_used_++) * srv_stride_;
  return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE DxDevice::gpu_of(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const {
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
    if (t->color && p.clear_color) {
      const float c[4] = {p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]};
      dev->barrier(t->color->res, t->color->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      t->color->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
      list->ClearRenderTargetView(t->rtv, c, 0, nullptr);
    }
    if (t->depth && p.clear_depth) {
      dev->barrier(t->depth->res, t->depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      t->depth->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
      list->ClearDepthStencilView(t->dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
    rtv = t->color_on ? t->rtv : D3D12_CPU_DESCRIPTOR_HANDLE{};
    dsv = t->depth ? t->dsv : D3D12_CPU_DESCRIPTOR_HANDLE{};
    list->OMSetRenderTargets(t->color_on ? 1 : 0, t->color_on ? &rtv : nullptr, FALSE, t->depth ? &dsv : nullptr);
  }
  pass = true;
  set_viewport(0, 0, w, h);
  ID3D12DescriptorHeap* heaps[] = {dev->srv_heap()};
  list->SetDescriptorHeaps(1, heaps);
}
void DxCmd::end_pass() { pass = false; }
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
  if (!pipe || !pass) return;
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

bool DxDevice::open(const DeviceDesc& desc, std::string* error) {
  desc_ = desc;
  set_uniform_align(256);
  if (CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.out())) != S_OK) {
    if (error) *error = "CreateDXGIFactory2 failed";
    return false;
  }
  Com<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.out())) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)) == S_OK) {
      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, 128, nullptr, nullptr);
      name_ = name;
      break;
    }
  }
  if (!device_) {
    Com<IDXGIAdapter> warp;
    factory_->EnumWarpAdapter(IID_PPV_ARGS(warp.out()));
    if (!warp || D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)) != S_OK) {
      if (error) *error = "D3D12CreateDevice failed";
      return false;
    }
    name_ = "D3D12 WARP";
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
  if (device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)) == S_OK) {
    raytracing_ = opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  device_->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.out()));
  device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc_.out()));
  device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_, nullptr, IID_PPV_ARGS(&list_));
  list_->Close();
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
    samps[i].AddressU = samps[i].AddressV = samps[i].AddressW =
        (i == 3) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samps[i].ShaderRegister = static_cast<UINT>(3 + i);
    samps[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samps[i].MaxLOD = 16;
  }
  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 4;
  rsd.pParameters = params;
  rsd.NumStaticSamplers = 12;
  rsd.pStaticSamplers = samps;
  rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Com<ID3DBlob> blob, errb;
  D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, blob.out(), errb.out());
  device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root_.out()));

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 64;
  device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtv_heap_.out()));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsv_heap_.out()));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 2048;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(srv_.out()));
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
  end_frame_wait();
  rt_reset();
  if (list_) { list_->Release(); list_ = nullptr; }
  if (fence_event_) CloseHandle(fence_event_);
  if (owned_window_ && window_) glfwDestroyWindow(window_);
  if (owned_glfw_) glfwTerminate();
  if (device_) { device_->Release(); device_ = nullptr; }
}

void DxDevice::end_frame_wait() {
  if (!queue_ || !fence_) return;
  const UINT64 v = ++fence_value_;
  queue_->Signal(fence_, v);
  if (fence_->GetCompletedValue() < v) {
    fence_->SetEventOnCompletion(v, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }
}

Buffer* DxDevice::create_buffer(const BufferDesc& desc, const void* data) {
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
  device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                   IID_PPV_ARGS(&b->res));
  b->gpu = b->res->GetGPUVirtualAddress();
  D3D12_RANGE range{0, 0};
  b->res->Map(0, &range, &b->map);
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
    b->res->Release();
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
  rd.Format = t->depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.Flags = t->depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = rd.Format;
  clear.DepthStencil.Depth = 1;
  clear.Color[3] = 1;
  t->state = D3D12_RESOURCE_STATE_COMMON;
  device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, t->state, t->depth || !rgba ? &clear : nullptr,
                                   IID_PPV_ARGS(&t->res));
  if (!t->depth) {
    t->cpu = alloc_cpu();
    t->gpu = gpu_of(t->cpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(t->res, &sv, t->cpu);
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
  device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                   IID_PPV_ARGS(&up));
  void* map = nullptr;
  up->Map(0, nullptr, &map);
  for (int y = 0; y < h; ++y) {
    std::memcpy(static_cast<char*>(map) + static_cast<std::size_t>(y) * pitch, static_cast<const char*>(rgba) + static_cast<std::size_t>(y) * row, row);
  }
  up->Unmap(0, nullptr);
  // Record on a tiny immediate list if the main list is closed.
  alloc_->Reset();
  list_->Reset(alloc_, nullptr);
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
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  t->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  list_->Close();
  ID3D12CommandList* lists[] = {list_};
  queue_->ExecuteCommandLists(1, lists);
  end_frame_wait();
  up->Release();
}
void DxDevice::destroy_texture(Texture* texture) {
  if (!texture) return;
  auto* t = static_cast<DxTexture*>(texture);
  if (t->res) t->res->Release();
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
  pso.RasterizerState.DepthBias = desc.polygon_offset ? 2 : 0;
  pso.RasterizerState.SlopeScaledDepthBias = desc.polygon_offset ? 1.5f : 0;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
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
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = desc.topology == Topology::Lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                               : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = depth_only ? 0 : 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  pso.SampleDesc.Count = 1;
  ID3D12PipelineState* p = nullptr;
  if (device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&p)) != S_OK) {
    if (error) *error = "CreateGraphicsPipelineState failed";
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
  if (p->pso) p->pso->Release();
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
    t->color = static_cast<DxTexture*>(create_texture(td, nullptr));
    t->rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    t->rtv.ptr += static_cast<SIZE_T>(rtv_used_++) * rtv_stride_;
    device_->CreateRenderTargetView(t->color->res, nullptr, t->rtv);
  }
  if (desc.depth) {
    td.format = TextureFormat::Depth;
    t->depth = static_cast<DxTexture*>(create_texture(td, nullptr));
    t->dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    t->dsv.ptr += static_cast<SIZE_T>(dsv_used_++) * dsv_stride_;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(t->depth->res, &dv, t->dsv);
    t->depth->cpu = alloc_cpu();
    t->depth->gpu = gpu_of(t->depth->cpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(t->depth->res, &sv, t->depth->cpu);
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
  alloc_->Reset();
  list_->Reset(alloc_, nullptr);
  cmd_.list = list_;
  cmd_.dev = this;
  cmd_.pass = false;
  if (window_) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w > 0 && h > 0) { width_ = w; height_ = h; }
  }
}
void DxDevice::end_frame() {
  if (depth_ == 0) return;
  if (--depth_ > 0) return;
  list_->Close();
  ID3D12CommandList* lists[] = {list_};
  queue_->ExecuteCommandLists(1, lists);
  if (swap_ && window_) swap_->Present(1, 0);
  end_frame_wait();
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
  device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
  alloc_->Reset();
  list_->Reset(alloc_, nullptr);
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
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(t->color->res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  t->color->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  list_->Close();
  ID3D12CommandList* lists[] = {list_};
  queue_->ExecuteCommandLists(1, lists);
  end_frame_wait();
  void* map = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(rd.Width)};
  rb->Map(0, &range, &map);
  rgba->assign(row * static_cast<std::size_t>(t->height), 0);
  for (int y = 0; y < t->height; ++y) {
    std::memcpy(rgba->data() + static_cast<std::size_t>(t->height - 1 - y) * row,
                static_cast<char*>(map) + static_cast<std::size_t>(y) * pitch, row);
  }
  rb->Unmap(0, nullptr);
  rb->Release();
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
