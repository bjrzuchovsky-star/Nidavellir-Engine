#include "render3d/GlTerrainFoliage.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

namespace ts {
namespace tombstone {
namespace render3d {
namespace gl_terrain {

namespace {

// Spliced into the mesh fragment shader (after its uniforms).
const char* kSurfaceGlsl = R"glsl(
// ---- terrain splat + fog (render3d/GlTerrainFoliage.cpp) ----
uniform int u_terrain;
uniform sampler2D u_splat;
uniform sampler2D u_layer0;
uniform sampler2D u_layer1;
uniform sampler2D u_layer2;
uniform sampler2D u_layer3;
uniform vec4 u_layer_tint[4];
uniform float u_layer_tiling[4];
uniform vec4 u_fog;         // rgb (display) + density per metre
uniform vec2 u_fog_height;  // base height, falloff per metre
uniform vec3 u_fog_cam;
vec4 ts_surface_albedo(vec4 factor, vec2 uv, vec3 world) {
  if (u_terrain == 0) return factor * texture(u_albedo_tex, uv);
  vec4 w = texture(u_splat, uv);
  w /= max(w.r + w.g + w.b + w.a, 0.0001);
  vec3 c = texture(u_layer0, world.xz / u_layer_tiling[0]).rgb * u_layer_tint[0].rgb * w.r
         + texture(u_layer1, world.xz / u_layer_tiling[1]).rgb * u_layer_tint[1].rgb * w.g
         + texture(u_layer2, world.xz / u_layer_tiling[2]).rgb * u_layer_tint[2].rgb * w.b
         + texture(u_layer3, world.xz / u_layer_tiling[3]).rgb * u_layer_tint[3].rgb * w.a;
  // Break up tiling with a second, larger-scale sample of the base layer.
  float macro = texture(u_layer0, world.xz / (u_layer_tiling[0] * 9.7)).g;
  c *= 0.85 + 0.3 * macro;
  return vec4(c * factor.rgb, 1.0);
}
float ts_fog_amount(vec3 world) {
  if (u_fog.a <= 0.0) return 0.0;
  float d = length(world - u_fog_cam);
  float h = 0.5 * (world.y + u_fog_cam.y) - u_fog_height.x;
  float k = u_fog.a * exp(-max(h, 0.0) * u_fog_height.y);
  return clamp(1.0 - exp(-d * k), 0.0, 1.0);
}
vec3 ts_apply_fog(vec3 display_color, vec3 world) {
  return mix(display_color, u_fog.rgb, ts_fog_amount(world));
}
)glsl";

const char* kSkyGlsl = R"glsl(
uniform vec4 u_fog;
vec3 ts_sky_fog(vec3 c, vec3 dir) {
  if (u_fog.a <= 0.0) return c;
  float k = clamp(u_fog.a * 60.0, 0.0, 1.0);
  float low = 1.0 - smoothstep(-0.05, 0.45, dir.y);
  return mix(c, u_fog.rgb, k * mix(0.35, 1.0, low));
}
)glsl";

const char* kFoliageVS = R"glsl(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 6) in vec4 a_inst;   // xyz, yaw
layout(location = 7) in vec2 a_inst2;  // scale, fade
uniform mat4 u_view_proj;
uniform mat4 u_light_vp;
uniform vec4 u_wind;       // dir.xz, strength, gust
uniform float u_time;
uniform float u_sway;
uniform float u_mesh_height;
out vec3 v_world;
out vec3 v_nrm;
out float v_part;
out float v_fade;
out vec4 v_light;
// Keep in step with terrain::wind_bend / sway_offset (Foliage.cpp).
float ts_wind_bend(vec2 at) {
  float ph = u_time * (1.3 + 0.7 * u_wind.z) + at.x * 0.13 + at.y * 0.11;
  return u_wind.z * (0.6 + 0.4 * sin(ph)) + u_wind.w * 0.5 * sin(ph * 2.7 + at.x * 0.3);
}
void main() {
  float c = cos(a_inst.w), s = sin(a_inst.w);
  vec3 p = a_pos * a_inst2.x;
  vec3 r = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
  vec3 n = vec3(c * a_nrm.x + s * a_nrm.z, a_nrm.y, -s * a_nrm.x + c * a_nrm.z);
  float weight = u_mesh_height > 0.0 ? clamp(a_pos.y / u_mesh_height, 0.0, 1.0) : a_uv.y;
  float k = ts_wind_bend(a_inst.xz) * u_sway * weight * weight * 0.35 * a_inst2.x;
  vec3 world = a_inst.xyz + r + vec3(u_wind.x * k, -abs(k) * 0.15, u_wind.y * k);
  v_world = world;
  v_nrm = n;
  v_part = a_uv.x;
  v_fade = a_inst2.y;
  v_light = u_light_vp * vec4(world, 1.0);
  gl_Position = u_view_proj * vec4(world, 1.0);
}
)glsl";

const char* kFoliageFS = R"glsl(#version 330 core
in vec3 v_world;
in vec3 v_nrm;
in float v_part;
in float v_fade;
in vec4 v_light;
out vec4 o_color;
uniform vec3 u_color_a;
uniform vec3 u_color_b;
uniform int u_part_colors;
uniform sampler2DShadow u_shadow;
uniform int u_shadows;
uniform float u_shadow_texel;
uniform vec3 u_sun_dir;
uniform vec3 u_sun_color;
uniform vec3 u_sky_top;
uniform vec3 u_sky_horizon;
uniform vec3 u_ground;
uniform float u_ambient;
uniform vec4 u_fog;
uniform vec2 u_fog_height;
uniform vec3 u_fog_cam;
float ts_dither(vec2 p) {
  // 4x4 Bayer.
  int x = int(mod(p.x, 4.0)), y = int(mod(p.y, 4.0));
  int i = x + y * 4;
  int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
  return (float(m[i]) + 0.5) / 16.0;
}
float shadow_term() {
  if (u_shadows == 0) return 1.0;
  vec3 p = v_light.xyz / v_light.w * 0.5 + 0.5;
  if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;
  float s = 0.0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
      s += texture(u_shadow, vec3(p.xy + vec2(x, y) * u_shadow_texel, p.z - 0.0015));
  return s / 9.0;
}
void main() {
  if (v_fade < 0.999 && ts_dither(gl_FragCoord.xy) > v_fade) discard;
  vec3 base = u_part_colors != 0 ? mix(u_color_a, u_color_b, v_part) : u_color_b;
  vec3 albedo = pow(base, vec3(2.2));
  vec3 n = normalize(v_nrm);
  if (!gl_FrontFacing) n = -n;
  vec3 l = normalize(-u_sun_dir);
  // Wrapped diffuse: leaves pass a little light through.
  float ndl = max((dot(n, l) + 0.35) / 1.35, 0.0);
  vec3 color = albedo / 3.14159265 * u_sun_color * ndl * shadow_term();
  vec3 sky = mix(pow(u_ground, vec3(2.2)), mix(pow(u_sky_horizon, vec3(2.2)), pow(u_sky_top, vec3(2.2)),
                 clamp(n.y, 0.0, 1.0)), clamp(n.y * 0.5 + 0.5, 0.0, 1.0));
  color += u_ambient * sky * albedo;
  color = color / (color + vec3(1.0));
  vec3 outc = pow(color, vec3(1.0 / 2.2));
  if (u_fog.a > 0.0) {
    float d = length(v_world - u_fog_cam);
    float h = 0.5 * (v_world.y + u_fog_cam.y) - u_fog_height.x;
    float k = u_fog.a * exp(-max(h, 0.0) * u_fog_height.y);
    outc = mix(outc, u_fog.rgb, clamp(1.0 - exp(-d * k), 0.0, 1.0));
  }
  o_color = vec4(outc, 1.0);
}
)glsl";

const char* kFoliageDepthVS = R"glsl(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 2) in vec2 a_uv;
layout(location = 6) in vec4 a_inst;
layout(location = 7) in vec2 a_inst2;
uniform mat4 u_light_vp;
uniform vec4 u_wind;
uniform float u_time;
uniform float u_sway;
uniform float u_mesh_height;
float ts_wind_bend(vec2 at) {
  float ph = u_time * (1.3 + 0.7 * u_wind.z) + at.x * 0.13 + at.y * 0.11;
  return u_wind.z * (0.6 + 0.4 * sin(ph)) + u_wind.w * 0.5 * sin(ph * 2.7 + at.x * 0.3);
}
void main() {
  float c = cos(a_inst.w), s = sin(a_inst.w);
  vec3 p = a_pos * a_inst2.x;
  vec3 r = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
  float weight = u_mesh_height > 0.0 ? clamp(a_pos.y / u_mesh_height, 0.0, 1.0) : a_uv.y;
  float k = ts_wind_bend(a_inst.xz) * u_sway * weight * weight * 0.35 * a_inst2.x;
  vec3 world = a_inst.xyz + r + vec3(u_wind.x * k, -abs(k) * 0.15, u_wind.y * k);
  // Faded-out instances drop their shadow too.
  if (a_inst2.y < 0.5) world = vec3(0.0, -1.0e6, 0.0);
  gl_Position = u_light_vp * vec4(world, 1.0);
}
)glsl";

