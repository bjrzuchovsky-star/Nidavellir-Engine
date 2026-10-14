#pragma once

// One RGBA8 + depth framebuffer per eye (ts_xr internal). Needs a current
// GL 3.3 context with render3d::load_gl33() done.

#include "render3d/GlApi.h"

#include <cstdint>

namespace ts {
namespace tombstone {
namespace xr {

struct EyeTarget {
  std::uint32_t fbo = 0, color = 0, depth = 0;
  int width = 0, height = 0;

  bool create(int w, int h) {
    const render3d::Gl33& g = render3d::gl33();
    GLint prev_fbo = 0, prev_tex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    width = w;
    height = h;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLuint rb = 0;
    g.GenRenderbuffers(1, &rb);
    g.BindRenderbuffer(GL_RENDERBUFFER, rb);
    g.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    GLuint fb = 0;
    g.GenFramebuffers(1, &fb);
    g.BindFramebuffer(GL_FRAMEBUFFER, fb);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    g.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    const bool ok = g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    fbo = fb;
    color = tex;
    depth = rb;
    if (!ok) destroy();
    return ok;
  }

  void destroy() {
    const render3d::Gl33& g = render3d::gl33();
    if (fbo) {
      const GLuint f = fbo;
      g.DeleteFramebuffers(1, &f);
    }
    if (depth) {
      const GLuint d = depth;
      g.DeleteRenderbuffers(1, &d);
    }
    if (color) {
      const GLuint c = color;
      glDeleteTextures(1, &c);
    }
    fbo = color = depth = 0;
    width = height = 0;
  }

  void bind() const {
    render3d::gl33().BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
  }
};

}  // namespace xr
}  // namespace tombstone
}  // namespace ts
