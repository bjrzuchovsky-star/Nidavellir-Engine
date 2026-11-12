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
  std::size_t skinned = 0;  // draws posed by a bone palette
  std::size_t foliage = 0;  // instanced foliage drawn (instances)
  // Live resources (profiler / Assay Office): meshes, materials, textures
  // and an estimate of their GPU memory.
  std::size_t meshes = 0;
  std::size_t materials = 0;
  std::size_t textures = 0;
  std::size_t gpu_bytes = 0;
};

// Modular lighting. Defaults reproduce the classic forward frame (one
// shadowed sun, Reinhard in the mesh shader, no extra passes).
struct Lighting {
  int preset = 0;          // 0 classic, 1 fast, 2 balanced, 3 cinematic, 4 custom
  int cascades = 1;        // 1..4
  int filter = 0;          // 0 PCF, 1 PCSS
  int gi = 0;              // 0 none, 1 baked probes, 2 screen-space, 3 ray traced
  int ao = 0;              // 0 off, 1 SSAO, 2 RTAO
  int reflections = 0;     // 0 off, 1 probes, 2 SSR, 3 ray traced
  int tonemap = 0;         // 0 Reinhard in-shader, 1 Reinhard post, 2 ACES
  float exposure = 1.0f;
  float bloom = 0.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float gain = 1.0f;
  float pom = 0.0f;        // parallax scale, 0 off
  float pcss = 0.08f;
  bool point_shadows = false;
  bool spot_shadows = false;
  bool raytracing = false; // off unless a scene asks, and only if the GPU can
  std::string hdri;
  std::string modules;     // plugin passes: "id=1,other=0"
  bool operator==(const Lighting& o) const = default;
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
  // Distance fog (display colour); density per metre, thinning above
  // fog_height by fog_falloff per metre (0 = uniform). Weather drives these.
  float fog_color[3] = {0.72f, 0.74f, 0.78f};
  float fog_density = 0.0f;
  float fog_height = 0.0f;
  float fog_falloff = 0.0f;
  // Wind for foliage sway and particles (xz direction, 0..1+ strength).
  float wind_dir[2] = {1.0f, 0.0f};
  float wind_strength = 0.0f;
  float wind_gust = 0.0f;
  Lighting lighting{};
  bool operator==(const Environment& o) const = default;
};

struct PointLight {
  Vec3 position;
  float color[3] = {1.0f, 0.75f, 0.45f};
  float intensity = 6.0f;
  float range = 8.0f;
};

// Cone along `direction` (the direction light travels). outer_cos / inner_cos
// are cosines of the half-angles.
struct SpotLight {
  Vec3 position;
  Vec3 direction{0, -1, 0};
  float color[3] = {1.0f, 0.9f, 0.7f};
  float intensity = 8.0f;
  float range = 12.0f;
  float outer_cos = 0.8f;
  float inner_cos = 0.9f;
};

// A flat rectangle centred at position, spanned by axis_u and axis_v.
struct AreaLight {
  Vec3 position;
  Vec3 axis_u{0.5f, 0, 0};
  Vec3 axis_v{0, 0, 0.5f};
  float color[3] = {1.0f, 0.95f, 0.85f};
  float intensity = 4.0f;
};

// One instanced foliage batch (terrain/Foliage.h): a mesh repeated at
// many ground points, wind-swayed and distance-faded on the GPU.
struct FoliageInstance3D {
  float x = 0, y = 0, z = 0, yaw = 0;  // world position, radians about +y
  float scale = 1, fade = 1;            // fade 1 solid .. 0 gone (dithered)
};
struct FoliageBatch {
  MeshHandle mesh;
  float color_a[3] = {0.2f, 0.3f, 0.1f};  // built-in meshes: uv.x blends a -> b
  float color_b[3] = {0.5f, 0.6f, 0.25f};
  float sway = 1.0f;
  float mesh_height = 0.0f;  // > 0: sway weight = y / height (project meshes), else uv.y
  bool part_colors = true;   // false: flat color_b (project meshes)
  bool cast_shadow = false;
  float time = 0.0f;         // seconds (wind animation)
  const FoliageInstance3D* instances = nullptr;
  std::size_t count = 0;
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
  virtual void add_spot_light(const SpotLight& /*light*/) {}
  virtual void add_area_light(const AreaLight& /*light*/) {}
  virtual void draw(MeshHandle mesh, MaterialHandle material,
                    const Transform& transform) = 0;
  virtual void draw_matrix(MeshHandle mesh, MaterialHandle material,
                           const Mat4& world, bool cast_shadow = true) {
    (void)world;
    (void)cast_shadow;
    draw(mesh, material, Transform{});
  }
  // A skinned mesh (MeshData::skin) posed by `palette` (joint * inverse
  // bind, up to kMaxBones), then placed by `world`. Backends without
  // skinning draw the bind pose.
  virtual void draw_skinned(MeshHandle mesh, MaterialHandle material, const Mat4& world,
                            const Mat4* palette, std::size_t bones, bool cast_shadow = true) {
    (void)palette;
    (void)bones;
    draw_matrix(mesh, material, world, cast_shadow);
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
    Vec3 stretch;           // non-zero: a streak from position along it (rain)
  };
  virtual void draw_billboards(const std::vector<Billboard>& /*billboards*/) {}
  // Instanced foliage (copied; pointers need only live for the call).
  // Backends without instancing draw each instance as a plain mesh.
  virtual void draw_foliage(const FoliageBatch& batch);
  // Replace a terrain material's splat weights (same size) after painting.
  virtual void update_splat(MaterialHandle /*material*/, const Image& /*splat*/) {}
  virtual FrameStats end_frame() = 0;
  // Screen-space overlay (the in-game UI raster): RGBA8, top row first,
  // straight alpha, stretched over the whole frame after the scene. Kept
  // until replaced; re-uploaded only when `version` changes. nullptr clears.
  virtual void set_overlay(const unsigned char* /*rgba*/, int /*width*/, int /*height*/,
                           std::uint64_t /*version*/) {}

  // Offscreen color texture (GL name; 0 when none) and its pixels, RGBA8
  // bottom row first. False when there is nothing to read.
  virtual std::uint64_t color_texture() const { return 0; }
  // True when row 0 of color_texture() is the bottom of the image (OpenGL).
  virtual bool color_bottom_up() const { return true; }
  virtual bool read_pixels(std::vector<unsigned char>* /*rgba*/, int* /*w*/,
                           int* /*h*/) const {
    return false;
  }
};

std::unique_ptr<Renderer3D> make_null_renderer3d();

// Hand a frame's stats to the profiler (render.* / assets.* / mem.gpu_mb
// counters); no-op while it is off.
void report_frame_stats(const FrameStats& stats);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
