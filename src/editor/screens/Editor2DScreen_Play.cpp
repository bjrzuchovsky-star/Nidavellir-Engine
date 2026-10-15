#include "editor/screens/Editor2DScreen.h"
#include "plugins/profiler/ProfilerPlugin.h"
#include "profiler/Profiler.h"
#include "ui/UiScript.h"
#include "editor/audio/EditorAudio.h"

#include "editor/launch/GameLauncher.h"
#include "editor/ui/QuadBlend.h"
#include "editor/ui/QuadDraw.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

// Gamepads come straight from GLFW (the admin's window backend); keyboard
// goes through ImGui. Only the GUI path calls into GLFW.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

ImU32 quad_u32(const float c[4]) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
}

ImTextureID play_tex_id(std::uint64_t handle) {
  return (ImTextureID)(std::uintptr_t)handle;
}

// "01:02.34"
std::string clock_text(double seconds) {
  const int total_cs = static_cast<int>(seconds * 100.0);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d.%02d", total_cs / 6000,
                (total_cs / 100) % 60, total_cs % 100);
  return buf;
}

// Amber "!" flag: sprite image missing (same as the edit viewport).
void missing_flag(ImDrawList* draw, const ImVec2& p0, const ImVec2& p1) {
  const float s = 14.0f;
  const ImVec2 a(p1.x - s - 2.0f, p0.y + 2.0f);
  const ImVec2 tip(a.x + s * 0.5f, a.y);
  const ImVec2 bl(a.x, a.y + s);
  const ImVec2 br(a.x + s, a.y + s);
  draw->AddTriangleFilled(tip, br, bl, theme::U32(theme::Warning()));
  draw->AddText(ImVec2(tip.x - 2.0f, a.y + 1.0f),
                theme::U32(theme::Charcoal()), "!");
}

float deadzone(float v) {
  constexpr float kDead = 0.2f;
  if (std::fabs(v) < kDead) {
    return 0.0f;
  }
  return (v - std::copysign(kDead, v)) / (1.0f - kDead);
}

}  // namespace

// --- Session control ------------------------------------------------------------

bool Editor2DScreen::start_play() {
  if (is_playing()) {
    return true;
  }
  // Land whatever edit is in flight as its own undo step first.
  cancel_rename();
  if (stroke_active()) {
    end_paint_stroke();
  }
  if (workspace_.move_active()) {
    workspace_.cancel_move();
  }
  drag_mode_ = DragMode::None;
  panning_ = false;
  flush_pending_edit();
  if (nudge_pending_save_ || dirty_) {
    nudge_pending_save_ = false;
    mark_dirty_and_autosave();
  }
  std::string err;
  // Plugin systems and Lua tables ride along (set before the world builds).
  play_.world().set_extensions(plugins_.manager().world_extensions());
  if (!play_.start(workspace_.entities(), workspace_.tile_solidity(),
                   project_.path, &err)) {
    play_.world().set_extensions(nullptr);
    note(err.empty() ? std::string("Could not start the ride.") : err);
    return false;
  }
  play_.world().set_lighting(workspace_.lighting());
  plugins_.manager().play_begin(play_.world());
  begin_sound_ride();
  play_ui_.load(project_.path);
  debug_tools_on_ = plugins_.loaded("ts.profiler");
  if (debug_tools_on_) {
    profiler::Profiler::get().set_capture_dir(plugins::capture_dir_for(project_.path));
    debug_tools_.attach(play_ui_.system());
  }
  // The edit scene waits here, untouched, until Stop.
  play_backup_ = workspace_;
  play_free_cam_ = false;
  play_focus_viewport_ = true;
  const runtime::CameraView cam = play_.world().camera();
  play_cam_x_ = cam.x;
  play_cam_y_ = cam.y;
  play_cam_zoom_ = cam.zoom;
  play_scripts_poll_ = now_seconds();
  if (play_.world().player_count() == 0) {
    note("Riding. Nobody in the saddle: give an entity a Player component.");
  } else {
    note("Riding. WASD / arrows to move, E / Space action, F6 pause, F5 stop.");
  }
  pump_play_logs();
  std::cout << "[Editor2D] play: " << play_.world().actors().size()
            << " actors, " << play_.world().player_count() << " rider(s)\n";
  return true;
}

