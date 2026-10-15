// VR claims in the 3D editor: Play in the desktop simulator, "Preview in
// headset" through OpenXR, and the Headset Mirror window (both eyes, the
// session state, comfort settings, simulator keys).

#include "weather/Weather3D.h"
#include "editor/editor3d/Editor3DScreen.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

using runtime::LogLevel;

namespace {
constexpr const char* kVrMirror = "Headset Mirror###HeadsetMirror3D";
constexpr const char* kSimKeys =
    "RMB drag look | WASD move | Q/E snap turn | hold T teleport arc (release to go) | "
    "LMB trigger (laser) | G or MMB grab / throw | Shift: left hand | M comfort menu | "
    "Ctrl crouch | Z/X hands closer/farther";
}  // namespace

xr::SimXrSession* Editor3DScreen::vr_sim() {
  return dynamic_cast<xr::SimXrSession*>(vr_session_.get());
}

bool Editor3DScreen::vr_preview_start(bool simulator) {
  if (vr_session_) return true;
  if (playing()) play_stop();
  gizmo_cancel();
  ensure_renderer();
  world_.set_log_sink([this](const runtime::LogEntry& e) {
    log_.push_back(e);
    while (log_.size() > 400) log_.pop_front();
  });
  xr::XrConfig cfg;
  cfg.app_name = project_.name;
  std::unique_ptr<xr::XrSession> s;
  std::string why;
  if (!simulator) {
    s = xr::create_openxr_session(cfg);
    if (!s->available()) {
      why = s->unavailable_reason();
      s.reset();
    } else if (!s->begin(&why)) {
      s.reset();
    }
    if (!s) note("Headset unavailable: " + why + ". Previewing in the desktop simulator.", LogLevel::Warn);
  }
  if (!s) {
    auto sim = xr::create_sim_session(cfg);
    std::string err;
    sim->begin(&err);
    s = std::move(sim);
  }
  world_.set_vr(true);
  if (!weather::Weather3D::of(world_)) weather::attach(world_, plugins_.loaded("ts.weather"));
  std::string err;
  if (!world_.start(scene_, project_.path, &err)) {
    s->end();
    world_.set_vr(false);
    note("VR preview failed: " + err, LogLevel::Error);
    return false;
  }
  vr_session_ = std::move(s);
  vr_status_ = vr_session_->system_name();
  vr_frame_ = {};
  if (vr_session_->simulated()) {
    note("VR preview in the desktop simulator (hover the viewport or Headset Mirror): " +
         std::string(kSimKeys));
  } else {
    note("VR preview on " + vr_status_ + ": put the headset on. Stop / Esc / F5 ends it.");
  }
  return true;
}

void Editor3DScreen::vr_preview_stop() { play_stop(); }

void Editor3DScreen::vr_preview_frame(double dt) {
  if (!vr_session_) return;
  ensure_renderer();
  if (xr::SimXrSession* sim = vr_sim()) {
    if (ImGui::GetCurrentContext() != nullptr) sim->drive(poll_sim_drive(), dt);
  }
  vr_presenter_ = std::make_unique<xr::VrPresenter>(renderer_.get(), view_.get());
  vr_presenter_->set_restore_offscreen(gl_);
  vr_frame_ = {};
  if (!vr_presenter_->frame(*vr_session_, world_, dt, &vr_frame_)) {
    note("The headset session ended", LogLevel::Warn);
    play_stop();
  }
}

xr::SimDrive Editor3DScreen::poll_sim_drive() {
  xr::SimDrive d;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || !(viewport_hovered_ || vr_mirror_hovered_)) return d;
  auto down = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
  if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    d.look_dx = io.MouseDelta.x;
    d.look_dy = io.MouseDelta.y;
  }
  d.forward = down(ImGuiKey_W);
  d.back = down(ImGuiKey_S);
  d.left = down(ImGuiKey_A);
  d.right = down(ImGuiKey_D);
  d.turn_left = down(ImGuiKey_Q);
  d.turn_right = down(ImGuiKey_E);
  d.teleport = down(ImGuiKey_T);
  d.trigger = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  d.grip = down(ImGuiKey_G) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
  d.left_hand = io.KeyShift;
  d.menu = down(ImGuiKey_M) || down(ImGuiKey_Tab);
  d.crouch = io.KeyCtrl;
  d.reach_out = down(ImGuiKey_X);
  d.reach_in = down(ImGuiKey_Z);
  return d;
}

