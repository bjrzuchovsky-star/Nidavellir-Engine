// OpenXR backend for ts_xr (see XrSession.h). Built when the loader is
// (TS_HAVE_OPENXR); headset rendering needs XR_KHR_opengl_enable on Win32
// (the GL context current when begin() is called). Everything else
// (instance, system probe, events) is portable.

#include "xr/XrSession.h"

#if defined(TS_HAVE_OPENXR)

#if defined(_WIN32)
#include <windows.h>
#include <unknwn.h>  // IUnknown (openxr_platform.h; WIN32_LEAN_AND_MEAN skips it)
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#endif

#include "render3d/GlApi.h"
#include "xr/EyeTarget.h"

#include <openxr/openxr.h>
#if defined(_WIN32)
#include <openxr/openxr_platform.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <utility>

namespace ts {
namespace tombstone {
namespace xr {

namespace {

using render3d::Quat;
using render3d::Vec3;

void copy_name(char* dst, std::size_t cap, const char* src) {
  std::snprintf(dst, cap, "%s", src);
}

std::string result_text(XrInstance inst, XrResult r) {
  char buf[XR_MAX_RESULT_STRING_SIZE] = {0};
  if (inst != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(inst, r, buf))) return buf;
  return "XrResult " + std::to_string(static_cast<int>(r));
}

world3d::VrPose to_pose(const XrPosef& p, bool valid, float lift) {
  world3d::VrPose out;
  out.position = {p.position.x, p.position.y + lift, p.position.z};
  out.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
  out.valid = valid;
  return out;
}

class OpenXrSession final : public XrSession {
 public:
  explicit OpenXrSession(const XrConfig& config) : cfg_(config) { probe(); }
  ~OpenXrSession() override {
    end();
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
  }

  const char* runtime_name() const override { return "OpenXR"; }
  std::string system_name() const override {
    return runtime_ + (system_label_.empty() ? std::string() : ", " + system_label_);
  }
  bool simulated() const override { return false; }
  bool available() const override { return available_; }
  const std::string& unavailable_reason() const override { return reason_; }
  bool instance_ok() const { return instance_ != XR_NULL_HANDLE; }
  bool system_ok() const { return system_ != XR_NULL_SYSTEM_ID; }
  const std::string& runtime() const { return runtime_; }
  const std::string& system_label() const { return system_label_; }
  std::string swapchain_text() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "2 x %dx%d %s", w_, h_,
                  format_ == 0x8C43 ? "GL_SRGB8_ALPHA8" : format_ == 0x8058 ? "GL_RGBA8" : "other");
    return buf;
  }

  bool begin(std::string* error) override;
  void end() override;
  SessionState state() const override { return map_state(xr_state_); }
  bool poll_events() override;
  bool running() const override { return running_; }
  bool wait_frame(FrameInfo* out) override;
  const world3d::VrInput& input() const override { return input_; }
  int eye_width() const override { return w_; }
  int eye_height() const override { return h_; }
  bool bind_eye(int eye) override {
    if (eye < 0 || eye > 1 || !eyes_[eye].fbo) return false;
    eyes_[eye].bind();
    return true;
  }
  void end_frame() override;
  std::uint32_t eye_texture(int eye) const override { return eyes_[eye & 1].color; }
  std::uint32_t eye_framebuffer(int eye) const override { return eyes_[eye & 1].fbo; }
  void haptic(const world3d::VrHaptic& h) override;

 private:
  static SessionState map_state(XrSessionState s) {
    switch (s) {
      case XR_SESSION_STATE_IDLE: return SessionState::Idle;
      case XR_SESSION_STATE_READY: return SessionState::Ready;
      case XR_SESSION_STATE_SYNCHRONIZED: return SessionState::Synchronized;
      case XR_SESSION_STATE_VISIBLE: return SessionState::Visible;
      case XR_SESSION_STATE_FOCUSED: return SessionState::Focused;
      case XR_SESSION_STATE_STOPPING: return SessionState::Stopping;
      case XR_SESSION_STATE_LOSS_PENDING: return SessionState::LossPending;
      case XR_SESSION_STATE_EXITING: return SessionState::Exiting;
      default: break;
    }
    return SessionState::Unknown;
  }
  void probe();
  bool fail(std::string* error, const std::string& what, XrResult r) {
    const std::string text = what + ": " + result_text(instance_, r);
    if (error) *error = text;
    end();
    return false;
  }
  bool create_actions(std::string* error);
  void read_hand(int h, XrTime t);

