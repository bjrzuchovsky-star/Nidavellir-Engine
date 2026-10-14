#pragma once

// VR sessions for "dimension": "vr" projects. Two backends behind one
// interface:
//
//   * OpenXR (OpenXrSession.cpp): the Khronos loader + whatever runtime is
//     active (SteamVR, Oculus, WMR...). Windows renders through
//     XR_KHR_opengl_enable (WGL context of the calling window); elsewhere
//     the loader is built and probed but sessions report unavailable and
//     callers fall back to the simulator.
//   * The desktop simulator (SimXrSession): a deterministic stand-in with
//     a head and two hands driven by mouse + keys (or set directly by
//     tests), rendering the same stereo pair into its own targets. Works
//     headless (smokes) and without a headset.
//
// Frame loop (GL context current):
//   poll_events(); if (running() && wait_frame(&f)) {
//     input() -> World3D::set_vr_input; for eye 0/1: bind_eye(eye), draw
//     with f.views[eye] (tracking space; World3D::to_world places it);
//     end_frame(); }
// Each eye is rendered into an RGBA8 target the app owns (eye_texture()
// feeds mirror windows); OpenXR copies it into the runtime swapchain.

#include "render3d/Math3D.h"
#include "world3d/VrTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace xr {

// Mirrors XrSessionState.
enum class SessionState {
  Unknown,
  Idle,
  Ready,
  Synchronized,
  Visible,
  Focused,
  Stopping,
  LossPending,
  Exiting,
};
const char* to_string(SessionState s);

enum class FormFactor { HeadMounted, Handheld };

struct XrConfig {
  std::string app_name = "TombStone";
  FormFactor form_factor = FormFactor::HeadMounted;
  bool want_hand_tracking = false;
  // Simulator eye targets (per eye) and interpupillary distance.
  int sim_eye_width = 640;
  int sim_eye_height = 640;
  float sim_ipd = 0.064f;
};

struct EyeView {
  render3d::Vec3 position;     // tracking space
  render3d::Quat orientation;
  float fov_left = 0.0f, fov_right = 0.0f, fov_up = 0.0f, fov_down = 0.0f;  // radians
};

struct FrameInfo {
  long long predicted_display_time_ns = 0;
  bool should_render = false;
  std::vector<EyeView> views;  // 2 for a headset
};

class XrSession {
 public:
  virtual ~XrSession() = default;
  virtual const char* runtime_name() const = 0;  // "OpenXR" / "simulator"
  // Runtime + system ("SteamVR/OpenXR 2.x, Index HMD").
  virtual std::string system_name() const { return runtime_name(); }
  virtual bool simulated() const = 0;
  virtual bool available() const = 0;
  // Why available() is false ("no headset", "no OpenXR runtime"...).
  virtual const std::string& unavailable_reason() const = 0;

  // xrCreateSession + swapchains + actions (GL context current).
  virtual bool begin(std::string* error) = 0;
  virtual void end() = 0;  // tear everything down (any state)
  virtual SessionState state() const = 0;
  // Pump runtime events; false once the session wants to exit.
  virtual bool poll_events() = 0;
  // Between xrBeginSession and xrEndSession (frames may be submitted).
  virtual bool running() const = 0;
  // xrWaitFrame + xrBeginFrame + locate views + sync input. False when no
  // frame was started (not running).
  virtual bool wait_frame(FrameInfo* out) = 0;
  // Head + hands of the last wait_frame (tracking space).
  virtual const world3d::VrInput& input() const = 0;
  virtual int eye_width() const = 0;
  virtual int eye_height() const = 0;
  // Bind eye 0 / 1's framebuffer (GL_FRAMEBUFFER) and its viewport.
  virtual bool bind_eye(int eye) = 0;
  // Submit the frame (copies the eye targets into the swapchains).
  virtual void end_frame() = 0;
  // The eye targets (GL names, 0 headless) for mirrors / pixel checks.
  virtual std::uint32_t eye_texture(int eye) const = 0;
  virtual std::uint32_t eye_framebuffer(int eye) const = 0;
  virtual void haptic(const world3d::VrHaptic& /*h*/) {}
  // RGBA8 rows bottom-up of an eye target; false headless.
  bool read_eye(int eye, std::vector<unsigned char>* rgba, int* w, int* h) const;
};

// ---- Desktop simulator ------------------------------------------------------

