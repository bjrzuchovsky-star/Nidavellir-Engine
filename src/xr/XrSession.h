#pragma once

// VR / XR session interface (sketch of an OpenXR session). No OpenXR
// loader is linked yet: create_xr_session() returns a stub that reports
// "not available" with the reason, so VR projects can open and show their
// placeholder. The shape follows OpenXR: instance + system, session
// lifecycle states, per-frame wait/begin/end with one view per eye.

#include "render3d/Math3D.h"

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
};

struct EyeView {
  render3d::Vec3 position;
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
  virtual const char* runtime_name() const = 0;  // "stub", later "OpenXR"
  virtual bool available() const = 0;
  // Why available() is false ("no OpenXR loader linked", "no headset").
  virtual const std::string& unavailable_reason() const = 0;

  virtual bool begin(std::string* error) = 0;  // xrBeginSession
  virtual void end() = 0;                      // xrEndSession
  virtual SessionState state() const = 0;
  // Pump runtime events (xrPollEvent); returns false when the session ends.
  virtual bool poll_events() = 0;
  virtual bool wait_frame(FrameInfo* out) = 0;  // xrWaitFrame + locate views
  virtual void begin_frame() = 0;
  virtual void end_frame() = 0;
};

std::unique_ptr<XrSession> create_xr_session(const XrConfig& config);

}  // namespace xr
}  // namespace tombstone
}  // namespace ts