  XrConfig cfg_;
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  ::XrSession session_ = XR_NULL_HANDLE;
  XrSpace app_space_ = XR_NULL_HANDLE, view_space_ = XR_NULL_HANDLE;
  bool stage_ = false;
  bool has_gl_ext_ = false;
  bool available_ = false;
  std::string reason_, runtime_, system_label_;
  XrSessionState xr_state_ = XR_SESSION_STATE_UNKNOWN;
  bool running_ = false;
  bool exit_ = false;
  int w_ = 0, h_ = 0;
  std::int64_t format_ = 0;
  XrSwapchain swapchain_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
#if defined(_WIN32)
  std::vector<XrSwapchainImageOpenGLKHR> images_[2];
#endif
  EyeTarget eyes_[2];
  GLuint copy_fbo_ = 0;
  XrActionSet set_ = XR_NULL_HANDLE;
  XrAction a_grip_ = XR_NULL_HANDLE, a_aim_ = XR_NULL_HANDLE, a_trigger_ = XR_NULL_HANDLE,
           a_squeeze_ = XR_NULL_HANDLE, a_stick_ = XR_NULL_HANDLE, a_primary_ = XR_NULL_HANDLE,
           a_secondary_ = XR_NULL_HANDLE, a_menu_ = XR_NULL_HANDLE, a_haptic_ = XR_NULL_HANDLE;
  XrPath hand_[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrSpace grip_space_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace aim_space_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrFrameState frame_state_{XR_TYPE_FRAME_STATE};
  std::vector<XrView> views_;
  bool frame_begun_ = false;
  bool should_render_ = false;
  world3d::VrInput input_;
};

void OpenXrSession::probe() {
  std::uint32_t n = 0;
  XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
  if (XR_FAILED(r)) {
    reason_ = "no OpenXR runtime is active (SteamVR: Settings > OpenXR > Set SteamVR as "
              "OpenXR runtime)";
    return;
  }
  std::vector<XrExtensionProperties> exts(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
  for (const XrExtensionProperties& e : exts) {
    if (std::strcmp(e.extensionName, "XR_KHR_opengl_enable") == 0) has_gl_ext_ = true;
  }
  std::vector<const char*> enable;
#if defined(_WIN32)
  if (has_gl_ext_) enable.push_back("XR_KHR_opengl_enable");
#endif
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  copy_name(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE,
            cfg_.app_name.empty() ? "TombStone" : cfg_.app_name.c_str());
  ci.applicationInfo.applicationVersion = 1;
  copy_name(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "TombStone Engine");
  ci.applicationInfo.engineVersion = 1;
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ci.enabledExtensionCount = static_cast<std::uint32_t>(enable.size());
  ci.enabledExtensionNames = enable.empty() ? nullptr : enable.data();
  r = xrCreateInstance(&ci, &instance_);
  if (XR_FAILED(r)) {
    instance_ = XR_NULL_HANDLE;
    reason_ = "OpenXR runtime refused an instance: " + result_text(XR_NULL_HANDLE, r);
    return;
  }
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip))) {
    char buf[XR_MAX_RUNTIME_NAME_SIZE + 48];
    std::snprintf(buf, sizeof(buf), "%s %u.%u.%u", ip.runtimeName,
                  static_cast<unsigned>(XR_VERSION_MAJOR(ip.runtimeVersion)),
                  static_cast<unsigned>(XR_VERSION_MINOR(ip.runtimeVersion)),
                  static_cast<unsigned>(XR_VERSION_PATCH(ip.runtimeVersion)));
    runtime_ = buf;
  }
  XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
  si.formFactor = cfg_.form_factor == FormFactor::Handheld ? XR_FORM_FACTOR_HANDHELD_DISPLAY
                                                           : XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  r = xrGetSystem(instance_, &si, &system_);
  if (XR_FAILED(r)) {
    system_ = XR_NULL_SYSTEM_ID;
    reason_ = r == XR_ERROR_FORM_FACTOR_UNAVAILABLE
                  ? runtime_ + ": no headset found (plug it in, start SteamVR, wait for it to go green)"
                  : runtime_ + ": xrGetSystem failed (" + result_text(instance_, r) + ")";
    return;
  }
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(instance_, system_, &sp))) system_label_ = sp.systemName;
#if defined(_WIN32)
  if (!has_gl_ext_) {
    reason_ = runtime_ + " does not offer XR_KHR_opengl_enable (TombStone renders with OpenGL)";
    return;
  }
  available_ = true;
