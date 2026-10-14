#include "fx/Light2D.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace fx {
namespace {

constexpr float kPi = 3.14159265358979f;

// True when the segment from (ax,ay) to (bx,by) crosses a solid cell other
// than the one holding a (lit faces of walls stay lit) or b (the light).
bool blocked(float ax, float ay, float bx, float by, const Occluders& occ) {
  if (!occ.solid) return false;
  const float c = occ.cell;
  int cx = static_cast<int>(std::floor(ax / c)), cy = static_cast<int>(std::floor(ay / c));
  const int ex = static_cast<int>(std::floor(bx / c)), ey = static_cast<int>(std::floor(by / c));
  const float dx = bx - ax, dy = by - ay;
  const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
  const float inv_x = dx != 0 ? std::fabs(c / dx) : 1e30f;
  const float inv_y = dy != 0 ? std::fabs(c / dy) : 1e30f;
  float tx = dx != 0 ? ((sx > 0 ? (cx + 1) * c - ax : ax - cx * c) / std::fabs(dx)) : 1e30f;
  float ty = dy != 0 ? ((sy > 0 ? (cy + 1) * c - ay : ay - cy * c) / std::fabs(dy)) : 1e30f;
  const bool start_solid = occ.solid(cx, cy);
  for (int steps = 0; steps < 256; ++steps) {
    if (cx == ex && cy == ey) return false;
    if (tx < ty) {
      cx += sx;
      tx += inv_x;
    } else {
      cy += sy;
      ty += inv_y;
    }
    if (cx == ex && cy == ey) return false;
    if (occ.solid(cx, cy)) {
      // Leaving a wall we started in does not shadow ourselves.
      if (start_solid) continue;
      return true;
    }
  }
  return false;
}

float hash01(std::uint32_t v) {
  v ^= v >> 16;
  v *= 0x7feb352du;
  v ^= v >> 15;
  v *= 0x846ca68bu;
  v ^= v >> 16;
  return static_cast<float>(v >> 8) / 16777216.0f;
}

void accumulate(float x, float y, const LightInstance& L, float gain, const Occluders& occ,
                float out[3]) {
  const Light2DData& d = L.data;
  const float dx = x - L.x, dy = y - L.y;
  const float dist2 = dx * dx + dy * dy;
  if (dist2 >= d.radius * d.radius) return;
  const float dist = std::sqrt(dist2);
  float f = 1.0f - dist / d.radius;
  f = f * f;
  if (d.kind == Light2DData::Kind::Spot && dist > 0.5f) {
    const float a = std::atan2(dy, dx) * 180.0f / kPi;
    float diff = std::fabs(std::remainder(a - d.angle, 360.0f));
    const float half = d.cone * 0.5f;
    const float edge = std::max(2.0f, half * 0.25f);
    if (diff > half + edge) return;
    if (diff > half - edge) f *= std::clamp((half + edge - diff) / (2 * edge), 0.0f, 1.0f);
  }
  if (d.shadows && occ.solid) {
    // Soft shadows: a few rays to points spread across the light's size.
    const float spread = d.softness * occ.cell * 0.75f;
    if (spread < 0.5f) {
      if (blocked(x, y, L.x, L.y, occ)) return;
    } else {
      const float px = -dy / std::max(dist, 1e-3f), py = dx / std::max(dist, 1e-3f);
      int lit = 0;
      static const float offs[3] = {-1.0f, 0.0f, 1.0f};
      for (float o : offs) {
        if (!blocked(x, y, L.x + px * spread * o, L.y + py * spread * o, occ)) ++lit;
      }
      if (lit == 0) return;
      f *= static_cast<float>(lit) / 3.0f;
    }
  }
  f *= d.intensity * gain;
  for (int i = 0; i < 3; ++i) out[i] += d.color[i] * f;
}

}  // namespace

void normalize(Light2DData& l) {
  for (float& c : l.color) c = std::clamp(c, 0.0f, 1.0f);
  l.intensity = std::clamp(l.intensity, 0.0f, 4.0f);
  l.radius = std::clamp(l.radius, 1.0f, 4096.0f);
  l.angle = std::remainder(l.angle, 360.0f);
  l.cone = std::clamp(l.cone, 1.0f, 360.0f);
  l.softness = std::clamp(l.softness, 0.0f, 1.0f);
  l.flicker = std::clamp(l.flicker, 0.0f, 1.0f);
}