void Editor2DScreen::stop_play() {
  if (!is_playing() && !play_backup_) {
    return;
  }
  const std::uint64_t ticks = play_.tick();
  pump_play_logs();  // the last words of the ride
  plugins_.manager().play_end();
  end_sound_ride();
  if (debug_tools_.attached()) debug_tools_.detach();
  debug_tools_on_ = false;
  play_ui_.clear();
  play_.stop();
  play_.world().set_extensions(nullptr);
  if (play_backup_) {
    workspace_ = std::move(*play_backup_);
    play_backup_.reset();
  }
  play_free_cam_ = false;
  drag_mode_ = DragMode::None;
  panning_ = false;
  edit_source_ = EditSource::None;
  insp_frame_valid_ = false;
  insp_drag_slot_ = -1;
  hover_cell_valid_ = false;
  note("Back at camp after " + std::to_string(ticks) +
       " ticks. Edit scene untouched.");
  apply_plugin_changes_if_idle();
}

void Editor2DScreen::toggle_play() {
  EditorAudio::get().click();
  if (is_playing()) {
    stop_play();
  } else {
    start_play();
  }
}

void Editor2DScreen::toggle_pause_play() {
  if (!is_playing()) {
    return;
  }
  play_.toggle_pause();
  note(play_.paused() ? "Holding up. F6 rides on, F10 steps one tick."
                      : "Riding on.");
}

bool Editor2DScreen::step_play() {
  if (!play_.paused()) {
    return false;
  }
  const runtime::InputFrame input = ImGui::GetCurrentContext() != nullptr
                                        ? poll_play_input()
                                        : runtime::InputFrame{};
  const bool stepped = play_.step_once(input);
  pump_play_logs();
  return stepped;
}

int Editor2DScreen::update_play(double real_seconds,
                                const runtime::InputFrame& input) {
  const int ticks = play_.update(real_seconds, play_ui_.wants_input() ? runtime::InputFrame{} : input);
  ui::apply_ui_commands(play_ui_.system(), play_.world().take_ui_commands());
  if (sound_stage_ && is_playing()) {
    // Ears ride with the camera the viewport shows.
    runtime::CameraView cam = play_.world().camera(play_.alpha());
    if (play_free_cam_) {
      cam.x = play_cam_x_;
      cam.y = play_cam_y_;
    }
    sound_stage_->update_2d(play_.world(), cam.x, cam.y);
  }
  // Saved scripts ride back in twice a second (a broken save keeps the old
  // version and says why on the Telegraph).
  const double now = now_seconds();
  if (is_playing() && now - play_scripts_poll_ >= 0.5) {
    play_scripts_poll_ = now;
    reload_play_scripts();
  }
  pump_play_logs();
  return ticks;
}

void Editor2DScreen::begin_sound_ride() {
  end_sound_ride();
  sound_stage_ = std::make_unique<audio::SoundStage>(EditorAudio::get().engine());
  sound_stage_->begin_ride(project_.path, false);
}

void Editor2DScreen::end_sound_ride() {
  if (!sound_stage_) return;
  sound_stage_->end_ride();
  sound_stage_.reset();
}

void Editor2DScreen::set_play_free_camera(bool on) {
  if (on && !play_free_cam_ && is_playing()) {
    const runtime::CameraView cam = play_.world().camera(play_.alpha());
    play_cam_x_ = cam.x;
    play_cam_y_ = cam.y;
    play_cam_zoom_ = cam.zoom;
  }
  play_free_cam_ = on;
}

bool Editor2DScreen::launch_game() {
  if (is_playing()) {
    note("Stop play first; ts_game reads the saved scene.");
    return false;
  }
  flush_pending_edit();
  std::string err;
  if (!project_.path.empty() && !save_scene(&err)) {
    note("Launch called off: " + err);
    return false;
  }
  const std::string exe = launcher::find_game_executable();
  if (exe.empty()) {
    note("ts_game isn't built. Configure with -DTS_BUILD_GAME=ON, build "
         "ts_game, then launch again.");
    return false;
  }
  if (!launcher::spawn_game(exe, project_.path, &err)) {
    note("ts_game would not start: " + err);
    return false;
  }
  note("ts_game is saddling up for " + project_.name + ".");
  std::cout << "[Editor2D] launched " << exe << " --project " << project_.path
            << '\n';
  return true;
}

