#pragma once

// The in-game UI inside the editor: Play (2D and 3D viewports) runs the
// project's ui/*.ui.json documents over the game view with mouse and
// (while a menu or text box owns them) keyboard navigation, and Lua ui.*
// flows both ways. The UI Preview panel uses draw_ui_draws() as well.

#include "gfx/TextureCache.h"
#include "ui/Ui.h"

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Texture for a UiDraw image ("" = the font atlas); 0 = skip the quad.
using UiTextureFn = std::function<ImTextureID(const std::string& image)>;

// Draw `draws` (pixels relative to `origin`) into `draw`.
void draw_ui_draws(ImDrawList* draw, const std::vector<ui::UiDraw>& draws, const ImVec2& origin,
                   const UiTextureFn& texture);

class PlayUi {
 public:
  void load(const std::string& project_dir);
  void clear() { ui_.clear(); }
  ui::UiSystem& system() { return ui_; }
  const ui::UiSystem& system() const { return ui_; }

  // Input from ImGui (mouse while `hovered`, keys while `focused`), layout
  // for the canvas size. Returns the widget events for the world.
  std::vector<ui::UiEvent> update(const ImVec2& origin, const ImVec2& size, bool hovered,
                                  bool focused, double dt);
  // Textures come from the PlayUi's own cache (atlas, project images).
  void draw(ImDrawList* draw, const ImVec2& origin);
  ImTextureID texture(const std::string& image);
  // A menu or text box has the keys: hold the game's keyboard input.
  bool wants_input() const { return ui_.wants_input(); }

 private:
  ui::UiSystem ui_;
  std::string project_dir_;
  gfx::TextureCache textures_;
};

// View > UI Preview: pick one of the project's ui/*.ui.json documents and
// see it live (16:9 frame, clickable) without entering Play.
class UiPreview {
 public:
  void draw(bool* open, const std::string& project_dir);

 private:
  PlayUi ui_;
  std::string dir_;
  std::string selected_;
  std::string last_event_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
