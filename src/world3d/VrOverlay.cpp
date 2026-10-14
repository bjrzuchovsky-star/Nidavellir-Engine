#include "world3d/VrOverlay.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace world3d {

using render3d::Quat;
using scene3d::Entity3D;

namespace {

Entity3D shape(std::uint64_t* id, const char* name, const char* prim, const Vec3& pos,
               const Quat& rot, const Vec3& scale, float r, float g, float b, float glow = 0.0f) {
  Entity3D e;
  e.id = (*id)++;
  e.name = name;
  e.transform.position = pos;
  e.transform.rotation = rot;
  e.transform.scale = scale;
  e.mesh = scene3d::MeshRef{std::string("primitive:") + prim, false};
  scene3d::Material3D m;
  m.albedo[0] = r;
  m.albedo[1] = g;
  m.albedo[2] = b;
  m.roughness = 0.5f;
  m.emissive[0] = r * glow;
  m.emissive[1] = g * glow;
  m.emissive[2] = b * glow;
  e.material = m;
  return e;
}

}  // namespace

void vr_overlay_entities(const World3D& w, bool vignette, std::vector<Entity3D>* out) {
  std::uint64_t id = kVrOverlayIdBase;
  // Controllers: a handle along the grip, a head ring at the aim point.
  for (int h = 0; h < 2; ++h) {
    const VrHandState& hs = w.hand(h);
    if (!hs.grip.valid) {
      id += 3;
      continue;
    }
    const Quat q = hs.grip.orientation;
    const float tint = 0.25f + 0.75f * std::max(hs.trigger, hs.squeeze);
    const float base[3] = {h == kLeftHand ? 0.20f : 0.70f, 0.22f, h == kLeftHand ? 0.70f : 0.20f};
    // Handle tilted forward like a real grip.
    const Quat tilt = q * Quat::axis_angle({1, 0, 0}, render3d::radians(-20.0f));
    out->push_back(shape(&id, h == kLeftHand ? "Left Controller" : "Right Controller", "capsule",
                         hs.grip.position + tilt.rotate({0, 0, 0.02f}), tilt,
                         {0.045f, 0.13f, 0.045f}, base[0], base[1], base[2], 0.1f));
    const Vec3 tip = hs.aim.valid ? hs.aim.position : hs.grip.position + q.rotate({0, 0.05f, -0.06f});
    out->push_back(shape(&id, "Controller Head", "sphere", tip, q, {0.06f, 0.06f, 0.06f},
                         base[0] * 0.6f + 0.2f, base[1] + 0.2f, base[2] * 0.6f + 0.2f, tint * 0.6f));
    // Trigger nub: lights up as it is pulled.
    out->push_back(shape(&id, "Trigger", "cube", hs.grip.position + q.rotate({0, 0.0f, -0.05f}), q,
                         {0.015f, 0.03f, 0.02f}, 0.9f, 0.8f * tint, 0.2f, hs.trigger));
  }
  // Teleport marker.
  const VrTeleport& t = w.teleport();
  if (t.aiming && !t.arc.empty()) {
    const Vec3 at = t.valid ? t.target : t.arc.back();
    out->push_back(shape(&id, "Teleport Marker", "cube", at + Vec3{0, 0.02f, 0}, {},
                         {0.6f, 0.03f, 0.6f}, t.valid ? 0.2f : 0.8f, t.valid ? 0.85f : 0.2f, 0.25f,
                         0.8f));
  } else {
    ++id;
  }
  // Comfort menu.
  if (w.comfort_menu_open()) {
    const auto& menu = w.comfort_menu();
    for (std::size_t i = 0; i < menu.size(); ++i) {
      const VrMenuButton& b = menu[i];
      const bool hover = w.hand(0).menu_button == static_cast<int>(i) ||
                         w.hand(1).menu_button == static_cast<int>(i);
      const Vec3 c = b.box.center();
      const Vec3 e = b.box.extent();
      float r = b.on ? 0.2f : 0.55f, g = b.on ? 0.7f : 0.2f, bl = 0.2f;
      if (hover) {
        r += 0.3f;
        g += 0.3f;
        bl += 0.1f;
      }
      out->push_back(shape(&id, b.label.c_str(), "cube", c, {}, e, r, g, bl, 0.7f));
    }
  }
  // Vignette: dark tiles around the edge of the view, closing in with the
  // amount (moving / turning).
  const float v = w.vignette();
  if (vignette && v > 0.01f) {
    const VrPose head = w.head();
    const int n = 12;
    const float radius = 0.34f - 0.12f * v;
    for (int i = 0; i < n; ++i) {
      const float a = static_cast<float>(i) * (2.0f * render3d::kPi / static_cast<float>(n));
      const Vec3 local{std::cos(a) * radius, std::sin(a) * radius, -0.2f};
      const Quat q = head.orientation * Quat::axis_angle({0, 0, 1}, a);
      out->push_back(shape(&id, "Vignette", "cube", head.position + head.orientation.rotate(local), q,
                           {0.2f, 0.22f, 0.005f}, 0.0f, 0.0f, 0.0f));
    }
  }
}

void vr_overlay_lines(const World3D& w, render3d::Renderer3D* r) {
  if (!r) return;
  for (int h = 0; h < 2; ++h) {
    const VrHandState& hs = w.hand(h);
    if (!hs.aim.valid || hs.held) continue;
    const bool live = hs.menu_button >= 0 || hs.pointed != 0;
    const float c[4] = {live ? 1.0f : 0.85f, live ? 0.85f : 0.85f, live ? 0.2f : 0.9f, 1.0f};
    const Vec3 end = hs.laser_hit ? hs.laser_end : hs.aim.position + hs.aim.forward() * 3.0f;
    r->draw_line(hs.aim.position, end, c, true);
  }
  const VrTeleport& t = w.teleport();
  if (t.aiming) {
    const float c[4] = {t.valid ? 0.3f : 0.9f, t.valid ? 0.95f : 0.25f, 0.35f, 1.0f};
    for (std::size_t i = 1; i < t.arc.size(); ++i) r->draw_line(t.arc[i - 1], t.arc[i], c, true);
  }
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
