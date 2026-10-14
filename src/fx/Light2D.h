#pragma once
// 2D lighting: point / spot light components, scene ambient with an
// optional day-night cycle, and a CPU lightmap with soft shadows cast by
// solid tiles. The lightmap is a small RGBA image (published as a "mem:"
// key) the hosts stretch over the view with 2x multiply blending, so
// 128 (0.5) is neutral, darker values shade and brighter ones glow.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace fx {

struct Light2DData {
  enum class Kind { Point, Spot };
  Kind kind = Kind::Point;
  float color[3] = {1.0f, 0.85f, 0.6f};
  float intensity = 1.0f;   // 0..4
  float radius = 220.0f;    // pixels
  float angle = 0.0f;       // spot direction, degrees clockwise from +x
  float cone = 60.0f;       // spot full width, degrees
  float softness = 0.5f;    // penumbra (0 hard .. 1 very soft)
  bool shadows = true;
  float flicker = 0.0f;     // 0..1 random intensity wobble
  bool operator==(const Light2DData& o) const = default;
};
void normalize(Light2DData& l);

// Scene-level settings (scene.json "lighting").
struct Lighting2D {
  bool enabled = false;
  float ambient[3] = {1.0f, 1.0f, 1.0f};      // day / fixed ambient
  bool day_night = false;
  float night_ambient[3] = {0.12f, 0.13f, 0.25f};
  float day_length = 240.0f;   // seconds for a whole day
  float start_time = 0.25f;    // 0 midnight, 0.25 dawn, 0.5 noon, 0.75 dusk
  int resolution = 4;          // world pixels per lightmap texel (2..16)
  bool operator==(const Lighting2D& o) const = default;
  // Ambient at `seconds` into the ride, and how dark it is (0 day .. 1 night).
  void ambient_at(double seconds, float out[3], float* night = nullptr) const;
};
void normalize(Lighting2D& l);

struct LightInstance {
  float x = 0, y = 0;  // world pixels
  Light2DData data;
};

// Solid cells that block light: a uniform grid of `cell` px.
struct Occluders {
  float cell = 32.0f;
  std::function<bool(int cx, int cy)> solid;
};

struct LightmapView {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // world rect
};

struct Lightmap {
  int width = 0, height = 0;
  std::vector<unsigned char> rgba;  // 128 = 1.0 (2x multiply)
  LightmapView view;
};

// Renders the lightmap for view. texel = world px per texel. seed varies
// flicker. Returns the light reaching the centre of each texel.
void render_lightmap(const LightmapView& view, float texel, const float ambient[3],
                     const std::vector<LightInstance>& lights, const Occluders& occ,
                     std::uint32_t flicker_seed, Lightmap* out);

// Light at one world point (smokes and gameplay checks).
void light_at(float x, float y, const float ambient[3], const std::vector<LightInstance>& lights,
              const Occluders& occ, float out[3]);

inline constexpr const char* kLightmapImage = "mem:fx/lightmap";

}  // namespace fx
}  // namespace ts
