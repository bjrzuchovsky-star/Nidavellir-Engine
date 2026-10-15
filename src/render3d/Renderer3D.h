#pragma once

// 3D renderer interface. Two backends:
//   null - accepts and validates everything and counts what a frame would
//          draw (headless tools, tests without a GPU)
//   gl   - OpenGL 3.3 core forward renderer (GlRenderer3D.h): PBR-lite
//          materials, a shadowed sun, point lights, gradient sky, debug
//          lines, optional offscreen target.

#include "render3d/Camera3D.h"
#include "render3d/Mesh.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct FrameStats {
  std::size_t draws = 0;
  std::size_t triangles = 0;
  std::size_t lights = 0;
  std::size_t shadow_casters = 0;
  std::size_t lines = 0;
};

// Sky, ambient and sun for a frame.
struct Environment {
  float sky_top[3] = {0.16f, 0.22f, 0.36f};
  float sky_horizon[3] = {0.93f, 0.62f, 0.38f};
  float ground[3] = {0.24f, 0.19f, 0.15f};
  float ambient = 0.35f;
  Vec3 sun_direction{-0.45f, -0.8f, -0.35f};  // direction light travels
  float sun_color[3] = {1.0f, 0.88f, 0.70f};
  float sun_intensity = 2.6f;
  bool shadows = true;
  float shadow_distance = 40.0f;  // half-size of the sun's shadow box
  bool operator==(const Environment& o) const = default;
};

struct PointLight {
  Vec3 position;
  float color[3] = {1.0f, 0.75f, 0.45f};
  float intensity = 6.0f;
  float range = 8.0f;
};

class Renderer3D {
 public:
  static constexpr std::size_t kMaxPointLights = 8;

  virtual ~Renderer3D() = default;
  virtual const char* backend_name() const = 0;  // "null", "gl"
  virtual bool init(std::string* error) = 0;
  virtual void shutdown() = 0;

  virtual MeshHandle create_mesh(const MeshData& mesh) = 0;
  virtual MaterialHandle create_material(const MaterialDesc& material) = 0;
  virtual void destroy_mesh(MeshHandle h) = 0;
  virtual void destroy_material(MaterialHandle h) = 0;

  // Offscreen (true): draw into the renderer's own color+depth target of
  // the frame size (editor viewport, tests); false: the bound framebuffer.
  virtual void set_offscreen(bool /*on*/) {}
  virtual void set_environment(const Environment& /*env*/) {}

  virtual void begin_frame(const Camera3D& camera, int width, int height) = 0;
  virtual void add_point_light(const PointLight& /*light*/) {}
  virtual void draw(MeshHandle mesh, MaterialHandle material,
                    const Transform& transform) = 0;
  virtual void draw_matrix(MeshHandle mesh, MaterialHandle material,
                           const Mat4& world, bool cast_shadow = true) {
    (void)world;
    (void)cast_shadow;
    draw(mesh, material, Transform{});
  }
  // Unlit lines (grid, gizmos, colliders); depth_test false = on top.
  virtual void draw_line(const Vec3& /*a*/, const Vec3& /*b*/,
                         const float* /*rgba*/, bool /*depth_test*/ = true) {}
  // Camera-facing soft particles for this frame (fx::ParticleWorld in 3D):
  // depth-tested, no depth writes; alpha ones first, then additive.
  struct Billboard {
    Vec3 position;
    float size = 0.1f;      // metres across
    float rotation = 0.0f;  // degrees
    float rgba[4] = {1, 1, 1, 1};
    bool additive = false;
  };
  virtual void draw_billboards(const std::vector<Billboard>& /*billboards*/) {}
  virtual FrameStats end_frame() = 0;
  // Screen-space overlay (the in-game UI raster): RGBA8, top row first,
  // straight alpha, stretched over the whole frame after the scene. Kept
  // until replaced; re-uploaded only when `version` changes. nullptr clears.
  virtual void set_overlay(const unsigned char* /*rgba*/, int /*width*/, int /*height*/,
                           std::uint64_t /*version*/) {}

  // Offscreen color texture (GL name; 0 when none) and its pixels, RGBA8
  // bottom row first. False when there is nothing to read.
  virtual std::uint64_t color_texture() const { return 0; }
  virtual bool read_pixels(std::vector<unsigned char>* /*rgba*/, int* /*w*/,
                           int* /*h*/) const {
    return false;
  }
};

std::unique_ptr<Renderer3D> make_null_renderer3d();

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
