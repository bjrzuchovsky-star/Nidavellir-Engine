#include "rhi/Internal.h"
#include "rhi/ShaderLib.h"
#include "render3d/GlApi.h"

#include <GLFW/glfw3.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace rhi {
namespace render3d = ::ts::tombstone::render3d;
using render3d::Gl33;
using render3d::gl33;
using render3d::load_gl33;
using render3d::GLintptr_ts;
using render3d::GLsizeiptr_ts;

#ifndef GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#endif

struct GlBuffer : Buffer {
  GLuint id = 0;
  BufferKind kind = BufferKind::Vertex;
  std::size_t size = 0;
};
struct GlTexture : Texture {
  GLuint id = 0;
  TextureFormat format = TextureFormat::RGBA8;
  int width = 1;
  int height = 1;
  std::uint64_t imgui = 0;
};
struct GlSampler : Sampler {
  SamplerDesc desc;
};
struct GlPipeline : Pipeline {
  GLuint program = 0;
  PipelineDesc desc;
};
struct GlTarget : RenderTarget {
  GLuint fbo = 0;
  GlTexture* color = nullptr;
  GlTexture* depth = nullptr;
  int width = 1;
  int height = 1;
};

GLenum buffer_target(BufferKind kind) {
  switch (kind) {
    case BufferKind::Index: return GL_ELEMENT_ARRAY_BUFFER;
    case BufferKind::Uniform: return GL_UNIFORM_BUFFER;
    default: return GL_ARRAY_BUFFER;
  }
}

const char* kSamplerNames[] = {
    "u_albedo_tex", "u_normal_tex", "u_mr_tex", "u_shadow", "u_splat", "u_layer0",
    "u_layer1",     "u_layer2",     "u_layer3", "u_hdri",   "u_ao",    "u_bloom"};