// --- Input ----------------------------------------------------------------------

runtime::InputFrame Editor2DScreen::poll_play_input() const {
  runtime::InputFrame frame;
  const ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput) {
    auto down = [](ImGuiKey a, ImGuiKey b) {
      return ImGui::IsKeyDown(a) || ImGui::IsKeyDown(b);
    };
    runtime::PlayerInput keys = runtime::digital_input(
        down(ImGuiKey_A, ImGuiKey_LeftArrow),
        down(ImGuiKey_D, ImGuiKey_RightArrow),
        down(ImGuiKey_W, ImGuiKey_UpArrow),
        down(ImGuiKey_S, ImGuiKey_DownArrow));
    // Action (E / Space / pad A): talk to, open or pick up what is nearby.
    if (ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_Space)) {
      keys.buttons |= runtime::kButtonAction;
    }
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) keys.buttons |= runtime::kButtonAlt;
    if (ImGui::IsKeyDown(ImGuiKey_Enter)) keys.buttons |= runtime::kButtonStart;
    // Frontier Survival: F / left mouse use, G / right mouse place, C fire,
    // Q / X previous / next hotbar slot, 1-0 pick a slot.
    if (ImGui::IsKeyDown(ImGuiKey_F)) keys.buttons |= runtime::kButtonUse;
    if (ImGui::IsKeyDown(ImGuiKey_G)) keys.buttons |= runtime::kButtonPlace;
    if (ImGui::IsKeyDown(ImGuiKey_C)) keys.buttons |= runtime::kButtonFire;
    if (ImGui::IsKeyDown(ImGuiKey_X)) keys.buttons |= runtime::kButtonNext;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) keys.buttons |= runtime::kButtonPrev;
    frame.slot(0) = keys;
  }
  // Gamepads 1-4 ride slots 0-3 (slot 0 shares with the keyboard).
  if (glfwGetCurrentContext() != nullptr) {
    for (int slot = 0; slot < kMaxPlayers; ++slot) {
      GLFWgamepadstate pad;
      if (!glfwGetGamepadState(GLFW_JOYSTICK_1 + slot, &pad)) {
        continue;
      }
      runtime::PlayerInput in;
      in.connected = true;
      in.move_x = deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
      in.move_y = deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
      if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT]) in.move_x = -1.0f;
      if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]) in.move_x = 1.0f;
      if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP]) in.move_y = -1.0f;
      if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN]) in.move_y = 1.0f;
      if (pad.buttons[GLFW_GAMEPAD_BUTTON_A]) in.buttons |= runtime::kButtonAction;
      if (pad.buttons[GLFW_GAMEPAD_BUTTON_B]) in.buttons |= runtime::kButtonAlt;
      if (pad.buttons[GLFW_GAMEPAD_BUTTON_START]) in.buttons |= runtime::kButtonStart;
      frame.slot(slot) = runtime::merge_input(frame.slot(slot), in);
    }
  }
  return frame;
}

// --- Toolbar / menu / status ----------------------------------------------------