void normalize(Lighting2D& l) {
  for (float& c : l.ambient) c = std::clamp(c, 0.0f, 2.0f);
  for (float& c : l.night_ambient) c = std::clamp(c, 0.0f, 2.0f);
  l.day_length = std::clamp(l.day_length, 5.0f, 86400.0f);
  l.start_time = l.start_time - std::floor(l.start_time);
  l.resolution = std::clamp(l.resolution, 2, 16);
}

void Lighting2D::ambient_at(double seconds, float out[3], float* night) const {
  float k = 0.0f;
  if (day_night) {
    double t = start_time + seconds / day_length;
    t -= std::floor(t);
    // Daylight: a cosine with flat noon and midnight (bright 0.3..0.7).
    const float sun = static_cast<float>(-std::cos(t * 2.0 * kPi));
    k = std::clamp(0.5f - sun * 1.2f, 0.0f, 1.0f);
  }
  for (int i = 0; i < 3; ++i) out[i] = ambient[i] + (night_ambient[i] - ambient[i]) * k;
  if (night) *night = k;
}

void light_at(float x, float y, const float ambient[3], const std::vector<LightInstance>& lights,
              const Occluders& occ, float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = ambient[i];
  for (const LightInstance& L : lights) accumulate(x, y, L, 1.0f, occ, out);
}

void render_lightmap(const LightmapView& view, float texel, const float ambient[3],
                     const std::vector<LightInstance>& lights, const Occluders& occ,
                     std::uint32_t flicker_seed, Lightmap* out) {
  texel = std::max(texel, 1.0f);
  // Snap to the texel grid so the map does not shimmer while panning.
  const float x0 = std::floor(view.x0 / texel) * texel - texel;
  const float y0 = std::floor(view.y0 / texel) * texel - texel;
  const int w = std::clamp(static_cast<int>(std::ceil((view.x1 - x0) / texel)) + 2, 1, 1024);
  const int h = std::clamp(static_cast<int>(std::ceil((view.y1 - y0) / texel)) + 2, 1, 1024);
  out->width = w;
  out->height = h;
  out->view = {x0, y0, x0 + w * texel, y0 + h * texel};
  // Only lights that can reach the view, with this frame's flicker.
  std::vector<LightInstance> near;
  std::vector<float> gains;
  for (std::size_t i = 0; i < lights.size(); ++i) {
    const LightInstance& L = lights[i];
    const float r = L.data.radius;
    if (L.x + r < out->view.x0 || L.x - r > out->view.x1 || L.y + r < out->view.y0 ||
        L.y - r > out->view.y1) {
      continue;
    }
    near.push_back(L);
    gains.push_back(1.0f - L.data.flicker * 0.35f *
                               hash01(flicker_seed * 31u + static_cast<std::uint32_t>(i)));
  }
  std::vector<float> acc(static_cast<std::size_t>(w) * h * 3);
  for (int ty = 0; ty < h; ++ty) {
    for (int tx = 0; tx < w; ++tx) {
      const float x = x0 + (tx + 0.5f) * texel, y = y0 + (ty + 0.5f) * texel;
      float c[3] = {ambient[0], ambient[1], ambient[2]};
      for (std::size_t i = 0; i < near.size(); ++i) accumulate(x, y, near[i], gains[i], occ, c);
      float* a = &acc[(static_cast<std::size_t>(ty) * w + tx) * 3];
      a[0] = c[0];
      a[1] = c[1];
      a[2] = c[2];
    }
  }
  // One 3x3 blur pass softens texel edges and shadow steps.
  out->rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
  for (int ty = 0; ty < h; ++ty) {
    for (int tx = 0; tx < w; ++tx) {
      float s[3] = {0, 0, 0};
      float n = 0;
      for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
          const int sx = std::clamp(tx + ox, 0, w - 1), sy = std::clamp(ty + oy, 0, h - 1);
          const float k = (ox == 0 && oy == 0) ? 4.0f : (ox == 0 || oy == 0) ? 2.0f : 1.0f;
          const float* a = &acc[(static_cast<std::size_t>(sy) * w + sx) * 3];
          for (int i = 0; i < 3; ++i) s[i] += a[i] * k;
          n += k;
        }
      }
      unsigned char* p = &out->rgba[(static_cast<std::size_t>(ty) * w + tx) * 4];
      for (int i = 0; i < 3; ++i) {
        p[i] = static_cast<unsigned char>(std::lround(std::clamp(s[i] / n * 0.5f, 0.0f, 1.0f) * 255));
      }
    }
  }
}

}  // namespace fx
}  // namespace ts
