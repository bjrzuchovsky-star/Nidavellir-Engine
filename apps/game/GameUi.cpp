#include "GameUi.h"

#include "GameRenderer.h"
#include "audio/Audio.h"
#include "audio/MixerSettings.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace ts {
namespace tombstone {
namespace game {

namespace {

GameUi* g_char_target = nullptr;

const std::string& atlas_key() {
  static const std::string k = ui::kAtlasKey;
  return k;
}

// Edge slots in GameUi::prev_.
enum Edge { kUp, kDown, kLeft, kRight, kAccept, kBack, kBackspace, kTab,
            kPadUp, kPadDown, kPadLeft, kPadRight, kPadA, kPadB, kPadStart, kEdgeCount };

std::uint64_t hash_draws(const std::vector<ui::UiDraw>& d, int w, int h) {
  std::uint64_t x = 1469598103934665603ull ^ static_cast<std::uint64_t>(w * 7919 + h);
  auto mix = [&x](const void* p, std::size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) x = (x ^ b[i]) * 1099511628211ull;
  };
  for (const ui::UiDraw& u : d) {
    mix(&u.quad, sizeof(u.quad));
    mix(u.image.data(), u.image.size());
  }
  return x;
}

}  // namespace

void game_ui_char(GLFWwindow*, unsigned int cp) {
  if (g_char_target && cp >= 32 && cp < 127) g_char_target->typed_.push_back(static_cast<char>(cp));
}

GameUi::GameUi() { ui::PauseMenu::install(ui_); }

GameUi::~GameUi() {
  if (g_char_target == this) g_char_target = nullptr;
}

void GameUi::load(const std::string& project_dir) {
  ui_.clear();
  std::string errors;
  const std::string dir = (std::filesystem::path(project_dir) / "ui").string();
  const int n = ui_.load_dir(dir, &errors);
  if (!errors.empty()) std::cerr << "[ui] " << errors;
  if (n > 0) std::cout << "[ui] " << n << " document(s) from ui/\n";
  ui::PauseMenu::install(ui_);
}

void GameUi::attach(GLFWwindow* win) {
  g_char_target = this;
  glfwSetCharCallback(win, game_ui_char);
}

void GameUi::detach(GLFWwindow* win) {
  if (g_char_target == this) g_char_target = nullptr;
  glfwSetCharCallback(win, nullptr);
}

void GameUi::open_pause(ts::audio::Engine* a) {
  using ts::audio::Bus;
  pause_.open(ui_, a ? a->bus_volume(Bus::Master) : 1.0f, a ? a->bus_volume(Bus::Music) : 1.0f,
              a ? a->bus_volume(Bus::Sfx) : 1.0f);
}

void GameUi::close_pause(ts::audio::Engine* a) {
  pause_.close(ui_);
  if (settings_dirty_ && a) {
    std::string err;
    if (!ts::audio::save_mixer_settings(ts::audio::default_mixer_settings_path(),
                                    ts::audio::capture_mixer_settings(*a), &err)) {
      std::cerr << "[ui] could not save volumes: " << err << '\n';
    }
  }
  settings_dirty_ = false;
}

void GameUi::toast(const std::string& text, double seconds) {
  toasts_.push_back({text, std::max(0.5, seconds)});
  if (toasts_.size() > 6) toasts_.erase(toasts_.begin());
}

GameUi::Frame GameUi::update(GLFWwindow* win, double dt, ts::audio::Engine* a) {
  Frame f;
  int ww = 0, wh = 0, fw = 0, fh = 0;
  glfwGetWindowSize(win, &ww, &wh);
  glfwGetFramebufferSize(win, &fw, &fh);
  if (fw <= 0 || fh <= 0 || ww <= 0 || wh <= 0) return f;
  width_ = static_cast<float>(fixed_w_ > 0 ? fixed_w_ : fw);
  height_ = static_cast<float>(fixed_h_ > 0 ? fixed_h_ : fh);
  for (Toast& t : toasts_) t.left -= dt;
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                               [](const Toast& t) { return t.left <= 0; }),
                toasts_.end());

  bool now[kEdgeCount] = {};
  auto key = [win](int k) { return glfwGetKey(win, k) == GLFW_PRESS; };
  now[kUp] = key(GLFW_KEY_UP);
  now[kDown] = key(GLFW_KEY_DOWN);
  now[kLeft] = key(GLFW_KEY_LEFT);
  now[kRight] = key(GLFW_KEY_RIGHT);
  now[kAccept] = key(GLFW_KEY_ENTER) || key(GLFW_KEY_KP_ENTER);
  now[kBack] = key(GLFW_KEY_ESCAPE);
  now[kBackspace] = key(GLFW_KEY_BACKSPACE);
  now[kTab] = key(GLFW_KEY_TAB);
  GLFWgamepadstate pad;
  if (glfwGetGamepadState(GLFW_JOYSTICK_1, &pad)) {
    now[kPadUp] = pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] || pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.6f;
    now[kPadDown] = pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] || pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] > 0.6f;
    now[kPadLeft] = pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] || pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.6f;
    now[kPadRight] = pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] || pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] > 0.6f;
    now[kPadA] = pad.buttons[GLFW_GAMEPAD_BUTTON_A];
    now[kPadB] = pad.buttons[GLFW_GAMEPAD_BUTTON_B];
    now[kPadStart] = pad.buttons[GLFW_GAMEPAD_BUTTON_START];
  }
  bool edge[kEdgeCount];
  for (int i = 0; i < kEdgeCount; ++i) {
    edge[i] = now[i] && !prev_[i];
    prev_[i] = now[i];
  }

  ui::UiInput in;
  in.dt = dt;
  double mx = 0, my = 0;
  glfwGetCursorPos(win, &mx, &my);
  in.mouse_x = static_cast<float>(mx * fw / ww);
  in.mouse_y = static_cast<float>(my * fh / wh);
  in.mouse_down = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
  if (fixed_w_ > 0) {
    in.mouse_x = in.mouse_y = -1.0f;  // the window is only a mirror
    in.mouse_down = false;
  }
  if (virtual_) {
    in.mouse_x = vx_;
    in.mouse_y = vy_;
    in.mouse_down = vdown_;
  }

  const bool text = ui_.wants_text();
  const bool modal = ui_.modal_open();
  // Keyboard navigation only while a menu or text box owns the keys (the
  // arrows still drive the rider otherwise); the pad's d-pad likewise.
  if (modal || text) {
    in.up = edge[kUp] || edge[kPadUp];
    in.down = edge[kDown] || edge[kTab] || edge[kPadDown];
    in.left = edge[kLeft] || edge[kPadLeft];
    in.right = edge[kRight] || edge[kPadRight];
    in.accept = edge[kAccept] || edge[kPadA];
    in.backspace = edge[kBackspace];
  }
  if (text) {
    in.text.swap(typed_);
  }
  typed_.clear();
  const bool back = edge[kBack] || edge[kPadB];
  if ((edge[kPadStart] || edge[kBack]) && !modal && !text) {
    open_pause(a);
  } else if (back && text) {
    ui_.set_focus(ui_.focus_document(), "");
  } else if (back && modal) {
    in.back = true;
  } else if (edge[kPadStart] && pause_open()) {
    close_pause(a);
  }

  f.events = ui_.update(in, width_, height_);
  for (const ui::UiEvent& e : f.events) {
    std::string bus;
    float v = 0;
    switch (pause_.handle(ui_, e, &bus, &v)) {
      case ui::PauseMenu::Action::Resume:
        close_pause(a);
        break;
      case ui::PauseMenu::Action::Quit:
        close_pause(a);
        f.quit = true;
        break;
      case ui::PauseMenu::Action::Volume: {
        ts::audio::Bus b;
        if (a && ts::audio::parse_bus(bus, &b)) {
          a->set_bus_volume(b, v);
          settings_dirty_ = true;
        }
        break;
      }
      case ui::PauseMenu::Action::None:
        break;
    }
  }
  return f;
}

