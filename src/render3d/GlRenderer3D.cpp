#include "render3d/GlRenderer3D.h"

#include "render3d/GlApi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

namespace {

constexpr int kShadowSize = 2048;

const char* kMeshVS = R"glsl(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_tan;
uniform mat4 u_model;
uniform mat4 u_view_proj;
uniform mat4 u_light_vp;
out vec3 v_world;
out vec3 v_nrm;
out vec2 v_uv;
out vec4 v_tan;
out vec4 v_light;
void main() {
  vec4 w = u_model * vec4(a_pos, 1.0);
  mat3 nm = transpose(inverse(mat3(u_model)));
  v_world = w.xyz;
  v_nrm = normalize(nm * a_nrm);
  v_tan = vec4(normalize(mat3(u_model) * a_tan.xyz), a_tan.w);
  v_uv = a_uv;
  v_light = u_light_vp * w;
  gl_Position = u_view_proj * w;
}
)glsl";

const char* kMeshFS = R"glsl(#version 330 core
in vec3 v_world;
in vec3 v_nrm;
in vec2 v_uv;
in vec4 v_tan;
in vec4 v_light;
out vec4 o_color;
uniform vec4 u_albedo;
uniform float u_metallic;
uniform float u_roughness;
uniform vec3 u_emissive;
uniform sampler2D u_albedo_tex;
uniform sampler2D u_normal_tex;
uniform sampler2D u_mr_tex;
uniform sampler2DShadow u_shadow;
uniform int u_has_normal;
uniform int u_shadows;
uniform float u_shadow_texel;
uniform vec3 u_cam;
uniform vec3 u_sun_dir;   // direction light travels
uniform vec3 u_sun_color;
uniform vec3 u_sky_top;
uniform vec3 u_sky_horizon;
uniform vec3 u_ground;
uniform float u_ambient;
uniform int u_point_count;
uniform vec3 u_point_pos[8];
uniform vec3 u_point_color[8];
uniform float u_point_range[8];
const float PI = 3.14159265;

float shadow_term(vec3 n) {
  if (u_shadows == 0) return 1.0;
  vec3 p = v_light.xyz / v_light.w * 0.5 + 0.5;
  if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;
  float bias = max(0.0025 * (1.0 - dot(n, -u_sun_dir)), 0.0006);
  float s = 0.0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
      s += texture(u_shadow, vec3(p.xy + vec2(x, y) * u_shadow_texel, p.z - bias));
  return s / 9.0;
}

vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, float metal, float rough) {
  vec3 h = normalize(v + l);
  float ndl = max(dot(n, l), 0.0);
  float ndv = max(dot(n, v), 0.001);
  float ndh = max(dot(n, h), 0.0);
  float a = rough * rough;
  float a2 = a * a;
  float d = a2 / (PI * pow(ndh * ndh * (a2 - 1.0) + 1.0, 2.0));
  float k = (rough + 1.0) * (rough + 1.0) / 8.0;
  float g = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
  vec3 f0 = mix(vec3(0.04), albedo, metal);
  vec3 f = f0 + (1.0 - f0) * pow(1.0 - max(dot(h, v), 0.0), 5.0);
  vec3 spec = d * g * f / (4.0 * ndv * max(ndl, 0.001) + 0.0001);
  vec3 kd = (1.0 - f) * (1.0 - metal);
  return (kd * albedo / PI + spec) * radiance * ndl;
}