const char* kFoliageDepthFS = R"glsl(#version 330 core
void main() {}
)glsl";

GLuint compile_one(GLenum type, const char* src, std::string* err) {
  const Gl33& g = gl33();
  GLuint s = g.CreateShader(type);
  g.ShaderSource(s, 1, &src, nullptr);
  g.CompileShader(s);
  GLint ok = 0;
  g.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {0};
    g.GetShaderInfoLog(s, sizeof(log), nullptr, log);
    if (err) *err = std::string("foliage ") + (type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + log;
    g.DeleteShader(s);
    return 0;
  }
  return s;
}

GLuint link_prog(const char* vs, const char* fs, std::string* err) {
  const Gl33& g = gl33();
  const GLuint v = compile_one(GL_VERTEX_SHADER, vs, err);
  if (!v) return 0;
  const GLuint f = compile_one(GL_FRAGMENT_SHADER, fs, err);
  if (!f) {
    g.DeleteShader(v);
    return 0;
  }
  GLuint p = g.CreateProgram();
  g.AttachShader(p, v);
  g.AttachShader(p, f);
  g.LinkProgram(p);
  g.DeleteShader(v);
  g.DeleteShader(f);
  GLint ok = 0;
  g.GetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = {0};
    g.GetProgramInfoLog(p, sizeof(log), nullptr, log);
    if (err) *err = std::string("foliage link: ") + log;
    g.DeleteProgram(p);
    return 0;
  }
  return p;
}

