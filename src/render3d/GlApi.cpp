#include "render3d/GlApi.h"

#include <type_traits>

namespace ts {
namespace tombstone {
namespace render3d {

namespace {
Gl33 g_gl{};
bool g_loaded = false;
}  // namespace

const Gl33& gl33() { return g_gl; }

bool load_gl33(std::string* error) {
  if (g_loaded) return true;
  if (!glfwGetCurrentContext()) {
    if (error) *error = "no current OpenGL context";
    return false;
  }
  const char* missing = nullptr;
  auto load = [&](auto& fn, const char* name) {
    auto* p = glfwGetProcAddress(name);
    if (!p && !missing) missing = name;
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
  };
#define TS_GL(name) load(g_gl.name, "gl" #name)
  TS_GL(CreateShader); TS_GL(ShaderSource); TS_GL(CompileShader); TS_GL(GetShaderiv);
  TS_GL(GetShaderInfoLog); TS_GL(DeleteShader); TS_GL(CreateProgram); TS_GL(AttachShader);
  TS_GL(BindAttribLocation); TS_GL(LinkProgram); TS_GL(GetProgramiv); TS_GL(GetProgramInfoLog);
  TS_GL(DeleteProgram); TS_GL(UseProgram); TS_GL(GetUniformLocation); TS_GL(Uniform1i);
  TS_GL(Uniform1f); TS_GL(Uniform2f); TS_GL(Uniform3f); TS_GL(Uniform4f); TS_GL(Uniform3fv);
  TS_GL(Uniform1fv); TS_GL(UniformMatrix4fv); TS_GL(GenVertexArrays); TS_GL(BindVertexArray);
  TS_GL(DeleteVertexArrays); TS_GL(GenBuffers); TS_GL(BindBuffer); TS_GL(BufferData);
  TS_GL(BufferSubData); TS_GL(DeleteBuffers); TS_GL(VertexAttribPointer);
  TS_GL(EnableVertexAttribArray); TS_GL(GenFramebuffers); TS_GL(BindFramebuffer);
  TS_GL(FramebufferTexture2D); TS_GL(CheckFramebufferStatus); TS_GL(DeleteFramebuffers);
  TS_GL(GenRenderbuffers); TS_GL(BindRenderbuffer); TS_GL(RenderbufferStorage);
  TS_GL(FramebufferRenderbuffer); TS_GL(DeleteRenderbuffers); TS_GL(ActiveTexture);
  TS_GL(GenerateMipmap); TS_GL(BlitFramebuffer); TS_GL(GetUniformBlockIndex);
  TS_GL(UniformBlockBinding); TS_GL(BindBufferBase);
#undef TS_GL
  if (missing) {
    if (error) *error = std::string("OpenGL 3.3 entry point missing: ") + missing;
    return false;
  }
  g_loaded = true;
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