constexpr int kSamplerUnits[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

class GlCmd final : public CommandList {
 public:
  explicit GlCmd(GLuint vao) : vao_(vao) {}
  void begin_pass(const PassDesc& pass) override {
    end_pass();
    const Gl33& g = gl33();
    pass_ = pass;
    if (pass.target) {
      auto* t = static_cast<GlTarget*>(pass.target);
      g.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
      if (!pass.color) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
      } else {
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
      }
    } else {
      g.BindFramebuffer(GL_FRAMEBUFFER, 0);
      glDrawBuffer(GL_BACK);
    }
    GLbitfield bits = 0;
    if (pass.clear_color && pass.color) {
      glClearColor(pass.rgba[0], pass.rgba[1], pass.rgba[2], pass.rgba[3]);
      bits |= GL_COLOR_BUFFER_BIT;
    }
    if (pass.clear_depth) {
      glClearDepth(1.0);
      bits |= GL_DEPTH_BUFFER_BIT;
      glDepthMask(GL_TRUE);
    }
    if (bits) glClear(bits);
    open_ = true;
  }
  void end_pass() override { open_ = false; }
  void set_viewport(int x, int y, int w, int h) override { glViewport(x, y, w, h); }
  void set_pipeline(Pipeline* pipeline) override {
    pipe_ = static_cast<GlPipeline*>(pipeline);
    if (!pipe_) return;
    const Gl33& g = gl33();
    g.UseProgram(pipe_->program);
    const PipelineDesc& d = pipe_->desc;
    if (d.depth_test) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthFunc(d.depth_cmp == Compare::Always ? GL_ALWAYS : GL_LESS);
    glDepthMask(d.depth_write ? GL_TRUE : GL_FALSE);
    if (d.cull == Cull::None) glDisable(GL_CULL_FACE);
    else {
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
    }
    if (d.blend == Blend::Off) glDisable(GL_BLEND);
    else {
      glEnable(GL_BLEND);
      if (d.blend == Blend::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    if (d.polygon_offset) {
      glEnable(GL_POLYGON_OFFSET_FILL);
      glPolygonOffset(1.5f, 2.0f);
    } else {
      glDisable(GL_POLYGON_OFFSET_FILL);
    }
  }
  void set_vertex_buffer(int slot, Buffer* buffer) override {
    if (slot >= 0 && slot < 2) vb_[slot] = static_cast<GlBuffer*>(buffer);
  }
  void set_index_buffer(Buffer* buffer) override { ib_ = static_cast<GlBuffer*>(buffer); }
  void set_uniform(int slot, Buffer* buffer, std::size_t offset, std::size_t size) override {
    auto* b = static_cast<GlBuffer*>(buffer);
    if (!b) return;
    gl33().BindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(slot), b->id,
                           static_cast<GLintptr_ts>(offset), static_cast<GLsizeiptr_ts>(size));
  }
  void set_texture(int slot, Texture* texture, Sampler* sampler) override {
    auto* t = static_cast<GlTexture*>(texture);
    const Gl33& g = gl33();
    g.ActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, t ? t->id : 0);
    if (t && sampler) {
      const SamplerDesc& s = static_cast<GlSampler*>(sampler)->desc;
      const GLint wrap = s.wrap == Wrap::Clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT;
      const GLint mag = s.filter == Filter::Nearest ? GL_NEAREST : GL_LINEAR;
      const GLint min = s.mips ? (s.filter == Filter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR)
                               : mag;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    }
  }
  void set_tlas(std::uint32_t) override {}
  void draw(std::uint32_t vertices, std::uint32_t instances, std::uint32_t first) override {
    bind_mesh();
    if (instances > 1) {
      gl33().DrawElementsInstanced(prim(), static_cast<GLsizei>(vertices), GL_UNSIGNED_INT,
                                   reinterpret_cast<const void*>(static_cast<std::uintptr_t>(first) * 4u),
                                   static_cast<GLsizei>(instances));
    } else if (ib_) {
      glDrawElements(prim(), static_cast<GLsizei>(vertices), GL_UNSIGNED_INT,
                     reinterpret_cast<const void*>(static_cast<std::uintptr_t>(first) * 4u));
    } else {
      glDrawArrays(prim(), static_cast<GLint>(first), static_cast<GLsizei>(vertices));
    }
  }
  void draw_indexed(std::uint32_t indices, std::uint32_t instances, std::uint32_t first) override {
    draw(indices, instances, first);
  }

 private:
  GLenum prim() const {
    return pipe_ && pipe_->desc.topology == Topology::Lines ? GL_LINES : GL_TRIANGLES;
  }
  void bind_mesh() {
    const Gl33& g = gl33();
    g.BindVertexArray(vao_);
    if (!pipe_) return;
    for (int i = 0; i < pipe_->desc.attr_count; ++i) {
      const VertexAttr& a = pipe_->desc.attrs[i];
      auto* b = a.buffer < 2 ? vb_[a.buffer] : nullptr;
      g.BindBuffer(GL_ARRAY_BUFFER, b ? b->id : 0);
      g.EnableVertexAttribArray(a.location);
      g.VertexAttribPointer(a.location, static_cast<GLint>(a.components), GL_FLOAT, GL_FALSE,
                            static_cast<GLsizei>(pipe_->desc.strides[a.buffer]),
                            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a.offset)));
      if (g.VertexAttribDivisor) g.VertexAttribDivisor(a.location, a.instance ? 1u : 0u);
    }
    if (ib_) g.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_->id);
  }
  GLuint vao_ = 0;
  PassDesc pass_{};
  bool open_ = false;
  GlPipeline* pipe_ = nullptr;
  GlBuffer* vb_[2] = {};
  GlBuffer* ib_ = nullptr;
};

class GlDevice final : public Device {
 public:
  ~GlDevice() override { shutdown(); }