#else
  reason_ = runtime_ + " found, but headset rendering is Windows-only in this build "
                       "(XR_KHR_opengl_enable via WGL); using the simulator";
#endif
}

[[maybe_unused]] bool OpenXrSession::create_actions(std::string* error) {
  xrStringToPath(instance_, "/user/hand/left", &hand_[0]);
  xrStringToPath(instance_, "/user/hand/right", &hand_[1]);
  XrActionSetCreateInfo as{XR_TYPE_ACTION_SET_CREATE_INFO};
  copy_name(as.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "tombstone");
  copy_name(as.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "TombStone");
  XrResult r = xrCreateActionSet(instance_, &as, &set_);
  if (XR_FAILED(r)) return fail(error, "xrCreateActionSet", r);
  auto make = [&](XrAction* a, const char* name, const char* loc, XrActionType type) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = type;
    copy_name(ai.actionName, XR_MAX_ACTION_NAME_SIZE, name);
    copy_name(ai.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, loc);
    ai.countSubactionPaths = 2;
    ai.subactionPaths = hand_;
    return xrCreateAction(set_, &ai, a);
  };
  const std::pair<XrAction*, XrResult> made[] = {
      {&a_grip_, make(&a_grip_, "grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT)},
      {&a_aim_, make(&a_aim_, "aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT)},
      {&a_trigger_, make(&a_trigger_, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT)},
      {&a_squeeze_, make(&a_squeeze_, "squeeze", "Grab", XR_ACTION_TYPE_FLOAT_INPUT)},
      {&a_stick_, make(&a_stick_, "thumbstick", "Move / turn / teleport", XR_ACTION_TYPE_VECTOR2F_INPUT)},
      {&a_primary_, make(&a_primary_, "primary", "Primary button", XR_ACTION_TYPE_BOOLEAN_INPUT)},
      {&a_secondary_, make(&a_secondary_, "secondary", "Secondary button", XR_ACTION_TYPE_BOOLEAN_INPUT)},
      {&a_menu_, make(&a_menu_, "menu", "Comfort menu", XR_ACTION_TYPE_BOOLEAN_INPUT)},
      {&a_haptic_, make(&a_haptic_, "haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT)}};
  for (const auto& m : made) {
    if (XR_FAILED(m.second)) return fail(error, "xrCreateAction", m.second);
  }
  // Suggested bindings: one profile failing (runtime does not know it) is
  // not fatal.
  auto suggest = [&](const char* profile,
                     const std::vector<std::pair<XrAction, std::string>>& binds) {
    std::vector<XrActionSuggestedBinding> sb;
    for (const auto& [action, path] : binds) {
      XrPath p = XR_NULL_PATH;
      if (XR_SUCCEEDED(xrStringToPath(instance_, path.c_str(), &p))) sb.push_back({action, p});
    }
    XrPath prof = XR_NULL_PATH;
    xrStringToPath(instance_, profile, &prof);
    XrInteractionProfileSuggestedBinding is{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    is.interactionProfile = prof;
    is.countSuggestedBindings = static_cast<std::uint32_t>(sb.size());
    is.suggestedBindings = sb.data();
    const XrResult rr = xrSuggestInteractionProfileBindings(instance_, &is);
    if (XR_FAILED(rr)) {
      std::cerr << "[ts_xr] bindings for " << profile << " refused: " << result_text(instance_, rr)
                << '\n';
    }
  };
  auto both = [&](std::vector<std::pair<XrAction, std::string>>* v, XrAction a,
                  const char* tail) {
    v->push_back({a, std::string("/user/hand/left/") + tail});
    v->push_back({a, std::string("/user/hand/right/") + tail});
  };
  {
    std::vector<std::pair<XrAction, std::string>> v;
    both(&v, a_grip_, "input/grip/pose");
    both(&v, a_aim_, "input/aim/pose");
    both(&v, a_trigger_, "input/select/click");
    both(&v, a_menu_, "input/menu/click");
    both(&v, a_haptic_, "output/haptic");
    suggest("/interaction_profiles/khr/simple_controller", v);
  }
  {
    std::vector<std::pair<XrAction, std::string>> v;
    both(&v, a_grip_, "input/grip/pose");
    both(&v, a_aim_, "input/aim/pose");
    both(&v, a_trigger_, "input/trigger/value");
    both(&v, a_squeeze_, "input/squeeze/value");
    both(&v, a_stick_, "input/thumbstick");
    both(&v, a_primary_, "input/a/click");
    both(&v, a_secondary_, "input/b/click");
    v.push_back({a_menu_, "/user/hand/left/input/b/click"});
    both(&v, a_haptic_, "output/haptic");
    suggest("/interaction_profiles/valve/index_controller", v);
  }
  {
    std::vector<std::pair<XrAction, std::string>> v;
    both(&v, a_grip_, "input/grip/pose");
    both(&v, a_aim_, "input/aim/pose");
    both(&v, a_trigger_, "input/trigger/value");
    both(&v, a_squeeze_, "input/squeeze/click");
    both(&v, a_stick_, "input/trackpad");
    both(&v, a_primary_, "input/trackpad/click");
    both(&v, a_menu_, "input/menu/click");
    both(&v, a_haptic_, "output/haptic");
    suggest("/interaction_profiles/htc/vive_controller", v);
  }
  {
    std::vector<std::pair<XrAction, std::string>> v;
    both(&v, a_grip_, "input/grip/pose");
    both(&v, a_aim_, "input/aim/pose");
    both(&v, a_trigger_, "input/trigger/value");
    both(&v, a_squeeze_, "input/squeeze/value");
    both(&v, a_stick_, "input/thumbstick");
    v.push_back({a_primary_, "/user/hand/left/input/x/click"});
    v.push_back({a_secondary_, "/user/hand/left/input/y/click"});
    v.push_back({a_menu_, "/user/hand/left/input/menu/click"});
    v.push_back({a_primary_, "/user/hand/right/input/a/click"});
    v.push_back({a_secondary_, "/user/hand/right/input/b/click"});
    both(&v, a_haptic_, "output/haptic");
    suggest("/interaction_profiles/oculus/touch_controller", v);
  }
  for (int h = 0; h < 2; ++h) {
    XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    sc.subactionPath = hand_[h];
    sc.poseInActionSpace.orientation.w = 1.0f;
    sc.action = a_grip_;
    r = xrCreateActionSpace(session_, &sc, &grip_space_[h]);
    if (XR_FAILED(r)) return fail(error, "xrCreateActionSpace(grip)", r);
    sc.action = a_aim_;
    r = xrCreateActionSpace(session_, &sc, &aim_space_[h]);
    if (XR_FAILED(r)) return fail(error, "xrCreateActionSpace(aim)", r);
  }
  XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  at.countActionSets = 1;
  at.actionSets = &set_;
  r = xrAttachSessionActionSets(session_, &at);
  if (XR_FAILED(r)) return fail(error, "xrAttachSessionActionSets", r);
  return true;
}

bool OpenXrSession::begin(std::string* error) {
  end();
  if (!available_) {
    if (error) *error = reason_;
    return false;
  }
#if defined(_WIN32)
  std::string why;
  if (!wglGetCurrentContext() || !render3d::load_gl33(&why)) {
    if (error) *error = "no current OpenGL 3.3 context for the headset" + (why.empty() ? "" : ": " + why);
    return false;
  }
  PFN_xrGetOpenGLGraphicsRequirementsKHR get_req = nullptr;
  XrResult r = xrGetInstanceProcAddr(instance_, "xrGetOpenGLGraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&get_req));
  if (XR_FAILED(r) || !get_req) return fail(error, "xrGetOpenGLGraphicsRequirementsKHR", r);
  XrGraphicsRequirementsOpenGLKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
  r = get_req(instance_, system_, &req);
  if (XR_FAILED(r)) return fail(error, "OpenGL graphics requirements", r);
  if (XR_MAKE_VERSION(3, 3, 0) < req.minApiVersionSupported) {
    std::cerr << "[ts_xr] runtime asks for a newer OpenGL than 3.3; trying anyway\n";
  }
  XrGraphicsBindingOpenGLWin32KHR bind{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
  bind.hDC = wglGetCurrentDC();
  bind.hGLRC = wglGetCurrentContext();
  XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
  ci.next = &bind;
  ci.systemId = system_;
  r = xrCreateSession(instance_, &ci, &session_);
  if (XR_FAILED(r)) {
    session_ = XR_NULL_HANDLE;
    return fail(error, "xrCreateSession", r);
  }
  // Floor-level stage space when the runtime has one (SteamVR does).
  std::uint32_t n = 0;
  xrEnumerateReferenceSpaces(session_, 0, &n, nullptr);
  std::vector<XrReferenceSpaceType> spaces(n);
  xrEnumerateReferenceSpaces(session_, n, &n, spaces.data());
  stage_ = std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();
  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.poseInReferenceSpace.orientation.w = 1.0f;
  rs.referenceSpaceType = stage_ ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
  r = xrCreateReferenceSpace(session_, &rs, &app_space_);
  if (XR_FAILED(r)) return fail(error, "xrCreateReferenceSpace(app)", r);
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  r = xrCreateReferenceSpace(session_, &rs, &view_space_);
  if (XR_FAILED(r)) return fail(error, "xrCreateReferenceSpace(view)", r);
  // Views + swapchains.
  std::uint32_t vc = 0;
  xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                    0, &vc, nullptr);
  std::vector<XrViewConfigurationView> vcv(vc, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
  r = xrEnumerateViewConfigurationViews(instance_, system_,
                                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc, vcv.data());
  if (XR_FAILED(r) || vc < 2) return fail(error, "stereo view configuration", r);
  w_ = static_cast<int>(vcv[0].recommendedImageRectWidth);
  h_ = static_cast<int>(vcv[0].recommendedImageRectHeight);
  std::uint32_t fc = 0;
  xrEnumerateSwapchainFormats(session_, 0, &fc, nullptr);
  std::vector<std::int64_t> formats(fc);
  xrEnumerateSwapchainFormats(session_, fc, &fc, formats.data());
  format_ = formats.empty() ? 0 : formats[0];
  // The renderer writes display-ready (gamma-encoded) colour, so an sRGB
  // swapchain shows it as is (the copy does no conversion).
  for (std::int64_t want : {static_cast<std::int64_t>(0x8C43), static_cast<std::int64_t>(0x8058)}) {
    if (std::find(formats.begin(), formats.end(), want) != formats.end()) {
      format_ = want;
      break;
    }
  }
  for (int e = 0; e < 2; ++e) {
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = format_;
    sc.sampleCount = 1;
    sc.width = static_cast<std::uint32_t>(w_);
    sc.height = static_cast<std::uint32_t>(h_);
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    r = xrCreateSwapchain(session_, &sc, &swapchain_[e]);
    if (XR_FAILED(r)) return fail(error, "xrCreateSwapchain", r);
    std::uint32_t ic = 0;
    xrEnumerateSwapchainImages(swapchain_[e], 0, &ic, nullptr);
    images_[e].assign(ic, XrSwapchainImageOpenGLKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    r = xrEnumerateSwapchainImages(swapchain_[e], ic, &ic,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images_[e].data()));
    if (XR_FAILED(r)) return fail(error, "xrEnumerateSwapchainImages", r);
    if (!eyes_[e].create(w_, h_)) {
      if (error) *error = "eye framebuffer incomplete";
      end();
      return false;
    }
  }
  render3d::gl33().GenFramebuffers(1, &copy_fbo_);
  if (!create_actions(error)) return false;
  views_.assign(2, XrView{XR_TYPE_VIEW});
  exit_ = false;
  std::cout << "[ts_xr] OpenXR session on " << system_name() << ": " << swapchain_text()
            << (stage_ ? ", stage space" : ", local space (floor estimated)") << '\n';
  return true;
#else
  if (error) *error = reason_;
  return false;
#endif
}

void OpenXrSession::end() {
  if (session_ != XR_NULL_HANDLE) {
    for (int h = 0; h < 2; ++h) {
      if (grip_space_[h] != XR_NULL_HANDLE) xrDestroySpace(grip_space_[h]);
      if (aim_space_[h] != XR_NULL_HANDLE) xrDestroySpace(aim_space_[h]);
      grip_space_[h] = aim_space_[h] = XR_NULL_HANDLE;
    }
    for (int e = 0; e < 2; ++e) {
      if (swapchain_[e] != XR_NULL_HANDLE) xrDestroySwapchain(swapchain_[e]);
      swapchain_[e] = XR_NULL_HANDLE;
#if defined(_WIN32)
      images_[e].clear();
#endif
    }
    if (app_space_ != XR_NULL_HANDLE) xrDestroySpace(app_space_);
    if (view_space_ != XR_NULL_HANDLE) xrDestroySpace(view_space_);
    app_space_ = view_space_ = XR_NULL_HANDLE;
    if (running_) xrEndSession(session_);
    xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
  }
  if (set_ != XR_NULL_HANDLE) xrDestroyActionSet(set_);
  set_ = XR_NULL_HANDLE;
  a_grip_ = a_aim_ = a_trigger_ = a_squeeze_ = a_stick_ = a_primary_ = a_secondary_ = a_menu_ =
      a_haptic_ = XR_NULL_HANDLE;
  if (glfwGetCurrentContext()) {
    for (EyeTarget& t : eyes_) {
      if (t.fbo) t.destroy();
    }
    if (copy_fbo_) render3d::gl33().DeleteFramebuffers(1, &copy_fbo_);
  }
  copy_fbo_ = 0;
  running_ = false;
  frame_begun_ = false;
  xr_state_ = XR_SESSION_STATE_UNKNOWN;
}

bool OpenXrSession::poll_events() {
  if (instance_ == XR_NULL_HANDLE) return false;
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto* e = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
      xr_state_ = e->state;
      if (e->state == XR_SESSION_STATE_READY && session_ != XR_NULL_HANDLE) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        running_ = XR_SUCCEEDED(xrBeginSession(session_, &bi));
      } else if (e->state == XR_SESSION_STATE_STOPPING && session_ != XR_NULL_HANDLE) {
        xrEndSession(session_);
        running_ = false;
      } else if (e->state == XR_SESSION_STATE_EXITING ||
                 e->state == XR_SESSION_STATE_LOSS_PENDING) {
        exit_ = true;
        running_ = false;
      }
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      exit_ = true;
      running_ = false;
    }
    ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
  }
  return !exit_;
}

void OpenXrSession::read_hand(int h, XrTime t) {
  const float lift = stage_ ? 0.0f : 1.6f;
  world3d::VrHandInput& hand = input_.hands[h];
  const XrSpaceLocationFlags ok =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_SUCCEEDED(xrLocateSpace(grip_space_[h], app_space_, t, &loc))) {
    hand.grip = to_pose(loc.pose, (loc.locationFlags & ok) == ok, lift);
  } else {
    hand.grip.valid = false;
  }
  loc = XrSpaceLocation{XR_TYPE_SPACE_LOCATION};
  if (XR_SUCCEEDED(xrLocateSpace(aim_space_[h], app_space_, t, &loc))) {
    hand.aim = to_pose(loc.pose, (loc.locationFlags & ok) == ok, lift);
  } else {
    hand.aim.valid = false;
  }
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
  gi.subactionPath = hand_[h];
  auto get_float = [&](XrAction a) {
    gi.action = a;
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(session_, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
  };
  auto get_bool = [&](XrAction a) {
    gi.action = a;
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &s)) && s.isActive &&
           s.currentState == XR_TRUE;
  };
  hand.trigger = get_float(a_trigger_);
  hand.squeeze = get_float(a_squeeze_);
  gi.action = a_stick_;
  XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
  if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v)) && v.isActive) {
    hand.stick_x = v.currentState.x;
    hand.stick_y = v.currentState.y;
  } else {
    hand.stick_x = hand.stick_y = 0.0f;
  }
  hand.primary = get_bool(a_primary_);
  hand.secondary = get_bool(a_secondary_);
  hand.menu = get_bool(a_menu_);
}

bool OpenXrSession::wait_frame(FrameInfo* out) {
  if (!running_ || session_ == XR_NULL_HANDLE) return false;
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  frame_state_ = XrFrameState{XR_TYPE_FRAME_STATE};
  if (XR_FAILED(xrWaitFrame(session_, &wi, &frame_state_))) return false;
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  if (XR_FAILED(xrBeginFrame(session_, &bi))) return false;
  frame_begun_ = true;
  should_render_ = frame_state_.shouldRender == XR_TRUE;
  const XrTime t = frame_state_.predictedDisplayTime;
  const float lift = stage_ ? 0.0f : 1.6f;
  XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
  li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  li.displayTime = t;
  li.space = app_space_;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  std::uint32_t n = 0;
  if (XR_FAILED(xrLocateViews(session_, &li, &vs, 2, &n, views_.data())) || n < 2 ||
      !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
    should_render_ = false;
  }
  if (xr_state_ == XR_SESSION_STATE_FOCUSED) {
    XrActiveActionSet aas{set_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    xrSyncActions(session_, &si);
  }
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  if (XR_SUCCEEDED(xrLocateSpace(view_space_, app_space_, t, &head))) {
    input_.head = to_pose(head.pose, (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0,
                          lift);
  }
  for (int h = 0; h < 2; ++h) read_hand(h, t);
  if (out) {
    out->predicted_display_time_ns = static_cast<long long>(t);
    out->should_render = should_render_;
    out->views.assign(2, EyeView{});
    for (int e = 0; e < 2; ++e) {
      const world3d::VrPose p = to_pose(views_[e].pose, true, lift);
      out->views[e].position = p.position;
      out->views[e].orientation = p.orientation;
      out->views[e].fov_left = views_[e].fov.angleLeft;
      out->views[e].fov_right = views_[e].fov.angleRight;
      out->views[e].fov_up = views_[e].fov.angleUp;
      out->views[e].fov_down = views_[e].fov.angleDown;
    }
  }
  return true;
}

void OpenXrSession::end_frame() {
  if (!frame_begun_) return;
  frame_begun_ = false;
  [[maybe_unused]] XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  [[maybe_unused]] XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  const XrCompositionLayerBaseHeader* layers[1] = {nullptr};
  std::uint32_t layer_count = 0;
#if defined(_WIN32)
  if (should_render_) {
    const render3d::Gl33& g = render3d::gl33();
    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_draw);
    bool ok = true;
    for (int e = 0; e < 2; ++e) {
      std::uint32_t idx = 0;
      XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      if (XR_FAILED(xrAcquireSwapchainImage(swapchain_[e], &ai, &idx))) {
        ok = false;
        continue;
      }
      XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wi.timeout = XR_INFINITE_DURATION;
      xrWaitSwapchainImage(swapchain_[e], &wi);
      g.BindFramebuffer(GL_READ_FRAMEBUFFER, eyes_[e].fbo);
      g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbo_);
      g.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             images_[e][idx].image, 0);
      g.BlitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      g.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrReleaseSwapchainImage(swapchain_[e], &ri);
      pv[e].pose = views_[e].pose;
      pv[e].fov = views_[e].fov;
      pv[e].subImage.swapchain = swapchain_[e];
      pv[e].subImage.imageRect.offset = {0, 0};
      pv[e].subImage.imageRect.extent = {w_, h_};
      pv[e].subImage.imageArrayIndex = 0;
    }
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw));
    if (ok) {
      layer.space = app_space_;
      layer.viewCount = 2;
      layer.views = pv;
      layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
      layer_count = 1;
    }
  }