std::string splice(const char* base, const char* marker, const char* module) {
  std::string s = base;
  const std::size_t at = s.find(marker);
  if (at == std::string::npos) return s;
  s.replace(at, std::strlen(marker), module);
  return s;
}

void wind_uniforms(GLuint prog, const Environment& env) {
  const Gl33& g = gl33();
  float dx = env.wind_dir[0], dz = env.wind_dir[1];
  const float l = std::sqrt(dx * dx + dz * dz);
  if (l > 1e-5f) {
    dx /= l;
    dz /= l;
  } else {
    dx = 1;
    dz = 0;
  }
  g.Uniform4f(g.GetUniformLocation(prog, "u_wind"), dx, dz, env.wind_strength, env.wind_gust);
}

}  // namespace

std::string mesh_fs(const char* base) { return splice(base, kSurfaceMarker, kSurfaceGlsl); }
std::string sky_fs(const char* base) { return splice(base, "//@ts:sky", kSkyGlsl); }

TerrainTextures upload(const MaterialDesc& d, MakeTexture make) {
  TerrainTextures t;
  if (!d.terrain() || !make) return t;
  auto up = [&](const std::shared_ptr<const Image>& img, bool mips) -> GLuint {
    if (!img || !img->valid()) return 0;
    t.bytes += static_cast<std::size_t>(img->width) * img->height * 4u * (mips ? 4u : 3u) / 3u;
    ++t.count;
    return make(img->rgba.data(), img->width, img->height, mips);
  };
  t.splat = up(d.splat_image, false);
  if (t.splat) {
    glBindTexture(GL_TEXTURE_2D, t.splat);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  for (int i = 0; i < 4; ++i) t.layers[i] = up(d.layer_images[i], true);
  return t;
}

void update_splat(TerrainTextures& t, const Image& splat) {
  if (!t.splat || !splat.valid()) return;
  glBindTexture(GL_TEXTURE_2D, t.splat);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, splat.width, splat.height, GL_RGBA, GL_UNSIGNED_BYTE,
                  splat.rgba.data());
}

void release(TerrainTextures& t) {
  if (t.splat) glDeleteTextures(1, &t.splat);
  for (GLuint& l : t.layers)
    if (l) glDeleteTextures(1, &l);
  t = {};
}

void set_frame_uniforms(GLuint prog, const Environment& env, const Vec3& cam) {
  const Gl33& g = gl33();
  g.Uniform4f(g.GetUniformLocation(prog, "u_fog"), env.fog_color[0], env.fog_color[1], env.fog_color[2],
              std::max(0.0f, env.fog_density));
  g.Uniform2f(g.GetUniformLocation(prog, "u_fog_height"), env.fog_height, std::max(0.0f, env.fog_falloff));
  g.Uniform3f(g.GetUniformLocation(prog, "u_fog_cam"), cam.x, cam.y, cam.z);
  const GLint splat = g.GetUniformLocation(prog, "u_splat");
  if (splat >= 0) {
    g.Uniform1i(splat, kSplatUnit);
    const char* names[4] = {"u_layer0", "u_layer1", "u_layer2", "u_layer3"};
    for (int i = 0; i < 4; ++i) g.Uniform1i(g.GetUniformLocation(prog, names[i]), kFirstLayerUnit + i);
  }
}