void Editor3DScreen::draw_vr_mirror() {
  if (!vr_project()) return;
  ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
  vr_mirror_hovered_ = false;
  if (!ImGui::Begin(kVrMirror)) {
    ImGui::End();
    return;
  }
  vr_mirror_hovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  if (!vr_session_) {
    ImGui::TextWrapped("Not previewing. Play (F5) runs the scene in the desktop simulator; "
                       "Preview in headset uses the active OpenXR runtime (SteamVR) and "
                       "falls back to the simulator.");
    if (theme::PrimaryButton("Preview in headset")) vr_preview_start(false);
    ImGui::SameLine();
    if (theme::SecondaryButton("Simulator")) vr_preview_start(true);
    ImGui::End();
    return;
  }
  xr::XrSession& s = *vr_session_;
  ImGui::Text("%s  |  %s  |  eyes %dx%d", s.system_name().c_str(), xr::to_string(s.state()),
              s.eye_width(), s.eye_height());
  const float avail = ImGui::GetContentRegionAvail().x;
  const float w = std::max(64.0f, (avail - 8.0f) * 0.5f);
  const float h = s.eye_width() > 0 ? w * static_cast<float>(s.eye_height()) / s.eye_width() : w;
  for (int e = 0; e < 2; ++e) {
    if (e) ImGui::SameLine(0, 8);
    const std::uint32_t tex = s.eye_texture(e);
    if (tex) {
      ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
    } else {
      ImGui::Button(e ? "right eye (headless)" : "left eye (headless)", ImVec2(w, h));
    }
  }
  const scene3d::VrRig3D& cfg = world_.vr_settings();
  ImGui::Text("Comfort: %s, %s, vignette %s, %s",
              cfg.locomotion == scene3d::VrRig3D::Locomotion::Teleport ? "teleport only"
              : cfg.locomotion == scene3d::VrRig3D::Locomotion::Smooth ? "smooth only"
                                                                        : "teleport + smooth",
              cfg.snap_turn_deg > 0 ? (std::to_string(static_cast<int>(cfg.snap_turn_deg)) + " deg snap").c_str()
                                    : "smooth turn",
              cfg.vignette ? "on" : "off", cfg.seated ? "seated" : "standing");
  if (world_.comfort_menu_open()) {
    std::string labels;
    for (const world3d::VrMenuButton& b : world_.comfort_menu()) {
      labels += (labels.empty() ? "" : "  |  ") + b.label;
    }
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1.0f), "Menu: %s", labels.c_str());
  }
  const world3d::World3D::VrStats& st = world_.vr_stats();
  ImGui::Text("grabs %llu  throws %llu  teleports %llu  snap turns %llu  laser %llu  ticks %llu",
              static_cast<unsigned long long>(st.grabs), static_cast<unsigned long long>(st.throws),
              static_cast<unsigned long long>(st.teleports),
              static_cast<unsigned long long>(st.snap_turns),
              static_cast<unsigned long long>(st.laser_triggers),
              static_cast<unsigned long long>(world_.tick()));
  for (int hnd = 0; hnd < 2; ++hnd) {
    const world3d::VrHandState& hs = world_.hand(hnd);
    const scene3d::Entity3D* held = world_.find(hs.held);
    const scene3d::Entity3D* pointed = world_.find(hs.pointed);
    ImGui::Text("%s: trigger %.2f grip %.2f%s%s%s%s", world3d::hand_name(hnd),
                static_cast<double>(hs.trigger), static_cast<double>(hs.squeeze),
                held ? "  holding " : "", held ? held->name.c_str() : "",
                pointed ? "  pointing at " : "", pointed ? pointed->name.c_str() : "");
  }
  if (s.simulated()) ImGui::TextDisabled("%s", kSimKeys);
  if (theme::SecondaryButton("Stop preview")) play_stop();
  ImGui::End();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