void main() {
  vec4 base = u_albedo * texture(u_albedo_tex, v_uv);
  vec3 albedo = pow(base.rgb, vec3(2.2));
  vec4 mr = texture(u_mr_tex, v_uv);
  float rough = clamp(u_roughness * mr.g, 0.04, 1.0);
  float metal = clamp(u_metallic * mr.b, 0.0, 1.0);
  vec3 n = normalize(v_nrm);
  if (!gl_FrontFacing) n = -n;
  if (u_has_normal != 0) {
    vec3 t = normalize(v_tan.xyz - n * dot(n, v_tan.xyz));
    vec3 b = cross(n, t) * v_tan.w;
    vec3 tn = texture(u_normal_tex, v_uv).xyz * 2.0 - 1.0;
    n = normalize(mat3(t, b, n) * tn);
  }
  vec3 v = normalize(u_cam - v_world);
  vec3 color = vec3(0.0);
  vec3 l = normalize(-u_sun_dir);
  color += brdf(n, v, l, u_sun_color, albedo, metal, rough) * shadow_term(n);
  for (int i = 0; i < u_point_count; ++i) {
    vec3 d = u_point_pos[i] - v_world;
    float dist = length(d);
    float fall = clamp(1.0 - pow(dist / u_point_range[i], 4.0), 0.0, 1.0);
    fall = fall * fall / (dist * dist + 1.0);
    color += brdf(n, v, d / max(dist, 0.0001), u_point_color[i] * fall, albedo, metal, rough);
  }
  vec3 sky = mix(pow(u_ground, vec3(2.2)), mix(pow(u_sky_horizon, vec3(2.2)),
                 pow(u_sky_top, vec3(2.2)), clamp(n.y, 0.0, 1.0)), clamp(n.y * 0.5 + 0.5, 0.0, 1.0));
  vec3 f0 = mix(vec3(0.04), albedo, metal);
  color += u_ambient * sky * (albedo * (1.0 - metal) + f0 * (1.0 - rough) * 0.5);
  color += u_emissive;
  color = color / (color + vec3(1.0));
  o_color = vec4(pow(color, vec3(1.0 / 2.2)), base.a);
}
)glsl";

const char* kDepthVS = R"glsl(#version 330 core
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)glsl";

const char* kDepthFS = R"glsl(#version 330 core
void main() {}
)glsl";

// Full-screen triangle; the view ray picks a gradient + sun disc.
const char* kSkyVS = R"glsl(#version 330 core
out vec2 v_ndc;
void main() {
  vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
  v_ndc = p;
  gl_Position = vec4(p, 0.9999, 1.0);
}
)glsl";

const char* kSkyFS = R"glsl(#version 330 core
in vec2 v_ndc;
out vec4 o_color;
uniform mat4 u_inv_view_proj;
uniform vec3 u_sky_top;
uniform vec3 u_sky_horizon;
uniform vec3 u_ground;
uniform vec3 u_sun_dir;
uniform vec3 u_sun_color;
void main() {
  vec4 a = u_inv_view_proj * vec4(v_ndc, -1.0, 1.0);
  vec4 b = u_inv_view_proj * vec4(v_ndc, 1.0, 1.0);
  vec3 d = normalize(b.xyz / b.w - a.xyz / a.w);
  vec3 c;
  if (d.y >= 0.0) c = mix(u_sky_horizon, u_sky_top, pow(d.y, 0.55));
  else c = mix(u_sky_horizon, u_ground, clamp(-d.y * 6.0, 0.0, 1.0));
  float s = max(dot(d, -u_sun_dir), 0.0);
  c += u_sun_color * (pow(s, 900.0) * 1.4 + pow(s, 12.0) * 0.18);
  o_color = vec4(c, 1.0);
}
)glsl";

const char* kLineVS = R"glsl(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
uniform mat4 u_view_proj;
out vec4 v_col;
void main() { v_col = a_col; gl_Position = u_view_proj * vec4(a_pos, 1.0); }
)glsl";

const char* kLineFS = R"glsl(#version 330 core
in vec4 v_col;
out vec4 o_color;
void main() { o_color = v_col; }
)glsl";

struct GpuMesh {
  GLuint vao = 0, vbo = 0, ibo = 0;
  GLsizei count = 0;
};

struct GpuMaterial {
  MaterialDesc desc;
  GLuint albedo = 0, normal = 0, mr = 0;  // 0 = default texture
};

struct DrawItem {
  std::uint32_t mesh;
  std::uint32_t material;
  Mat4 model;
  bool shadow;
};

struct LineVertex {
  float p[3];
  float c[4];
};

GLuint compile(GLenum type, const char* src, std::string* err) {
  const Gl33& g = gl33();
  GLuint s = g.CreateShader(type);
  g.ShaderSource(s, 1, &src, nullptr);
  g.CompileShader(s);
  GLint ok = 0;
  g.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {0};
    g.GetShaderInfoLog(s, sizeof(log), nullptr, log);
    if (err) *err = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + log;
    g.DeleteShader(s);
    return 0;
  }
  return s;
}

