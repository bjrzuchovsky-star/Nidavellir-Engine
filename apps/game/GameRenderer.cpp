#include "GameRenderer.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
// GLFW pulls the platform OpenGL header portably (Windows needs its
// APIENTRY / WINGDIAPI first; GLFW handles that).
#include <GLFW/glfw3.h>

#include <utility>

namespace ts {
namespace tombstone {
namespace game {

namespace {

// Dusk over the badlands: the clear colour outside the scene.
constexpr float kClear[4] = {0.075f, 0.067f, 0.063f, 1.0f};

void solid_quad(float x0, float y0, float x1, float y1) {
  glBegin(GL_QUADS);
  glVertex2f(x0, y0);
  glVertex2f(x1, y0);
  glVertex2f(x1, y1);
  glVertex2f(x0, y1);
  glEnd();
}

// Missing sprite image: solid stand-in plus a red cross so it reads as a
// problem, not as art.
void missing_cross(float x0, float y0, float x1, float y1) {
  glColor4f(0.86f, 0.24f, 0.18f, 1.0f);
  glBegin(GL_LINES);
  glVertex2f(x0, y0);
  glVertex2f(x1, y1);
  glVertex2f(x1, y0);
  glVertex2f(x0, y1);
  glEnd();
}

// Overlay colours, matching the editor's K overlay (Danger, Copper,
// Success, Warning).
void overlay_color(runtime::OverlayKind kind, float alpha) {
  switch (kind) {
    case runtime::OverlayKind::SolidTile:
      glColor4f(0.808f, 0.290f, 0.251f, alpha);
      break;
    case runtime::OverlayKind::StaticSolid:
      glColor4f(0.722f, 0.384f, 0.220f, alpha);
      break;
    case runtime::OverlayKind::DynamicSolid:
      glColor4f(0.541f, 0.690f, 0.408f, alpha);
      break;
    case runtime::OverlayKind::Trigger:
      glColor4f(0.925f, 0.690f, 0.337f, alpha);
      break;
  }
}

void outline(float x0, float y0, float x1, float y1) {
  glBegin(GL_LINE_LOOP);
  glVertex2f(x0, y0);
  glVertex2f(x1, y0);
  glVertex2f(x1, y1);
  glVertex2f(x0, y1);
  glEnd();
}

}  // namespace

GameRenderer::GameRenderer(std::string project_dir)
    : project_dir_(std::move(project_dir)) {}

const gfx::TextureInfo& GameRenderer::texture(const std::string& rel_path) {
  return textures_.get(runtime::resolve_asset(project_dir_, rel_path));
}

bool GameRenderer::image_size(const std::string& rel_path, int* width,
                              int* height) {
  const gfx::TextureInfo& t = texture(rel_path);
  if (!t.ok) {
    return false;
  }
  *width = t.width;
  *height = t.height;
  return true;
}

void GameRenderer::render(const runtime::WorldRect& view, int framebuffer_w,
                          int framebuffer_h,
                          const std::vector<runtime::DrawQuad>& quads) {
  glViewport(0, 0, framebuffer_w, framebuffer_h);
  glClearColor(kClear[0], kClear[1], kClear[2], kClear[3]);
  glClear(GL_COLOR_BUFFER_BIT);

  // World space straight onto the framebuffer, y down like the editor.
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(view.x0, view.x1, view.y1, view.y0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  draw_quads(quads);
}

void GameRenderer::render_screen(int framebuffer_w, int framebuffer_h,
                                 const std::vector<runtime::DrawQuad>& quads) {
  glViewport(0, 0, framebuffer_w, framebuffer_h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0, framebuffer_w, framebuffer_h, 0.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  draw_quads(quads);
}

void GameRenderer::draw_quads(const std::vector<runtime::DrawQuad>& quads) {
  std::uint8_t blend = 0;
  auto set_blend = [&](std::uint8_t mode) {
    if (mode == blend) return;
    blend = mode;
    if (mode == 1) {
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);            // glowing particles
    } else if (mode == 2) {
      glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);      // lightmap: dst * src * 2
    } else {
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
  };
  for (const runtime::DrawQuad& q : quads) {
    set_blend(q.blend);
    const std::uint64_t handle = q.image ? texture(*q.image).handle : 0;
    glColor4f(q.rgba[0], q.rgba[1], q.rgba[2], q.rgba[3]);
    const bool turned = q.rotation != 0.0f;
    if (turned) {
      // Clockwise on screen (y down) about the quad centre.
      const float cx = (q.x0 + q.x1) * 0.5f;
      const float cy = (q.y0 + q.y1) * 0.5f;
      glPushMatrix();
      glTranslatef(cx, cy, 0.0f);
      glRotatef(q.rotation, 0.0f, 0.0f, 1.0f);
      glTranslatef(-cx, -cy, 0.0f);
    }
    if (handle != 0) {
      glEnable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(handle));
      if (q.image->rfind("mem:fx/", 0) == 0) {
        // Lightmap and soft particles: smooth, not pixel-art crisp.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }
      glBegin(GL_QUADS);
      glTexCoord2f(q.u0, q.v0);
      glVertex2f(q.x0, q.y0);
      glTexCoord2f(q.u1, q.v0);
      glVertex2f(q.x1, q.y0);
      glTexCoord2f(q.u1, q.v1);
      glVertex2f(q.x1, q.y1);
      glTexCoord2f(q.u0, q.v1);
      glVertex2f(q.x0, q.y1);
      glEnd();
      glBindTexture(GL_TEXTURE_2D, 0);
      glDisable(GL_TEXTURE_2D);
    } else {
      solid_quad(q.x0, q.y0, q.x1, q.y1);
      if (q.missing) {
        missing_cross(q.x0, q.y0, q.x1, q.y1);
      }
    }
    if (turned) glPopMatrix();
  }
  set_blend(0);
}

void GameRenderer::render_overlay(
    const std::vector<runtime::OverlayBox>& boxes) {
  glDisable(GL_TEXTURE_2D);
  // Washes first so every outline sits on top.
  for (const runtime::OverlayBox& b : boxes) {
    const bool lit = b.kind == runtime::OverlayKind::Trigger && b.active;
    overlay_color(b.kind, lit ? 0.35f : 0.14f);
    solid_quad(b.x0, b.y0, b.x1, b.y1);
  }
  glLineWidth(2.0f);
  for (const runtime::OverlayBox& b : boxes) {
    overlay_color(b.kind, 0.95f);
    outline(b.x0, b.y0, b.x1, b.y1);
    if (b.kind == runtime::OverlayKind::Trigger) {
      glBegin(GL_LINES);
      glVertex2f(b.x0, b.y0);
      glVertex2f(b.x1, b.y1);
      glVertex2f(b.x1, b.y0);
      glVertex2f(b.x0, b.y1);
      glEnd();
    }
  }
  glLineWidth(1.0f);
}

}  // namespace game
}  // namespace tombstone
}  // namespace ts
