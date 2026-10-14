#include "xr/XrSession.h"

namespace ts {
namespace tombstone {
namespace xr {

const char* to_string(SessionState s) {
  switch (s) {
    case SessionState::Idle: return "idle";
    case SessionState::Ready: return "ready";
    case SessionState::Synchronized: return "synchronized";
    case SessionState::Visible: return "visible";
    case SessionState::Focused: return "focused";
    case SessionState::Stopping: return "stopping";
    case SessionState::LossPending: return "loss pending";
    case SessionState::Exiting: return "exiting";
    case SessionState::Unknown: break;
  }
  return "unknown";
}

namespace {

class StubXrSession final : public XrSession {
 public:
  explicit StubXrSession(const XrConfig& config)
      : reason_("no OpenXR loader linked yet (app '" + config.app_name +
                "'): this frontier ain't surveyed yet") {}

  const char* runtime_name() const override { return "stub"; }
  bool available() const override { return false; }
  const std::string& unavailable_reason() const override { return reason_; }
  bool begin(std::string* error) override {
    if (error) *error = reason_;
    return false;
  }
  void end() override {}
  SessionState state() const override { return SessionState::Unknown; }
  bool poll_events() override { return false; }
  bool wait_frame(FrameInfo* out) override {
    if (out) *out = FrameInfo{};
    return false;
  }
  void begin_frame() override {}
  void end_frame() override {}

 private:
  std::string reason_;
};

}  // namespace

std::unique_ptr<XrSession> create_xr_session(const XrConfig& config) {
  return std::make_unique<StubXrSession>(config);
}

}  // namespace xr
}  // namespace tombstone
}  // namespace ts