bool GameUi::anything_visible() const {
  if (!toasts_.empty()) return true;
  for (const std::string& n : ui_.names()) {
    if (ui_.visible(n)) return true;
  }
  return false;
}

void GameUi::draws(std::vector<ui::UiDraw>* out) const {
  ui_.draw(out);
  if (toasts_.empty()) return;
  // Toast cards stacked up from the bottom centre, newest lowest.
  const ui::FontAtlas& atlas = ui_.atlas();
  const float s = ui::ui_scale(width_, height_);
  const float px = 20.0f * s;
  const float lh = atlas.line_height(ui::FontFace::Body, px);
  const ui::Theme& t = ui_.theme;
  float y = height_ - 40.0f * s;
  for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
    const float a = static_cast<float>(std::min(1.0, it->left * 2.0));
    const float tw = atlas.measure(it->text, ui::FontFace::Body, px);
    const float pad = 14.0f * s;
    const float x0 = (width_ - tw) * 0.5f - pad, x1 = (width_ + tw) * 0.5f + pad;
    const float y1 = y, y0 = y - lh - pad;
    auto solid = [&](float ax, float ay, float bx, float by, const float* c, float alpha) {
      ui::UiDraw d;
      d.quad = {ax, ay, bx, by, atlas.white_u(), atlas.white_v(), atlas.white_u(), atlas.white_v(),
                {c[0], c[1], c[2], c[3] * alpha}};
      out->push_back(d);
    };
    solid(x0, y0, x1, y1, t.panel, a);
    solid(x0, y0, x1, y0 + 2 * s, t.accent, a);
    std::vector<ui::UiQuad> q;
    const float col[4] = {t.text[0], t.text[1], t.text[2], a};
    atlas.text_quads(it->text, ui::FontFace::Body, px, std::round((width_ - tw) * 0.5f),
                     std::round(y0 + pad * 0.5f), col, &q);
    for (const ui::UiQuad& g : q) {
      ui::UiDraw d;
      d.quad = g;
      out->push_back(d);
    }
    y = y0 - 8.0f * s;
  }
}

