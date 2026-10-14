#pragma once

// OpenGL 3.3 core entry points for ts_render3d, loaded through
// glfwGetProcAddress (no extra loader library). GL 1.1 functions come
// straight from the platform gl.h; everything newer goes through the
// `gl33()` table. Constants missing from Windows' gl.h are defined here.

#include <GLFW/glfw3.h>

#include <cstddef>
#include <string>

#if defined(_WIN32)
#define TS_GLAPI __stdcall
#else
#define TS_GLAPI
#endif

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif

namespace ts {
namespace tombstone {
namespace render3d {

using GLsizeiptr_ts = std::ptrdiff_t;
using GLintptr_ts = std::ptrdiff_t;

struct Gl33 {
  GLuint(TS_GLAPI* CreateShader)(GLenum);
  void(TS_GLAPI* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
  void(TS_GLAPI* CompileShader)(GLuint);
  void(TS_GLAPI* GetShaderiv)(GLuint, GLenum, GLint*);
  void(TS_GLAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
  void(TS_GLAPI* DeleteShader)(GLuint);
  GLuint(TS_GLAPI* CreateProgram)();
  void(TS_GLAPI* AttachShader)(GLuint, GLuint);
  void(TS_GLAPI* BindAttribLocation)(GLuint, GLuint, const char*);
  void(TS_GLAPI* LinkProgram)(GLuint);
  void(TS_GLAPI* GetProgramiv)(GLuint, GLenum, GLint*);
  void(TS_GLAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
  void(TS_GLAPI* DeleteProgram)(GLuint);
  void(TS_GLAPI* UseProgram)(GLuint);
  GLint(TS_GLAPI* GetUniformLocation)(GLuint, const char*);
  void(TS_GLAPI* Uniform1i)(GLint, GLint);
  void(TS_GLAPI* Uniform1f)(GLint, GLfloat);
  void(TS_GLAPI* Uniform2f)(GLint, GLfloat, GLfloat);
  void(TS_GLAPI* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
  void(TS_GLAPI* Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
  void(TS_GLAPI* Uniform3fv)(GLint, GLsizei, const GLfloat*);
  void(TS_GLAPI* Uniform1fv)(GLint, GLsizei, const GLfloat*);
  void(TS_GLAPI* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
  void(TS_GLAPI* GenVertexArrays)(GLsizei, GLuint*);
  void(TS_GLAPI* BindVertexArray)(GLuint);
  void(TS_GLAPI* DeleteVertexArrays)(GLsizei, const GLuint*);
  void(TS_GLAPI* GenBuffers)(GLsizei, GLuint*);
  void(TS_GLAPI* BindBuffer)(GLenum, GLuint);
  void(TS_GLAPI* BufferData)(GLenum, GLsizeiptr_ts, const void*, GLenum);
  void(TS_GLAPI* BufferSubData)(GLenum, GLintptr_ts, GLsizeiptr_ts, const void*);
  void(TS_GLAPI* DeleteBuffers)(GLsizei, const GLuint*);
  void(TS_GLAPI* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
  void(TS_GLAPI* EnableVertexAttribArray)(GLuint);
  void(TS_GLAPI* GenFramebuffers)(GLsizei, GLuint*);
  void(TS_GLAPI* BindFramebuffer)(GLenum, GLuint);
  void(TS_GLAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  GLenum(TS_GLAPI* CheckFramebufferStatus)(GLenum);
  void(TS_GLAPI* DeleteFramebuffers)(GLsizei, const GLuint*);
  void(TS_GLAPI* GenRenderbuffers)(GLsizei, GLuint*);
  void(TS_GLAPI* BindRenderbuffer)(GLenum, GLuint);
  void(TS_GLAPI* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
  void(TS_GLAPI* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
  void(TS_GLAPI* DeleteRenderbuffers)(GLsizei, const GLuint*);
  void(TS_GLAPI* ActiveTexture)(GLenum);
  void(TS_GLAPI* GenerateMipmap)(GLenum);
};

// Load once per process (needs a current GL 3.3 context). False with the
// first missing entry point.
bool load_gl33(std::string* error);
const Gl33& gl33();

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