// One frame of desktop controls (from GLFW or ImGui; see README "VR").
struct SimDrive {
  float look_dx = 0.0f, look_dy = 0.0f;  // mouse pixels while looking
  bool forward = false, back = false, left = false, right = false;  // W S A D
  bool turn_left = false, turn_right = false;  // Q / E: right stick x
  bool teleport = false;      // T held: right stick forward (release = go)
  bool trigger = false;       // left mouse button
  bool grip = false;          // G / middle mouse
  bool left_hand = false;     // Shift: trigger / grip act on the left hand
  bool menu = false;          // M / Tab
  bool crouch = false;        // Ctrl
  bool reach_out = false, reach_in = false;  // X / Z: hands farther / closer
};

class SimXrSession final : public XrSession {
 public:
  struct Controls {
    float yaw_deg = 0.0f, pitch_deg = 0.0f;
    render3d::Vec3 head{0.0f, 1.7f, 0.0f};
    float standing_height = 1.7f;
    float reach = 0.38f;  // m in front of the head
    // Hands follow the head (aim where you look) unless set manually.
    bool hands_follow_head = true;
    world3d::VrInput manual;  // used when !hands_follow_head (and its head)
    world3d::VrHandInput buttons[2];  // triggers / sticks / buttons
  };

  explicit SimXrSession(const XrConfig& config);
  ~SimXrSession() override;

  const char* runtime_name() const override { return "simulator"; }
  std::string system_name() const override { return "TombStone desktop VR simulator"; }
  bool simulated() const override { return true; }
  bool available() const override { return true; }
  const std::string& unavailable_reason() const override { return none_; }
  bool begin(std::string* error) override;
  void end() override;
  SessionState state() const override { return state_; }
  bool poll_events() override { return state_ != SessionState::Exiting; }
  bool running() const override { return running_; }
  bool wait_frame(FrameInfo* out) override;
  const world3d::VrInput& input() const override { return input_; }
  int eye_width() const override { return w_; }
  int eye_height() const override { return h_; }
  bool bind_eye(int eye) override;
  void end_frame() override { ++frames_; }
  std::uint32_t eye_texture(int eye) const override;
  std::uint32_t eye_framebuffer(int eye) const override;
  void haptic(const world3d::VrHaptic& h) override;

  Controls& controls() { return c_; }
  // Apply one frame of desktop controls.
  void drive(const SimDrive& d, double dt);
  long frames() const { return frames_; }
  // Last haptic per hand (amplitude, seconds) for tests / the mirror.
  world3d::VrHaptic last_haptic(int hand) const { return last_haptic_[hand & 1]; }
  std::size_t haptic_count() const { return haptics_; }
  // Ask the loop to end (like the runtime's EXITING).
  void request_exit() { state_ = SessionState::Exiting; }
  bool gl() const { return gl_; }

 private:
  void compute_input();
  XrConfig cfg_;
  Controls c_;
  world3d::VrInput input_;
  std::string none_;
  SessionState state_ = SessionState::Idle;
  bool running_ = false;
  bool gl_ = false;
  int w_ = 0, h_ = 0;
  std::uint32_t fbo_[2] = {0, 0}, color_[2] = {0, 0}, depth_[2] = {0, 0};
  long frames_ = 0;
  long long time_ns_ = 0;
  world3d::VrHaptic last_haptic_[2];
  std::size_t haptics_ = 0;
  bool menu_was_ = false;
};

// ---- Factories ----------------------------------------------------------------

// OpenXR: instance + system (headset) probed now; available() false with
// the reason when there is no loader / runtime / headset (or no GL binding
// on this platform).
std::unique_ptr<XrSession> create_openxr_session(const XrConfig& config);
std::unique_ptr<SimXrSession> create_sim_session(const XrConfig& config);
// OpenXR when a headset answers, else the simulator. `why` gets the
// reason when it fell back.
std::unique_ptr<XrSession> create_xr_session(const XrConfig& config, std::string* why = nullptr);
// True when this build has the OpenXR loader linked.
bool openxr_built();

// Non-interactive check (ts_game --xr-probe): loader, runtime, headset and,
// with a GL context current (Windows), a session created + destroyed.
struct XrProbe {
  bool loader = false;
  bool instance = false;
  bool system = false;
  bool session = false;
  std::string runtime;  // "SteamVR/OpenXR 2.x"
  std::string system_name;
  std::string error;    // first failure
  std::string swapchain;  // "2 x 2016x2224 GL_SRGB8_ALPHA8"
};
XrProbe probe_openxr(bool create_session);

}  // namespace xr
}  // namespace tombstone
}  // namespace ts