void GameUi::render_gl(GameRenderer& renderer, int fw, int fh) {
  std::vector<ui::UiDraw> d;
  draws(&d);
  if (d.empty()) return;
  quads_.clear();
  images_.clear();
  for (const ui::UiDraw& u : d) {
    runtime::DrawQuad q;
    q.x0 = u.quad.x0;
    q.y0 = u.quad.y0;
    q.x1 = u.quad.x1;
    q.y1 = u.quad.y1;
    q.u0 = u.quad.u0;
    q.v0 = u.quad.v0;
    q.u1 = u.quad.u1;
    q.v1 = u.quad.v1;
    std::memcpy(q.rgba, u.quad.rgba, sizeof(q.rgba));
    if (u.image.empty()) {
      q.image = &atlas_key();
    } else {
      images_.push_back(u.image);
      q.image = &images_.back();
    }
    quads_.push_back(q);
  }
  renderer.render_screen(fw, fh, quads_);
}

const std::vector<unsigned char>& GameUi::raster(int fw, int fh, std::uint64_t* version) {
  std::vector<ui::UiDraw> d;
  draws(&d);
  const std::uint64_t h = hash_draws(d, fw, fh);
  if (h != raster_hash_ || fw != raster_w_ || fh != raster_h_) {
    raster_hash_ = h;
    raster_w_ = fw;
    raster_h_ = fh;
    if (d.empty()) {
      raster_.clear();
    } else {
      ui::raster(d, ui_.atlas(), fw, fh, &raster_);
    }
    ++raster_version_;
  }
  if (version) *version = raster_version_;
  return raster_;
}

}  // namespace game
}  // namespace tombstone
}  // namespace ts