  Api api() const override { return Api::OpenGL; }
  const char* adapter_name() const override { return name_.c_str(); }
  bool raytracing() const override { return false; }
  bool bottom_up() const override { return true; }
  void clip_params(float o[4]) const override { o[0] = 1; o[1] = 1; o[2] = 0; o[3] = 0; }
  bool shadow_flip_v() const override { return false; }
  void* window() const override { return window_; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  bool open(const DeviceDesc& desc, std::string* error) {
    desc_ = desc;
    if (!desc.window) {
      if (!glfwInit()) {
        if (error) *error = "glfwInit failed";
        return false;
      }
      owned_glfw_ = true;
      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
      glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
      glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
      glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
      glfwWindowHint(GLFW_VISIBLE, desc.visible && !desc.offscreen ? GLFW_TRUE : GLFW_FALSE);
      window_ = glfwCreateWindow(desc.width, desc.height, desc.title ? desc.title : "Nidavellir", nullptr, nullptr);
      if (!window_) {
        if (error) *error = "OpenGL 3.3 window failed";
        return false;
      }
      owned_window_ = true;
    } else {
      window_ = static_cast<GLFWwindow*>(desc.window);
    }
    glfwMakeContextCurrent(window_);
    if (!load_gl33(error)) return false;
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    name_ = renderer ? renderer : "OpenGL";
    GLint align = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    set_uniform_align(static_cast<std::size_t>(align > 0 ? align : 256));
    gl33().GenVertexArrays(1, &vao_);
    glfwGetFramebufferSize(window_, &width_, &height_);
    if (width_ < 1) width_ = desc.width;
    if (height_ < 1) height_ = desc.height;
    cmd_ = std::make_unique<GlCmd>(vao_);
    return true;
  }

  void shutdown() {
    if (!window_) return;
    glfwMakeContextCurrent(window_);
    if (vao_) gl33().DeleteVertexArrays(1, &vao_);
    vao_ = 0;
    if (owned_window_) glfwDestroyWindow(window_);
    window_ = nullptr;
    if (owned_glfw_) glfwTerminate();
    owned_glfw_ = false;
  }

  Buffer* create_buffer(const BufferDesc& desc, const void* data) override {
    auto* b = new GlBuffer();
    b->kind = desc.kind;
    b->size = desc.size;
    const Gl33& g = gl33();
    g.GenBuffers(1, &b->id);
    g.BindBuffer(buffer_target(desc.kind), b->id);
    g.BufferData(buffer_target(desc.kind), static_cast<GLsizeiptr_ts>(desc.size), data, GL_DYNAMIC_DRAW);
    return b;
  }
  void update_buffer(Buffer* buffer, const void* data, std::size_t size, std::size_t offset) override {
    auto* b = static_cast<GlBuffer*>(buffer);
    const Gl33& g = gl33();
    g.BindBuffer(buffer_target(b->kind), b->id);
    g.BufferSubData(buffer_target(b->kind), static_cast<GLintptr_ts>(offset), static_cast<GLsizeiptr_ts>(size), data);
  }
  void destroy_buffer(Buffer* buffer) override {
    if (!buffer) return;
    auto* b = static_cast<GlBuffer*>(buffer);
    gl33().DeleteBuffers(1, &b->id);
    delete b;
  }
  Texture* create_texture(const TextureDesc& desc, const void* rgba) override {
    auto* t = new GlTexture();
    t->format = desc.format;
    t->width = desc.width;
    t->height = desc.height;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (desc.format == TextureFormat::Depth) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, desc.width, desc.height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,
                   nullptr);
    } else {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, desc.width, desc.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    const GLint wrap = GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, desc.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (desc.mips && desc.format != TextureFormat::Depth && rgba) gl33().GenerateMipmap(GL_TEXTURE_2D);
    t->imgui = t->id;
    return t;
  }
  void update_texture(Texture* texture, const void* rgba, int w, int h) override {
    auto* t = static_cast<GlTexture*>(texture);
    t->width = w;
    t->height = h;
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  }
  void destroy_texture(Texture* texture) override {
    if (!texture) return;
    auto* t = static_cast<GlTexture*>(texture);
    glDeleteTextures(1, &t->id);
    delete t;
  }
  std::uint64_t imgui_texture(Texture* texture) override {
    return texture ? static_cast<GlTexture*>(texture)->imgui : 0;
  }
  Sampler* create_sampler(const SamplerDesc& desc) override {
    auto* s = new GlSampler();
    s->desc = desc;
    return s;
  }
  void destroy_sampler(Sampler* sampler) override { delete static_cast<GlSampler*>(sampler); }
  Pipeline* create_pipeline(const PipelineDesc& desc, std::string* error) override {
    const ShaderCode* vs = find_shader(desc.vs);
    const ShaderCode* fs = find_shader(desc.fs);
    if (!vs || !fs || !vs->glsl || !fs->glsl) {
      if (error) *error = std::string("missing baked shader ") + (desc.vs ? desc.vs : "?");
      return nullptr;
    }
    std::string err;
    const GLuint program = link(vs->glsl, fs->glsl, &err);
    if (!program) {
      if (error) *error = err;
      return nullptr;
    }
    const Gl33& g = gl33();
    auto bind_block = [&](const char* name, GLuint point) {
      const GLuint idx = g.GetUniformBlockIndex(program, name);
      if (idx != GL_INVALID_INDEX) g.UniformBlockBinding(program, idx, point);
    };
    bind_block("Frame", 0);
    bind_block("Draw", 1);
    bind_block("Bones", 2);
    g.UseProgram(program);
    for (int i = 0; i < 12; ++i) {
      const GLint loc = g.GetUniformLocation(program, kSamplerNames[i]);
      if (loc >= 0) g.Uniform1i(loc, kSamplerUnits[i]);
    }
    auto* p = new GlPipeline();
    p->program = program;
    p->desc = desc;
    return p;
  }
  void destroy_pipeline(Pipeline* pipeline) override {
    if (!pipeline) return;
    auto* p = static_cast<GlPipeline*>(pipeline);
    gl33().DeleteProgram(p->program);
    delete p;
  }
  RenderTarget* create_target(const TargetDesc& desc, std::string* error) override {
    auto* t = new GlTarget();
    t->width = desc.width;
    t->height = desc.height;
    TextureDesc cd;
    cd.width = desc.width;
    cd.height = desc.height;
    cd.format = TextureFormat::RGBA8;
    if (desc.color) t->color = static_cast<GlTexture*>(create_texture(cd, nullptr));
    if (desc.depth) {
      TextureDesc dd = cd;
      dd.format = TextureFormat::Depth;
      t->depth = static_cast<GlTexture*>(create_texture(dd, nullptr));
    }
    const Gl33& g = gl33();
    g.GenFramebuffers(1, &t->fbo);
    g.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    if (t->color) g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color->id, 0);
    else {
      glDrawBuffer(GL_NONE);
      glReadBuffer(GL_NONE);
    }
    if (t->depth) g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t->depth->id, 0);
    const bool ok = g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    g.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
      if (error) *error = "OpenGL framebuffer incomplete";
      destroy_target(t);
      return nullptr;
    }
    return t;
  }
  void destroy_target(RenderTarget* target) override {
    if (!target) return;
    auto* t = static_cast<GlTarget*>(target);
    if (t->fbo) gl33().DeleteFramebuffers(1, &t->fbo);
    destroy_texture(t->color);
    destroy_texture(t->depth);
    delete t;
  }
  Texture* target_color(RenderTarget* target) override {
    return target ? static_cast<GlTarget*>(target)->color : nullptr;
  }
  Texture* target_depth(RenderTarget* target) override {
    return target ? static_cast<GlTarget*>(target)->depth : nullptr;
  }

  void begin_frame() override {
    glfwMakeContextCurrent(window_);
    glfwGetFramebufferSize(window_, &width_, &height_);
    if (width_ < 1) width_ = 1;
    if (height_ < 1) height_ = 1;
    ++depth_;
  }
  void end_frame() override {
    if (depth_ == 0) return;
    if (--depth_ == 0 && window_ && !desc_.offscreen) glfwSwapBuffers(window_);
  }
  CommandList* commands() override { return depth_ > 0 ? cmd_.get() : nullptr; }
  void wait_idle() override { glFinish(); }
  bool read_color(RenderTarget* target, std::vector<std::uint8_t>* rgba, int* w, int* h) override {
    if (!target || !rgba) return false;
    auto* t = static_cast<GlTarget*>(target);
    const Gl33& g = gl33();
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    g.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    rgba->assign(static_cast<std::size_t>(t->width) * static_cast<std::size_t>(t->height) * 4u, 0);
    glReadPixels(0, 0, t->width, t->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
    g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
    if (w) *w = t->width;
    if (h) *h = t->height;
    return true;
  }

 private:
  static GLuint compile(GLenum type, const char* src, std::string* err) {
    const Gl33& g = gl33();
    const GLuint s = g.CreateShader(type);
    g.ShaderSource(s, 1, &src, nullptr);
    g.CompileShader(s);
    GLint ok = 0;
    g.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[4096] = {};
      g.GetShaderInfoLog(s, sizeof(log), nullptr, log);
      if (err) *err = log;
      g.DeleteShader(s);
      return 0;
    }
    return s;
  }
  static GLuint link(const char* vs, const char* fs, std::string* err) {
    const Gl33& g = gl33();
    const GLuint v = compile(GL_VERTEX_SHADER, vs, err);
    if (!v) return 0;
    const GLuint f = compile(GL_FRAGMENT_SHADER, fs, err);
    if (!f) {
      g.DeleteShader(v);
      return 0;
    }
    const GLuint p = g.CreateProgram();
    g.AttachShader(p, v);
    g.AttachShader(p, f);
    g.LinkProgram(p);
    g.DeleteShader(v);
    g.DeleteShader(f);
    GLint ok = 0;
    g.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[4096] = {};
      g.GetProgramInfoLog(p, sizeof(log), nullptr, log);
      if (err) *err = log;
      g.DeleteProgram(p);
      return 0;
    }
    return p;
  }

  DeviceDesc desc_{};
  GLFWwindow* window_ = nullptr;
  bool owned_window_ = false;
  bool owned_glfw_ = false;
  std::string name_ = "OpenGL";
  int width_ = 1;
  int height_ = 1;
  int depth_ = 0;
  GLuint vao_ = 0;
  std::unique_ptr<GlCmd> cmd_;
};

std::unique_ptr<Device> create_gl_device(const DeviceDesc& desc, std::string* error) {
  auto dev = std::make_unique<GlDevice>();
  if (!dev->open(desc, error)) return nullptr;
  return dev;
}

}  // namespace rhi
}  // namespace tombstone
}  // namespace ts
