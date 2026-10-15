#pragma once

// GL pieces for terrain and foliage, kept apart from GlRenderer3D so the
// lighting / shadow work there can change without touching them:
//   - GLSL modules spliced into the mesh program at the "//@ts:surface"
//     marker: ts_surface_albedo() (terrain splat: four tiled layers) and
//     ts_apply_fog() (distance + height fog), plus the sky's ts_sky_fog().
//   - FoliagePass: instanced foliage (own small program: sun + shadow map +
//     hemisphere ambient + fog, wind sway, dithered distance fade) and its
//     shadow-map draw.
// GlRenderer3D calls in at a handful of points (see the TS_TERRAIN notes).

#include "render3d/GlApi.h"
#include "render3d/Renderer3D.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
namespace gl_terrain {

inline constexpr int kSplatUnit = 4;       // units 4..8: splat + layers
inline constexpr int kFirstLayerUnit = 5;
inline constexpr const char* kSurfaceMarker = "//@ts:surface";

// The mesh fragment shader with the modules spliced in at the marker.
std::string mesh_fs(const char* base);
// The sky fragment shader's fog hook ("//@ts:sky" marker before o_color).
std::string sky_fs(const char* base);

// Per-material GPU textures of a terrain material (0 = none).
struct TerrainTextures {
  GLuint splat = 0;
  GLuint layers[4] = {0, 0, 0, 0};
  std::size_t bytes = 0;
  std::size_t count = 0;
};
// Upload a terrain material's images (make_texture: rgba, w, h, mipmaps).
using MakeTexture = GLuint (*)(const unsigned char* rgba, int w, int h, bool mipmaps);
TerrainTextures upload(const MaterialDesc& d, MakeTexture make);
void update_splat(TerrainTextures& t, const Image& splat);
void release(TerrainTextures& t);

// Once per frame on a program using the modules (mesh) or the sky.
void set_frame_uniforms(GLuint prog, const Environment& env, const Vec3& camera);
// Per draw: terrain textures + uniforms, or u_terrain = 0.
void bind_material(GLuint prog, const MaterialDesc& d, const TerrainTextures* t, GLuint white);

struct FrameContext {
  Mat4 view_proj;
  Mat4 light_vp;
  Vec3 camera;
  Environment env;
  GLuint shadow_tex = 0;  // depth compare texture
  bool shadows = false;
  float shadow_texel = 1.0f / 2048.0f;
  int width = 1, height = 1;
};

class FoliagePass {
 public:
  bool init(std::string* error);
  void shutdown();
  void begin_frame();
  // A batch for the frame; vbo / ibo / count are the GPU mesh.
  void add(const FoliageBatch& batch, GLuint vbo, GLuint ibo, GLsizei count);
  bool empty() const { return batches_.empty(); }
  // Into the bound shadow framebuffer (depth only).
  std::size_t draw_shadows(const Mat4& light_vp, const Environment& env);
  // Into the main target after the opaque meshes. Returns instances drawn.
  std::size_t draw(const FrameContext& ctx);
  void forget_mesh(GLuint vbo);

 private:
  struct Batch {
    FoliageBatch b;
    GLuint vbo = 0, ibo = 0;
    GLsizei count = 0;
    std::size_t first = 0;  // into instances_
  };
  void upload();
  GLuint vao_for(GLuint vbo, GLuint ibo);
  void bind_instances(GLuint vao, std::size_t first);
  void set_batch_uniforms(GLuint prog, const Batch& b);

  GLuint prog_ = 0, depth_prog_ = 0, inst_vbo_ = 0;
  std::unordered_map<GLuint, GLuint> vaos_;  // mesh vbo -> vao
  std::vector<Batch> batches_;
  std::vector<FoliageInstance3D> instances_;
  bool uploaded_ = false;
};

}  // namespace gl_terrain
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