#endif
  XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
  fe.displayTime = frame_state_.predictedDisplayTime;
  fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  fe.layerCount = layer_count;
  fe.layers = layer_count ? layers : nullptr;
  xrEndFrame(session_, &fe);
}

void OpenXrSession::haptic(const world3d::VrHaptic& h) {
  if (session_ == XR_NULL_HANDLE || a_haptic_ == XR_NULL_HANDLE || !running_) return;
  XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
  v.amplitude = std::clamp(h.amplitude, 0.0f, 1.0f);
  v.duration = static_cast<XrDuration>(static_cast<double>(h.seconds) * 1e9);
  v.frequency = XR_FREQUENCY_UNSPECIFIED;
  XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
  hi.action = a_haptic_;
  hi.subactionPath = hand_[h.hand & 1];
  xrApplyHapticFeedback(session_, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
}

}  // namespace

std::unique_ptr<XrSession> create_openxr_session(const XrConfig& config) {
  return std::make_unique<OpenXrSession>(config);
}

bool openxr_built() { return true; }

XrProbe probe_openxr(bool create_session) {
  XrProbe p;
  p.loader = true;
  XrConfig cfg;
  cfg.app_name = "TombStone xr-probe";
  OpenXrSession s(cfg);
  p.instance = s.instance_ok();
  p.system = s.system_ok();
  p.runtime = s.runtime();
  p.system_name = s.system_label();
  if (!s.available()) p.error = s.unavailable_reason();
  if (create_session && s.available()) {
    std::string err;
    p.session = s.begin(&err);
    if (p.session) {
      p.swapchain = s.swapchain_text();
    } else {
      p.error = err;
    }
    s.end();
  }
  return p;
}

}  // namespace xr
}  // namespace tombstone
}  // namespace ts