void Editor2DScreen::draw_play_controls() {
  if (!is_playing()) {
    if (theme::PrimaryButton("Play", ImVec2(56, 0))) {
      start_play();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Ride the scene in the Viewport (F5 / Ctrl+P). Stop "
                        "puts everything back.");
    }
  } else {
    if (theme::DangerButton("Stop", ImVec2(56, 0))) {
      stop_play();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Stop and restore the edit scene (F5 / Ctrl+P)");
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!is_playing());
  bool paused = is_paused();
  if (theme::ToggleButton("Pause", &paused, ImVec2(56, 0))) {
    toggle_pause_play();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Hold up / ride on (F6)");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!is_paused());
  if (theme::SecondaryButton("Step", ImVec2(50, 0))) {
    step_play();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Advance exactly one tick while paused (F10)");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(is_playing());
  if (theme::CopperButton("Launch", ImVec2(64, 0))) {
    launch_game();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Save, then run this project in ts_game (own window)");
  }
  ImGui::EndDisabled();
  // Plugin toolbar contributions (the posse toggle when it's hitched).
  draw_plugin_menu_items("Toolbar");
}

void Editor2DScreen::draw_play_menu() {
  if (!ImGui::BeginMenu("Play")) {
    return;
  }
  if (ImGui::MenuItem(is_playing() ? "Stop###PlayToggle" : "Play###PlayToggle",
                      "F5 / Ctrl+P")) {
    toggle_play();
  }
  if (ImGui::MenuItem("Pause", "F6", is_paused(), is_playing())) {
    toggle_pause_play();
  }
  if (ImGui::MenuItem("Step One Tick", "F10", false, is_paused())) {
    step_play();
  }
  bool free_cam = play_free_cam_;
  if (ImGui::MenuItem("Free Camera", "C", &free_cam, is_playing())) {
    set_play_free_camera(free_cam);
  }
  bool overlay = show_collision_;
  if (ImGui::MenuItem("Collision Overlay", "K", &overlay)) {
    set_show_collision(overlay);
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Launch Game (ts_game)", nullptr, false, !is_playing())) {
    launch_game();
  }
  draw_plugin_menu_items("Play");
  ImGui::EndMenu();
}

// Posse UI, contributed by the ts.posse plugin (EditorBuiltinPlugins.cpp).
void Editor2DScreen::draw_posse_menu_items() {
  ImGui::Separator();
  if (ImGui::MenuItem(posse_running() ? "Break Camp (stop posse)###Posse" : "Play with Posse###Posse",
                      nullptr, false, !is_playing())) {
    toggle_posse();
  }
  if (ImGui::BeginMenu("Posse Size", !posse_running())) {
    for (int n = 1; n <= mp::kMaxPosse; ++n) {
      char label[32];
      std::snprintf(label, sizeof(label), "%d rider%s", n, n == 1 ? "" : "s");
      if (ImGui::MenuItem(label, nullptr, posse_riders_ == n)) set_posse_riders(n);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Open Client Window (ts_client)", nullptr, false, posse_running())) {
    open_posse_client_window();
  }
  ImGui::MenuItem("Network Panel", nullptr, &show_network_);
}

void Editor2DScreen::draw_posse_toolbar_button() {
  ImGui::SameLine();
  ImGui::BeginDisabled(is_playing());
  bool posse_on = posse_running();
  if (theme::ToggleButton("Posse", &posse_on, ImVec2(60, 0))) {
    toggle_posse();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(posse_running()
                          ? "Break camp: stop the local server and its riders"
                          : "Play with posse: a local server + %d rider(s) in this editor, "
                            "lag sim and Network panel",
                      posse_riders_);
  }
  ImGui::EndDisabled();
}

void Editor2DScreen::draw_play_status() {
  auto sep = []() {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
  };
  sep();
  if (is_paused()) {
    ImGui::TextColored(theme::Warning(), "Holding up (paused)");
  } else {
    ImGui::TextColored(theme::Copper(), "Riding...");
  }
  ImGui::SameLine();
  ImGui::Text("%s", clock_text(play_.play_seconds()).c_str());
  sep();
  ImGui::Text("tick %llu", static_cast<unsigned long long>(play_.tick()));
  sep();
  ImGui::Text("%.0f ticks/s", play_.ticks_per_second() > 0.0
                                  ? play_.ticks_per_second()
                                  : static_cast<double>(runtime::kTickRate));
  sep();
  if (const runtime::Actor* p = play_.world().player(0)) {
    ImGui::Text("P1 %s @ %.0f, %.0f", p->data.name.c_str(), p->data.x,
                p->data.y);
  } else {
    ImGui::TextDisabled("no rider on slot 1");
  }
  sep();
  ImGui::TextDisabled(play_free_cam_ ? "free camera" : "follow camera");
  // Last trigger event (scripts hook these; the Telegraph keeps them all).
  const std::string trig = last_trigger_text();
  if (!trig.empty()) {
    sep();
    ImGui::TextColored(theme::Warning(), "%s", trig.c_str());
  }
}

// --- Viewport while playing -----------------------------------------------------

void Editor2DScreen::draw_play_toasts(ImDrawList* draw, const ImVec2& canvas_pos,
                                      const ImVec2& canvas_size) {
  const runtime::World& world = play_.world();
  const bool tag = world.player_count() > 1;
  const float pad_x = 12.0f;
  const float pad_y = 6.0f;
  float bottom = canvas_pos.y + canvas_size.y - 28.0f;
  const auto& toasts = world.toasts();
  // Newest at the bottom, older ones stacked above it.
  for (auto it = toasts.rbegin(); it != toasts.rend(); ++it) {
    std::string text = it->text;
    if (tag && it->slot >= 0) {
      text = "P" + std::to_string(it->slot + 1) + ": " + text;
    }
    const std::uint64_t left =
        it->until >= world.tick() ? it->until - world.tick() : 0;
    const float a = left >= 30 ? 1.0f : static_cast<float>(left + 1) / 31.0f;
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 p0(canvas_pos.x + (canvas_size.x - text_size.x) * 0.5f - pad_x,
                    bottom - text_size.y - pad_y * 2.0f);
    const ImVec2 p1(p0.x + text_size.x + pad_x * 2.0f, bottom);
    if (p0.y < canvas_pos.y) {
      break;
    }
    draw->AddRectFilled(p0, p1, theme::U32(theme::Charcoal(), 0.9f * a), 3.0f);
    draw->AddRect(p0, p1, theme::U32(theme::Copper(), a), 3.0f, 0, 1.5f);
    draw->AddText(ImVec2(p0.x + pad_x, p0.y + pad_y),
                  theme::U32(theme::Sand(), a), text.c_str());
    bottom = p0.y - 6.0f;
  }
}

void Editor2DScreen::draw_play_viewport() {
  viewport_focused_ =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  handle_collision_hotkey();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  if (is_paused()) {
    ImGui::TextUnformatted(
        "HOLDING UP | F6 ride on | F10 step one tick | F5 / Stop ends the ride");
  } else {
    ImGui::TextUnformatted(
        "RIDING | WASD / arrows move P1 | E / Space action | gamepads P1-P4 | "
        "F6 pause | C free camera | K collision | F5 / Stop ends the ride");
  }
  ImGui::PopStyleColor();

  const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
  ImVec2 canvas_size = ImGui::GetContentRegionAvail();
  canvas_size.x = std::max(canvas_size.x, 32.0f);
  canvas_size.y = std::max(canvas_size.y, 32.0f);
  const ImVec2 canvas_end(canvas_pos.x + canvas_size.x,
                          canvas_pos.y + canvas_size.y);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(canvas_pos, canvas_end, theme::U32(theme::CanvasBg()));
  ImGui::InvisibleButton("##PlayCanvas", canvas_size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle |
                             ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  const ImGuiIO& io = ImGui::GetIO();

  runtime::World& world = play_.world();
  world.set_view_size(canvas_size.x, canvas_size.y);

  // Free camera: drag with any button to pan, wheel to zoom at the cursor.
  if (play_free_cam_ && hovered) {
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ||
        ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
        ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
      play_cam_x_ -= io.MouseDelta.x / play_cam_zoom_;
      play_cam_y_ -= io.MouseDelta.y / play_cam_zoom_;
    }
    if (std::fabs(io.MouseWheel) > 0.0f) {
      const float factor = io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f;
      const float mx = io.MousePos.x - canvas_pos.x - canvas_size.x * 0.5f;
      const float my = io.MousePos.y - canvas_pos.y - canvas_size.y * 0.5f;
      const float before_x = play_cam_x_ + mx / play_cam_zoom_;
      const float before_y = play_cam_y_ + my / play_cam_zoom_;
      play_cam_zoom_ = std::clamp(play_cam_zoom_ * factor, 0.15f, 8.0f);
      play_cam_x_ = before_x - mx / play_cam_zoom_;
      play_cam_y_ = before_y - my / play_cam_zoom_;
    }
  }

  const float alpha = play_.alpha();
  const runtime::CameraView cam =
      play_free_cam_
          ? runtime::CameraView{play_cam_x_, play_cam_y_, play_cam_zoom_}
          : world.camera(alpha);
  auto to_screen = [&](float wx, float wy) {
    return ImVec2(canvas_pos.x + canvas_size.x * 0.5f + (wx - cam.x) * cam.zoom,
                  canvas_pos.y + canvas_size.y * 0.5f + (wy - cam.y) * cam.zoom);
  };
  const runtime::WorldRect view =
      runtime::World::view_rect(cam, canvas_size.x, canvas_size.y);
  world.build_draw_list(
      view, alpha,
      [this](const std::string& rel, int* w, int* h) {
        const TextureInfo& t = texture(rel);
        if (!t.ok) {
          return false;
        }
        *w = t.width;
        *h = t.height;
        return true;
      },
      &play_quads_);

  draw->PushClipRect(canvas_pos, canvas_end, true);
  quad_blend::Blender blender(draw);
  for (const runtime::DrawQuad& q : play_quads_) {
    const ImVec2 p0 = to_screen(q.x0, q.y0);
    const ImVec2 p1 = to_screen(q.x1, q.y1);
    const std::uint64_t handle = q.image ? texture(*q.image).handle : 0;
    blender.set(q.blend);
    if (handle != 0) {
      quad_blend::smooth(*q.image, handle);
      quad_draw::image(draw, play_tex_id(handle), p0, p1, q.rotation, ImVec2(q.u0, q.v0),
                     ImVec2(q.u1, q.v1), quad_u32(q.rgba));
    } else {
      quad_draw::filled(draw, p0, p1, q.rotation, quad_u32(q.rgba));
      if (q.missing) {
        missing_flag(draw, p0, p1);
      }
    }
  }
  blender.end();
  draw->PopClipRect();
  // Collision overlay (K): live walls, bodies and lit-up triggers.
  draw_collision_overlay(draw, canvas_pos, canvas_size, cam.x, cam.y,
                         cam.zoom);
  draw->PushClipRect(canvas_pos, canvas_end, true);
  // Rider tags.
  for (const runtime::Actor& a : world.actors()) {
    if (!a.data.player) {
      continue;
    }
    const float x = a.prev_x + (a.data.x - a.prev_x) * alpha;
    const float y = a.prev_y + (a.data.y - a.prev_y) * alpha;
    char tag[16];
    std::snprintf(tag, sizeof(tag), "P%d", a.data.player->slot + 1);
    const ImVec2 at = to_screen(x + a.data.w * 0.5f, y);
    const float tw = ImGui::CalcTextSize(tag).x;
    draw->AddText(ImVec2(at.x - tw * 0.5f, at.y - ImGui::GetTextLineHeight() - 2.0f),
                  theme::U32(theme::Accent()), tag);
  }
  draw_play_toasts(draw, canvas_pos, canvas_size);
  // In-game UI (ui/*.ui.json, Lua ui.*) over the view.
  if (debug_tools_on_ && (viewport_focused_ || hovered)) {
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) debug_tools_.toggle_overlay();
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) debug_tools_.toggle_console();
  }
  std::vector<ui::UiEvent> ui_events =
      play_ui_.update(canvas_pos, canvas_size, hovered, viewport_focused_, io.DeltaTime);
  if (debug_tools_on_) {
    ui_events = debug_tools_.filter(std::move(ui_events));
    debug_tools_.update(io.DeltaTime);
  }
  ui::forward_ui_events(world, ui_events);
  play_ui_.draw(draw, canvas_pos);
  if (is_paused()) {
    draw->AddRectFilled(canvas_pos, canvas_end,
                        theme::U32(theme::Charcoal(), 0.35f));
    const char* msg = "HOLDING UP";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    draw->AddText(ImVec2(canvas_pos.x + (canvas_size.x - ts.x) * 0.5f,
                         canvas_pos.y + 14.0f),
                  theme::U32(theme::Warning()), msg);
  }
  draw->PopClipRect();

  // Play indicator: tinted frame + badge with the trail clock.
  const ImVec4 tint = is_paused() ? theme::Warning() : theme::Copper();
  draw->AddRect(ImVec2(canvas_pos.x + 1.5f, canvas_pos.y + 1.5f),
                ImVec2(canvas_end.x - 1.5f, canvas_end.y - 1.5f),
                theme::U32(tint), 0.0f, 0, 3.0f);
  char badge[96];
  std::snprintf(badge, sizeof(badge), "%s  %s  tick %llu%s",
                is_paused() ? "PAUSED" : "RIDING",
                clock_text(play_.play_seconds()).c_str(),
                static_cast<unsigned long long>(play_.tick()),
                play_free_cam_ ? "  | free cam" : "");
  const ImVec2 bs = ImGui::CalcTextSize(badge);
  const ImVec2 b0(canvas_pos.x + 8.0f, canvas_pos.y + 8.0f);
  const ImVec2 b1(b0.x + bs.x + 14.0f, b0.y + bs.y + 8.0f);
  draw->AddRectFilled(b0, b1, theme::U32(tint, 0.9f), 3.0f);
  draw->AddText(ImVec2(b0.x + 7.0f, b0.y + 4.0f), theme::U32(theme::Charcoal()),
                badge);
}

// --- Inspector: gameplay components ---------------------------------------------

void Editor2DScreen::draw_inspector_gameplay(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || e->tilemap) {
    return;  // TileMaps are ground, not actors
  }
  ImGui::SeparatorText("Gameplay");

  // Discrete change: one undo step + autosave.
  auto edit = [&](const char* label, const char* note_text, auto&& fn) {
    Workspace2D::Snapshot before = prepare_edit();
    if (Entity2D* target = workspace_.find(id)) {
      fn(*target);
      normalize_components(*target);
      commit_discrete(std::move(before), label, note_text);
    }
  };
  static const char* kSlots[kMaxPlayers] = {"P1 (keys / pad 1)", "P2 (pad 2)",
                                            "P3 (pad 3)", "P4 (pad 4)"};

  if (e->player) {
    ImGui::PushID("player");
    ImGui::TextColored(theme::Accent(), "Player Controller");
    int slot = e->player->slot;
    if (ImGui::Combo("Slot", &slot, kSlots, kMaxPlayers)) {
      edit("Player Slot", "", [&](Entity2D& t) {
        if (t.player) t.player->slot = slot;
      });
    }
    if (Entity2D* t = workspace_.find(id); t && t->player) {
      if (ImGui::DragFloat("Speed", &t->player->speed, 1.0f,
                           PlayerControllerData::kMinSpeed,
                           PlayerControllerData::kMaxSpeed, "%.0f px/s",
                           ImGuiSliderFlags_AlwaysClamp)) {
        mark_dirty();
      }
      track_inspector_item("Edit Speed");
    }
    if (theme::DangerButton("Remove Player", ImVec2(120, 0))) {
      edit("Remove Player", "Unsaddled", [](Entity2D& t) { t.player.reset(); });
    }
    ImGui::PopID();
    e = workspace_.find(id);
    if (!e) return;
  }

  if (e->camera) {
    ImGui::PushID("camera");
    ImGui::TextColored(theme::Accent(), "Camera2D");
    const Entity2D* target = workspace_.find(e->camera->target);
    const std::string preview =
        target ? target->name : std::string("(stay put)");
    if (ImGui::BeginCombo("Follow", preview.c_str())) {
      if (ImGui::Selectable("(stay put)", e->camera->target == 0)) {
        edit("Camera Target", "", [](Entity2D& t) {
          if (t.camera) t.camera->target = 0;
        });
      }
      for (const Entity2D& other : workspace_.entities()) {
        if (other.id == id || other.tilemap) {
          continue;
        }
        char label[160];
        std::snprintf(label, sizeof(label), "%s%s##%llu", other.name.c_str(),
                      other.player ? "  (rider)" : "",
                      static_cast<unsigned long long>(other.id));
        if (ImGui::Selectable(label, other.id == e->camera->target)) {
          const std::uint64_t pick = other.id;
          edit("Camera Target", "", [pick](Entity2D& t) {
            if (t.camera) t.camera->target = pick;
          });
          break;  // entities() may have been replaced by the commit
        }
      }
      ImGui::EndCombo();
    }
    if (Entity2D* t = workspace_.find(id); t && t->camera) {
      if (ImGui::DragFloat("Smoothing", &t->camera->smoothing, 0.01f, 0.0f,
                           5.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) {
        mark_dirty();
      }
      track_inspector_item("Edit Camera Smoothing");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Catch-up time. 0 = locked on the target.");
      }
      if (ImGui::DragFloat("Zoom", &t->camera->zoom, 0.01f, 0.05f, 16.0f,
                           "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
        mark_dirty();
      }
      track_inspector_item("Edit Camera Zoom");
      bool bounds = t->camera->use_bounds;
      if (ImGui::Checkbox("Keep inside bounds", &bounds)) {
        edit("Camera Bounds", "", [bounds](Entity2D& c) {
          if (c.camera) c.camera->use_bounds = bounds;
        });
      }
    }
    if (Entity2D* t = workspace_.find(id);
        t && t->camera && t->camera->use_bounds) {
      float b[4] = {t->camera->bounds_x, t->camera->bounds_y,
                    t->camera->bounds_w, t->camera->bounds_h};
      if (ImGui::DragFloat4("Bounds x/y/w/h", b, 1.0f, -1.0e6f, 1.0e6f,
                            "%.0f")) {
        t->camera->bounds_x = b[0];
        t->camera->bounds_y = b[1];
        t->camera->bounds_w = std::max(1.0f, b[2]);
        t->camera->bounds_h = std::max(1.0f, b[3]);
        mark_dirty();
      }
      track_inspector_item("Edit Camera Bounds");
      if (const std::uint64_t tm = workspace_.first_tilemap()) {
        if (theme::SecondaryButton("Fit to TileMap", ImVec2(120, 0))) {
          const Entity2D* m = workspace_.find(tm);
          const float bx = m->x, by = m->y, bw = m->w, bh = m->h;
          edit("Camera Bounds", "Bounds fenced to the TileMap",
               [=](Entity2D& c) {
                 if (!c.camera) return;
                 c.camera->bounds_x = bx;
                 c.camera->bounds_y = by;
                 c.camera->bounds_w = bw;
                 c.camera->bounds_h = bh;
               });
        }
        ImGui::SameLine();
      }
    }
    if (theme::DangerButton("Remove Camera", ImVec2(120, 0))) {
      edit("Remove Camera", "Camera pulled", [](Entity2D& t) {
        t.camera.reset();
      });
    }
    ImGui::PopID();
    e = workspace_.find(id);
    if (!e) return;
  }

  if (e->spawn) {
    ImGui::PushID("spawn");
    ImGui::TextColored(theme::Accent(), "Spawn Point");
    int slot = e->spawn->slot;
    if (ImGui::Combo("Slot", &slot, kSlots, kMaxPlayers)) {
      edit("Spawn Slot", "", [&](Entity2D& t) {
        if (t.spawn) t.spawn->slot = slot;
      });
    }
    ImGui::TextDisabled("The slot's rider starts centred here.");
    if (theme::DangerButton("Remove Spawn", ImVec2(120, 0))) {
      edit("Remove Spawn", "Spawn pulled up", [](Entity2D& t) {
        t.spawn.reset();
      });
    }
    ImGui::PopID();
    e = workspace_.find(id);
    if (!e) return;
  }

  // Add what is missing.
  const bool can_player = !e->player;
  const bool can_camera = !e->camera;
  const bool can_spawn = !e->spawn;
  if (can_player || can_camera || can_spawn) {
    ImGui::TextDisabled("Add:");
    if (can_player) {
      ImGui::SameLine();
      if (theme::SecondaryButton("Player", ImVec2(64, 0))) {
        edit("Add Player", "Saddled up on slot P1",
             [](Entity2D& t) { t.player = PlayerControllerData{}; });
      }
    }
    if (can_camera) {
      ImGui::SameLine();
      if (theme::SecondaryButton("Camera", ImVec2(64, 0))) {
        std::uint64_t follow = 0;
        for (const Entity2D& other : workspace_.entities()) {
          if (other.player && other.id != id) {
            follow = other.id;
            break;
          }
        }
        edit("Add Camera", "Camera on the rider", [follow](Entity2D& t) {
          Camera2DData c;
          c.target = follow;
          t.camera = c;
        });
      }
    }
    if (can_spawn) {
      ImGui::SameLine();
      if (theme::SecondaryButton("Spawn", ImVec2(64, 0))) {
        edit("Add Spawn", "Spawn staked",
             [](Entity2D& t) { t.spawn = SpawnPointData{}; });
      }
    }
  }
  // Collider: AABB, solid / trigger, static / dynamic.
  draw_inspector_collider(id);
  // Rigid Body (Box2D) + Joint.
  draw_inspector_body(id);
  // Script: file + per-entity props.
  draw_inspector_script(id);
  // Audio Source: clip, bus, autoplay / loop, positional.
  draw_inspector_audio(id);
  // Particles + Light.
  draw_inspector_fx(id);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