void bind_material(GLuint prog, const MaterialDesc& d, const TerrainTextures* t, GLuint white) {
  const Gl33& g = gl33();
  const bool on = d.terrain() && t && t->splat;
  g.Uniform1i(g.GetUniformLocation(prog, "u_terrain"), on ? 1 : 0);
  if (!on) return;
  g.ActiveTexture(GL_TEXTURE0 + kSplatUnit);
  glBindTexture(GL_TEXTURE_2D, t->splat);
  for (int i = 0; i < 4; ++i) {
    g.ActiveTexture(GL_TEXTURE0 + kFirstLayerUnit + i);
    glBindTexture(GL_TEXTURE_2D, t->layers[i] ? t->layers[i] : white);
    const std::string tint = "u_layer_tint[" + std::to_string(i) + "]";
    g.Uniform4f(g.GetUniformLocation(prog, tint.c_str()), d.layer_tint[i][0], d.layer_tint[i][1],
                d.layer_tint[i][2], d.layer_tint[i][3]);
  }
  float tiling[4];
  for (int i = 0; i < 4; ++i) tiling[i] = std::max(0.05f, d.layer_tiling[i]);
  g.Uniform1fv(g.GetUniformLocation(prog, "u_layer_tiling"), 4, tiling);
  g.ActiveTexture(GL_TEXTURE0);
}

bool FoliagePass::init(std::string* error) {
  const Gl33& g = gl33();
  if (!g.VertexAttribDivisor || !g.DrawElementsInstanced) {
    if (error) *error = "instancing entry points missing";
    return false;
  }
  prog_ = link_prog(kFoliageVS, kFoliageFS, error);
  if (!prog_) return false;
  depth_prog_ = link_prog(kFoliageDepthVS, kFoliageDepthFS, error);
  if (!depth_prog_) return false;
  g.GenBuffers(1, &inst_vbo_);
  return true;
}

void FoliagePass::shutdown() {
  const Gl33& g = gl33();
  if (!g.DeleteBuffers) return;
  for (auto& [vbo, vao] : vaos_) g.DeleteVertexArrays(1, &vao);
  vaos_.clear();
  if (inst_vbo_) g.DeleteBuffers(1, &inst_vbo_);
  if (prog_) g.DeleteProgram(prog_);
  if (depth_prog_) g.DeleteProgram(depth_prog_);
  inst_vbo_ = prog_ = depth_prog_ = 0;
  batches_.clear();
  instances_.clear();
}

void FoliagePass::begin_frame() {
  batches_.clear();
  instances_.clear();
  uploaded_ = false;
}

void FoliagePass::add(const FoliageBatch& b, GLuint vbo, GLuint ibo, GLsizei count) {
  if (!prog_ || !b.instances || b.count == 0 || count <= 0) return;
  if (instances_.size() + b.count > 1000000) return;
  Batch x;
  x.b = b;
  x.vbo = vbo;
  x.ibo = ibo;
  x.count = count;
  x.first = instances_.size();
  instances_.insert(instances_.end(), b.instances, b.instances + b.count);
  x.b.instances = nullptr;
  batches_.push_back(x);
}

void FoliagePass::forget_mesh(GLuint vbo) {
  auto it = vaos_.find(vbo);
  if (it == vaos_.end()) return;
  gl33().DeleteVertexArrays(1, &it->second);
  vaos_.erase(it);
}

void FoliagePass::upload() {
  if (uploaded_) return;
  uploaded_ = true;
  const Gl33& g = gl33();
  g.BindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
  g.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr_ts>(instances_.size() * sizeof(FoliageInstance3D)),
               instances_.data(), GL_DYNAMIC_DRAW);
}

GLuint FoliagePass::vao_for(GLuint vbo, GLuint ibo) {
  auto it = vaos_.find(vbo);
  if (it != vaos_.end()) return it->second;
  const Gl33& g = gl33();
  GLuint vao = 0;
  g.GenVertexArrays(1, &vao);
  g.BindVertexArray(vao);
  g.BindBuffer(GL_ARRAY_BUFFER, vbo);
  g.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  const GLsizei stride = sizeof(Vertex3D);
  g.EnableVertexAttribArray(0);
  g.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex3D, position)));
  g.EnableVertexAttribArray(1);
  g.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex3D, normal)));
  g.EnableVertexAttribArray(2);
  g.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex3D, u)));
  g.EnableVertexAttribArray(6);
  g.EnableVertexAttribArray(7);
  g.VertexAttribDivisor(6, 1);
  g.VertexAttribDivisor(7, 1);
  vaos_[vbo] = vao;
  return vao;
}

