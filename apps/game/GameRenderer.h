#pragma once

// ts_game's renderer: a runtime::World draw list through fixed-function
// OpenGL (GL 1.1 immediate mode, so no loader). Textures come from the
// shared gfx::TextureCache via gfx::GlTextureUploader.

#include "gfx/TextureCache.h"
#include "runtime/World.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace game {

class GameRenderer {
 public:
  explicit GameRenderer(std::string project_dir);

  // Image size for draw-list UVs (decodes on first use, cached).
  bool image_size(const std::string& rel_path, int* width, int* height);

  // Clears the framebuffer and draws `quads`, mapping `view` (world space)
  // onto the whole framebuffer.
  void render(const runtime::WorldRect& view, int framebuffer_w,
              int framebuffer_h, const std::vector<runtime::DrawQuad>& quads);

  // K overlay, drawn over the last render() with the same view: solid
  // tiles red, static solids copper, dynamic bodies green, triggers gold
  // with a cross (shaded while somebody is inside).
  void render_overlay(const std::vector<runtime::OverlayBox>& boxes);

  // Screen-space quads (framebuffer pixels, y down) over whatever is on the
  // framebuffer: the in-game UI. Images may be "mem:" keys (UI atlas).
  void render_screen(int framebuffer_w, int framebuffer_h,
                     const std::vector<runtime::DrawQuad>& quads);

  // Re-decode images changed on disk (throttled by the cache).
  void poll_changes(double now) { textures_.poll_changes(now); }
  void clear() { textures_.clear(); }

 private:
  const gfx::TextureInfo& texture(const std::string& rel_path);
  void draw_quads(const std::vector<runtime::DrawQuad>& quads);

  std::string project_dir_;
  gfx::TextureCache textures_;
};

}  // namespace game
}  // namespace tombstone
}  // namespace ts
