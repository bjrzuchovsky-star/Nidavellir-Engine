#pragma once

// The in-game UI for the GLFW hosts (ts_game 2D / 3D, ts_client): loads
// <project>/ui/*.ui.json, polls mouse / keyboard / gamepad into ts_ui,
// runs the pause menu (Esc / pad Start: Resume, Settings with master /
// music / sfx volume sliders saved to TombStoneConfig/audio.json, Quit),
// keeps 3D toasts, and hands the result to a renderer: fixed-function GL
// quads (2D) or an RGBA raster for Renderer3D::set_overlay (3D).

#include "runtime/World.h"
#include "ui/Ui.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct GLFWwindow;

namespace ts {
namespace audio {
class Engine;
}
namespace tombstone {
namespace game {

class GameRenderer;

class GameUi {
 public:
  GameUi();
  ~GameUi();
  GameUi(const GameUi&) = delete;
  GameUi& operator=(const GameUi&) = delete;

  // (Re)load the project's documents; the built-in pause menu unless the
  // project ships ui/pause.ui.json.
  void load(const std::string& project_dir);
  ui::UiSystem& system() { return ui_; }
  // Typed characters for text inputs come through GLFW's char callback.
  void attach(GLFWwindow* win);
  void detach(GLFWwindow* win);

  struct Frame {
    std::vector<ui::UiEvent> events;  // forward to the world (ui.on_click ...)
    bool quit = false;                // pause menu Quit
  };
  // Poll input, run the UI in framebuffer pixels, run the pause menu
  // (volumes go to `audio` when given).
  Frame update(GLFWwindow* win, double dt, ts::audio::Engine* audio);

  bool pause_open() const { return ui_.visible("pause"); }
  // Game input should be held back (a modal is up or a text box is focused).
  bool wants_input() const { return ui_.wants_input(); }
  void open_pause(ts::audio::Engine* audio);
  void close_pause(ts::audio::Engine* audio);

  // Toasts drawn as text cards (the 3D game; 2D keeps HudText).
  void toast(const std::string& text, double seconds);

  // Everything to draw this frame (documents, then toasts).
  void draws(std::vector<ui::UiDraw>* out) const;
  // 2D: fixed-function quads through the game renderer.
  void render_gl(GameRenderer& renderer, int fw, int fh);
  // 3D: the frame rasterised to RGBA8 (fw x fh), redone only when the
  // draw list changes. version bumps on every change.
  const std::vector<unsigned char>& raster(int fw, int fh, std::uint64_t* version);

 private:
  ui::UiSystem ui_;
  ui::PauseMenu pause_;
  bool settings_dirty_ = false;
  struct Toast {
    std::string text;
    double left = 0.0;
  };
  std::vector<Toast> toasts_;
  float width_ = 1280.0f, height_ = 720.0f;
  // Key / pad edges.
  bool prev_[16] = {};
  std::string typed_;
  // Draw conversion / raster caches.
  std::deque<std::string> images_;
  std::vector<runtime::DrawQuad> quads_;
  std::vector<unsigned char> raster_;
  std::uint64_t raster_hash_ = 0;
  std::uint64_t raster_version_ = 0;
  int raster_w_ = 0, raster_h_ = 0;

  friend void game_ui_char(GLFWwindow*, unsigned int);
};

}  // namespace game
}  // namespace tombstone
}  // namespace ts