void FoliagePass::bind_instances(GLuint vao, std::size_t first) {
  const Gl33& g = gl33();
  g.BindVertexArray(vao);
  g.BindBuffer(GL_ARRAY_BUFFER, inst_vbo_);
  const GLsizei stride = sizeof(FoliageInstance3D);
  const std::size_t base = first * sizeof(FoliageInstance3D);
  g.VertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(base));
  g.VertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, stride,
                        reinterpret_cast<const void*>(base + offsetof(FoliageInstance3D, scale)));
}

void FoliagePass::set_batch_uniforms(GLuint prog, const Batch& x) {
  const Gl33& g = gl33();
  g.Uniform1f(g.GetUniformLocation(prog, "u_time"), x.b.time);
  g.Uniform1f(g.GetUniformLocation(prog, "u_sway"), x.b.sway);
  g.Uniform1f(g.GetUniformLocation(prog, "u_mesh_height"), x.b.mesh_height);
}

std::size_t FoliagePass::draw_shadows(const Mat4& light_vp, const Environment& env) {
  if (batches_.empty() || !depth_prog_) return 0;
  upload();
  const Gl33& g = gl33();
  g.UseProgram(depth_prog_);
  g.UniformMatrix4fv(g.GetUniformLocation(depth_prog_, "u_light_vp"), 1, GL_FALSE, light_vp.m);
  wind_uniforms(depth_prog_, env);
  std::size_t n = 0;
  glDisable(GL_CULL_FACE);
  for (const Batch& x : batches_) {
    if (!x.b.cast_shadow) continue;
    if (x.b.count == 0) continue;
    set_batch_uniforms(depth_prog_, x);
    bind_instances(vao_for(x.vbo, x.ibo), x.first);
    g.DrawElementsInstanced(GL_TRIANGLES, x.count, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(x.b.count));
    n += x.b.count;
  }
  glEnable(GL_CULL_FACE);
  return n;
}

std::size_t FoliagePass::draw(const FrameContext& c) {
  if (batches_.empty() || !prog_) return 0;
  upload();
  const Gl33& g = gl33();
  const GLuint p = prog_;
  g.UseProgram(p);
  auto U = [&](const char* n) { return g.GetUniformLocation(p, n); };
  g.UniformMatrix4fv(U("u_view_proj"), 1, GL_FALSE, c.view_proj.m);
  g.UniformMatrix4fv(U("u_light_vp"), 1, GL_FALSE, c.light_vp.m);
  const Vec3 sun = normalize(c.env.sun_direction);
  g.Uniform3f(U("u_sun_dir"), sun.x, sun.y, sun.z);
  g.Uniform3f(U("u_sun_color"), c.env.sun_color[0] * c.env.sun_intensity, c.env.sun_color[1] * c.env.sun_intensity,
              c.env.sun_color[2] * c.env.sun_intensity);
  g.Uniform3fv(U("u_sky_top"), 1, c.env.sky_top);
  g.Uniform3fv(U("u_sky_horizon"), 1, c.env.sky_horizon);
  g.Uniform3fv(U("u_ground"), 1, c.env.ground);
  g.Uniform1f(U("u_ambient"), c.env.ambient);
  g.Uniform1i(U("u_shadows"), c.shadows ? 1 : 0);
  g.Uniform1f(U("u_shadow_texel"), c.shadow_texel);
  g.Uniform1i(U("u_shadow"), 3);
  g.ActiveTexture(GL_TEXTURE0 + 3);
  glBindTexture(GL_TEXTURE_2D, c.shadow_tex);
  g.ActiveTexture(GL_TEXTURE0);
  set_frame_uniforms(p, c.env, c.camera);
  wind_uniforms(p, c.env);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  std::size_t n = 0;
  for (const Batch& x : batches_) {
    set_batch_uniforms(p, x);
    g.Uniform3fv(U("u_color_a"), 1, x.b.color_a);
    g.Uniform3fv(U("u_color_b"), 1, x.b.color_b);
    g.Uniform1i(U("u_part_colors"), x.b.part_colors ? 1 : 0);
    bind_instances(vao_for(x.vbo, x.ibo), x.first);
    g.DrawElementsInstanced(GL_TRIANGLES, x.count, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(x.b.count));
    n += x.b.count;
  }
  return n;
}

}  // namespace gl_terrain
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