#else  // !TS_HAVE_OPENXR

namespace ts {
namespace tombstone {
namespace xr {

namespace {

class NoOpenXr final : public XrSession {
 public:
  NoOpenXr() : reason_("built without OpenXR (TS_WITH_OPENXR=OFF)") {}
  const char* runtime_name() const override { return "OpenXR"; }
  bool simulated() const override { return false; }
  bool available() const override { return false; }
  const std::string& unavailable_reason() const override { return reason_; }
  bool begin(std::string* error) override {
    if (error) *error = reason_;
    return false;
  }
  void end() override {}
  SessionState state() const override { return SessionState::Unknown; }
  bool poll_events() override { return false; }
  bool running() const override { return false; }
  bool wait_frame(FrameInfo*) override { return false; }
  const world3d::VrInput& input() const override { return input_; }
  int eye_width() const override { return 0; }
  int eye_height() const override { return 0; }
  bool bind_eye(int) override { return false; }
  void end_frame() override {}
  std::uint32_t eye_texture(int) const override { return 0; }
  std::uint32_t eye_framebuffer(int) const override { return 0; }

 private:
  std::string reason_;
  world3d::VrInput input_;
};

}  // namespace

std::unique_ptr<XrSession> create_openxr_session(const XrConfig&) {
  return std::make_unique<NoOpenXr>();
}

bool openxr_built() { return false; }

XrProbe probe_openxr(bool) {
  XrProbe p;
  p.error = "built without OpenXR (TS_WITH_OPENXR=OFF)";
  return p;
}

}  // namespace xr
}  // namespace tombstone
}  // namespace ts

#endif