GLuint link(const char* vs, const char* fs, std::string* err) {
  const Gl33& g = gl33();
  const GLuint v = compile(GL_VERTEX_SHADER, vs, err);
  if (!v) return 0;
  const GLuint f = compile(GL_FRAGMENT_SHADER, fs, err);
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
    if (err) *err = std::string("link: ") + log;
    g.DeleteProgram(p);
    return 0;
  }
  return p;
}

GLuint make_texture(const unsigned char* rgba, int w, int h, bool mips) {
  const Gl33& g = gl33();
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  if (mips) {
    g.GenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  return t;
}

class GlRenderer3D final : public Renderer3D {
 public:
  ~GlRenderer3D() override { shutdown(); }

  const char* backend_name() const override { return "gl"; }

  bool init(std::string* error) override {
    if (ready_) return true;
    if (!load_gl33(error)) return false;
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 3 || (major == 3 && minor < 3)) {
      if (error) *error = "OpenGL 3.3 needed, context is " + std::to_string(major) + "." + std::to_string(minor);
      return false;
    }
    const Gl33& g = gl33();
    std::string err;
    mesh_prog_ = link(kMeshVS, kMeshFS, &err);
    if (mesh_prog_) depth_prog_ = link(kDepthVS, kDepthFS, &err);
    if (depth_prog_) sky_prog_ = link(kSkyVS, kSkyFS, &err);
    if (sky_prog_) line_prog_ = link(kLineVS, kLineFS, &err);
    if (!line_prog_) {
      if (error) *error = "shader build failed: " + err;
      shutdown_programs();
      return false;
    }
    const unsigned char white[4] = {255, 255, 255, 255};
    const unsigned char flat[4] = {128, 128, 255, 255};
    white_tex_ = make_texture(white, 1, 1, false);
    flat_tex_ = make_texture(flat, 1, 1, false);
    g.GenVertexArrays(1, &empty_vao_);
    g.GenVertexArrays(1, &line_vao_);
    g.GenBuffers(1, &line_vbo_);
    g.BindVertexArray(line_vao_);
    g.BindBuffer(GL_ARRAY_BUFFER, line_vbo_);
    g.EnableVertexAttribArray(0);
    g.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), nullptr);
    g.EnableVertexAttribArray(1);
    g.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<const void*>(sizeof(float) * 3));
    g.BindVertexArray(0);
    // Shadow map.
    glGenTextures(1, &shadow_tex_);
    glBindTexture(GL_TEXTURE_2D, shadow_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kShadowSize, kShadowSize, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    g.GenFramebuffers(1, &shadow_fbo_);
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    g.BindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_tex_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool complete = g.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    if (!complete) {
      if (error) *error = "shadow framebuffer incomplete";
      shutdown();
      return false;
    }
    ready_ = true;
    return true;
  }

  void shutdown() override {
    if (!line_prog_ && !mesh_prog_ && !ready_) return;
    const Gl33& g = gl33();
    if (g.DeleteBuffers) {
      for (auto& [id, m] : meshes_) free_mesh(m);
      meshes_.clear();
      for (auto& [id, m] : materials_) free_material(m);
      materials_.clear();
      if (white_tex_) glDeleteTextures(1, &white_tex_);
      if (flat_tex_) glDeleteTextures(1, &flat_tex_);
      if (shadow_tex_) glDeleteTextures(1, &shadow_tex_);
      if (shadow_fbo_) g.DeleteFramebuffers(1, &shadow_fbo_);
      if (line_vbo_) g.DeleteBuffers(1, &line_vbo_);
      if (line_vao_) g.DeleteVertexArrays(1, &line_vao_);
      if (empty_vao_) g.DeleteVertexArrays(1, &empty_vao_);
      free_target();
      shutdown_programs();
    }
    white_tex_ = flat_tex_ = shadow_tex_ = shadow_fbo_ = 0;
    line_vbo_ = line_vao_ = empty_vao_ = 0;
    ready_ = false;
  }

  MeshHandle create_mesh(const MeshData& mesh) override {
    if (!ready_ || !mesh.valid()) return {};
    const Gl33& g = gl33();
    GpuMesh m;
    GLint prev_vao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    g.GenVertexArrays(1, &m.vao);
    g.GenBuffers(1, &m.vbo);
    g.GenBuffers(1, &m.ibo);
    g.BindVertexArray(m.vao);
    g.BindBuffer(GL_ARRAY_BUFFER, m.vbo);
    g.BufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr_ts>(mesh.vertices.size() * sizeof(Vertex3D)),
                 mesh.vertices.data(), GL_STATIC_DRAW);
    g.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    g.BufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr_ts>(mesh.indices.size() * sizeof(std::uint32_t)),
                 mesh.indices.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(Vertex3D);
    g.EnableVertexAttribArray(0);
    g.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(Vertex3D, position)));
    g.EnableVertexAttribArray(1);
    g.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(Vertex3D, normal)));
    g.EnableVertexAttribArray(2);
    g.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(Vertex3D, u)));
    g.EnableVertexAttribArray(3);
    g.VertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offsetof(Vertex3D, tangent)));
    g.BindVertexArray(static_cast<GLuint>(prev_vao));
    m.count = static_cast<GLsizei>(mesh.indices.size());
    const std::uint32_t id = ++next_;
    meshes_[id] = m;
    tri_counts_[id] = mesh.indices.size() / 3;
    return {id};
  }

  MaterialHandle create_material(const MaterialDesc& d) override {
    if (!ready_) return {};
    GpuMaterial m;
    m.desc = d;
    auto upload = [](const std::shared_ptr<const Image>& img) -> GLuint {
      if (!img || !img->valid()) return 0;
      return make_texture(img->rgba.data(), img->width, img->height, true);
    };
    m.albedo = upload(d.base_color_image);
    m.normal = upload(d.normal_image);
    m.mr = upload(d.metal_rough_image);
    const std::uint32_t id = ++next_;
    materials_[id] = m;
    return {id};
  }

  void destroy_mesh(MeshHandle h) override {
    auto it = meshes_.find(h.id);
    if (it == meshes_.end()) return;
    free_mesh(it->second);
    meshes_.erase(it);
    tri_counts_.erase(h.id);
  }
  void destroy_material(MaterialHandle h) override {
    auto it = materials_.find(h.id);
    if (it == materials_.end()) return;
    free_material(it->second);
    materials_.erase(it);
  }

  void set_offscreen(bool on) override { offscreen_ = on; }
  void set_environment(const Environment& env) override { env_ = env; }

  void begin_frame(const Camera3D& camera, int width, int height) override {
    camera_ = camera;
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    draws_.clear();
    points_.clear();
    lines_depth_.clear();
    lines_top_.clear();
    stats_ = {};
  }

  void add_point_light(const PointLight& l) override {
    if (points_.size() < kMaxPointLights) points_.push_back(l);
  }

  void draw(MeshHandle mesh, MaterialHandle material, const Transform& t) override {
    draw_matrix(mesh, material, t.matrix(), true);
  }

  void draw_matrix(MeshHandle mesh, MaterialHandle material, const Mat4& world,
                   bool cast_shadow) override {
    if (!meshes_.count(mesh.id)) return;
    draws_.push_back({mesh.id, material.id, world, cast_shadow});
  }

  void draw_line(const Vec3& a, const Vec3& b, const float* rgba, bool depth_test) override {
    const float c[4] = {rgba ? rgba[0] : 1.0f, rgba ? rgba[1] : 1.0f, rgba ? rgba[2] : 1.0f,
                        rgba ? rgba[3] : 1.0f};
    auto& list = depth_test ? lines_depth_ : lines_top_;
    list.push_back({{a.x, a.y, a.z}, {c[0], c[1], c[2], c[3]}});
    list.push_back({{b.x, b.y, b.z}, {c[0], c[1], c[2], c[3]}});
  }

  FrameStats end_frame() override {
    if (!ready_) return stats_;
    const Gl33& g = gl33();
    // Save caller state.
    GLint prev_fbo = 0, prev_vp[4] = {0, 0, 0, 0}, prev_prog = 0, prev_vao = 0, prev_tex_unit = 0;
    GLint prev_tex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_tex_unit);
    g.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    const GLboolean was_depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean was_cull = glIsEnabled(GL_CULL_FACE);
    const GLboolean was_blend = glIsEnabled(GL_BLEND);
    const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prev_depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // Sun shadow pass.
    const Vec3 sun = normalize(env_.sun_direction);
    const Vec3 focus = camera_.target;
    const float sd = env_.shadow_distance;
    const Mat4 light_view = Mat4::look_at(focus - sun * (sd * 2.0f), focus,
                                          std::fabs(sun.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
    const Mat4 light_vp = Mat4::ortho(-sd, sd, -sd, sd, 0.1f, sd * 4.0f) * light_view;
    const bool shadows = env_.shadows && env_.sun_intensity > 0.0f;
    if (shadows) {
      g.BindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
      glViewport(0, 0, kShadowSize, kShadowSize);
      glClear(GL_DEPTH_BUFFER_BIT);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
      glEnable(GL_POLYGON_OFFSET_FILL);
      glPolygonOffset(1.5f, 2.0f);
      g.UseProgram(depth_prog_);
      const GLint u_mvp = g.GetUniformLocation(depth_prog_, "u_mvp");
      for (const DrawItem& d : draws_) {
        if (!d.shadow) continue;
        const GpuMesh& m = meshes_[d.mesh];
        const Mat4 mvp = light_vp * d.model;
        g.UniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
        g.BindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr);
        ++stats_.shadow_casters;
      }
      glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Main target.
    if (offscreen_) {
      ensure_target(width_, height_);
      g.BindFramebuffer(GL_FRAMEBUFFER, target_fbo_);
    } else {
      g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    }
    glViewport(0, 0, width_, height_);
    glClearColor(env_.sky_horizon[0], env_.sky_horizon[1], env_.sky_horizon[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Mat4 view_proj = camera_.projection(aspect) * camera_.view();

    // Sky.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    g.UseProgram(sky_prog_);
    const Mat4 inv_vp = view_proj.inverse();
    g.UniformMatrix4fv(g.GetUniformLocation(sky_prog_, "u_inv_view_proj"), 1, GL_FALSE, inv_vp.m);
    g.Uniform3fv(g.GetUniformLocation(sky_prog_, "u_sky_top"), 1, env_.sky_top);
    g.Uniform3fv(g.GetUniformLocation(sky_prog_, "u_sky_horizon"), 1, env_.sky_horizon);
    g.Uniform3fv(g.GetUniformLocation(sky_prog_, "u_ground"), 1, env_.ground);
    g.Uniform3f(g.GetUniformLocation(sky_prog_, "u_sun_dir"), sun.x, sun.y, sun.z);
    g.Uniform3fv(g.GetUniformLocation(sky_prog_, "u_sun_color"), 1, env_.sun_color);
    g.BindVertexArray(empty_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Meshes.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    g.UseProgram(mesh_prog_);
    const GLuint p = mesh_prog_;
    auto U = [&](const char* n) { return g.GetUniformLocation(p, n); };
    g.UniformMatrix4fv(U("u_view_proj"), 1, GL_FALSE, view_proj.m);
    g.UniformMatrix4fv(U("u_light_vp"), 1, GL_FALSE, light_vp.m);
    g.Uniform3f(U("u_cam"), camera_.position.x, camera_.position.y, camera_.position.z);
    g.Uniform3f(U("u_sun_dir"), sun.x, sun.y, sun.z);
    g.Uniform3f(U("u_sun_color"), env_.sun_color[0] * env_.sun_intensity,
                env_.sun_color[1] * env_.sun_intensity, env_.sun_color[2] * env_.sun_intensity);
    g.Uniform3fv(U("u_sky_top"), 1, env_.sky_top);
    g.Uniform3fv(U("u_sky_horizon"), 1, env_.sky_horizon);
    g.Uniform3fv(U("u_ground"), 1, env_.ground);
    g.Uniform1f(U("u_ambient"), env_.ambient);
    g.Uniform1i(U("u_shadows"), shadows ? 1 : 0);
    g.Uniform1f(U("u_shadow_texel"), 1.0f / kShadowSize);
    g.Uniform1i(U("u_albedo_tex"), 0);
    g.Uniform1i(U("u_normal_tex"), 1);
    g.Uniform1i(U("u_mr_tex"), 2);
    g.Uniform1i(U("u_shadow"), 3);
    const int np = static_cast<int>(points_.size());
    g.Uniform1i(U("u_point_count"), np);
    if (np > 0) {
      float pos[3 * kMaxPointLights], col[3 * kMaxPointLights], range[kMaxPointLights];
      for (int i = 0; i < np; ++i) {
        const PointLight& l = points_[static_cast<std::size_t>(i)];
        pos[i * 3 + 0] = l.position.x;
        pos[i * 3 + 1] = l.position.y;
        pos[i * 3 + 2] = l.position.z;
        for (int k = 0; k < 3; ++k) col[i * 3 + k] = l.color[k] * l.intensity;
        range[i] = std::max(0.1f, l.range);
      }
      g.Uniform3fv(U("u_point_pos"), np, pos);
      g.Uniform3fv(U("u_point_color"), np, col);
      g.Uniform1fv(U("u_point_range"), np, range);
    }
    stats_.lights = points_.size() + (env_.sun_intensity > 0.0f ? 1u : 0u);
    g.ActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, shadow_tex_);
    const GLint u_model = U("u_model"), u_alb = U("u_albedo"), u_met = U("u_metallic"),
                u_rgh = U("u_roughness"), u_emi = U("u_emissive"), u_hn = U("u_has_normal");
    const MaterialDesc fallback;
    for (const DrawItem& d : draws_) {
      const GpuMesh& m = meshes_[d.mesh];
      auto mit = materials_.find(d.material);
      const GpuMaterial* mat = mit == materials_.end() ? nullptr : &mit->second;
      const MaterialDesc& md = mat ? mat->desc : fallback;
      if (md.double_sided) glDisable(GL_CULL_FACE);
      else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
      }
      g.UniformMatrix4fv(u_model, 1, GL_FALSE, d.model.m);
      g.Uniform4f(u_alb, md.base_color[0], md.base_color[1], md.base_color[2], md.base_color[3]);
      g.Uniform1f(u_met, md.metallic);
      g.Uniform1f(u_rgh, md.roughness);
      g.Uniform3f(u_emi, md.emissive[0], md.emissive[1], md.emissive[2]);
      g.Uniform1i(u_hn, mat && mat->normal ? 1 : 0);
      g.ActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, mat && mat->albedo ? mat->albedo : white_tex_);
      g.ActiveTexture(GL_TEXTURE0 + 1);
      glBindTexture(GL_TEXTURE_2D, mat && mat->normal ? mat->normal : flat_tex_);
      g.ActiveTexture(GL_TEXTURE0 + 2);
      glBindTexture(GL_TEXTURE_2D, mat && mat->mr ? mat->mr : white_tex_);
      g.BindVertexArray(m.vao);
      glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr);
      ++stats_.draws;
      stats_.triangles += static_cast<std::size_t>(m.count) / 3;
    }

    // Lines.
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    g.UseProgram(line_prog_);
    g.UniformMatrix4fv(g.GetUniformLocation(line_prog_, "u_view_proj"), 1, GL_FALSE, view_proj.m);
    g.BindVertexArray(line_vao_);
    auto draw_lines = [&](const std::vector<LineVertex>& v) {
      if (v.empty()) return;
      g.BindBuffer(GL_ARRAY_BUFFER, line_vbo_);
      g.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr_ts>(v.size() * sizeof(LineVertex)),
                   v.data(), GL_DYNAMIC_DRAW);
      glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(v.size()));
      stats_.lines += v.size() / 2;
    };
    glDepthMask(GL_FALSE);
    draw_lines(lines_depth_);
    glDisable(GL_DEPTH_TEST);
    draw_lines(lines_top_);

    // Restore.
    glDepthMask(prev_depth_mask);
    g.BindVertexArray(static_cast<GLuint>(prev_vao));
    g.UseProgram(static_cast<GLuint>(prev_prog));
    g.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    g.ActiveTexture(static_cast<GLenum>(prev_tex_unit));
    g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
    set(GL_DEPTH_TEST, was_depth);
    set(GL_CULL_FACE, was_cull);
    set(GL_BLEND, was_blend);
    set(GL_SCISSOR_TEST, was_scissor);
    return stats_;
  }

  std::uint64_t color_texture() const override { return target_color_; }

  bool read_pixels(std::vector<unsigned char>* rgba, int* w, int* h) const override {
    if (!ready_ || !target_fbo_) return false;
    const Gl33& g = gl33();
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    g.BindFramebuffer(GL_FRAMEBUFFER, target_fbo_);
    rgba->assign(static_cast<std::size_t>(target_w_) * target_h_ * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, target_w_, target_h_, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
    g.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
    if (w) *w = target_w_;
    if (h) *h = target_h_;
    return true;
  }

 private:
  void free_mesh(GpuMesh& m) {
    const Gl33& g = gl33();
    if (m.vbo) g.DeleteBuffers(1, &m.vbo);
    if (m.ibo) g.DeleteBuffers(1, &m.ibo);
    if (m.vao) g.DeleteVertexArrays(1, &m.vao);
    m = {};
  }
  void free_material(GpuMaterial& m) {
    for (GLuint* t : {&m.albedo, &m.normal, &m.mr}) {
      if (*t) glDeleteTextures(1, t);
      *t = 0;
    }
  }
  void shutdown_programs() {
    const Gl33& g = gl33();
    for (GLuint* p : {&mesh_prog_, &depth_prog_, &sky_prog_, &line_prog_}) {
      if (*p && g.DeleteProgram) g.DeleteProgram(*p);
      *p = 0;
    }
  }
  void free_target() {
    const Gl33& g = gl33();
    if (target_fbo_) g.DeleteFramebuffers(1, &target_fbo_);
    if (target_depth_) g.DeleteRenderbuffers(1, &target_depth_);
    if (target_color_) glDeleteTextures(1, &target_color_);
    target_fbo_ = target_depth_ = target_color_ = 0;
    target_w_ = target_h_ = 0;
  }
  void ensure_target(int w, int h) {
    if (target_fbo_ && w == target_w_ && h == target_h_) return;
    free_target();
    const Gl33& g = gl33();
    glGenTextures(1, &target_color_);
    glBindTexture(GL_TEXTURE_2D, target_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g.GenRenderbuffers(1, &target_depth_);
    g.BindRenderbuffer(GL_RENDERBUFFER, target_depth_);
    g.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    g.GenFramebuffers(1, &target_fbo_);
    g.BindFramebuffer(GL_FRAMEBUFFER, target_fbo_);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_color_, 0);
    g.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target_depth_);
    target_w_ = w;
    target_h_ = h;
  }

  bool ready_ = false;
  bool offscreen_ = false;
  GLuint mesh_prog_ = 0, depth_prog_ = 0, sky_prog_ = 0, line_prog_ = 0;
  GLuint white_tex_ = 0, flat_tex_ = 0;
  GLuint shadow_tex_ = 0, shadow_fbo_ = 0;
  GLuint line_vao_ = 0, line_vbo_ = 0, empty_vao_ = 0;
  GLuint target_fbo_ = 0, target_color_ = 0, target_depth_ = 0;
  int target_w_ = 0, target_h_ = 0;
  std::uint32_t next_ = 0;
  std::unordered_map<std::uint32_t, GpuMesh> meshes_;
  std::unordered_map<std::uint32_t, std::size_t> tri_counts_;
  std::unordered_map<std::uint32_t, GpuMaterial> materials_;
  Environment env_;
  Camera3D camera_;
  int width_ = 1, height_ = 1;
  std::vector<DrawItem> draws_;
  std::vector<PointLight> points_;
  std::vector<LineVertex> lines_depth_, lines_top_;
  FrameStats stats_;
};

}  // namespace

std::unique_ptr<Renderer3D> make_gl_renderer3d() {
  return std::make_unique<GlRenderer3D>();
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
