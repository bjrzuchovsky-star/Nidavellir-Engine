#include "render3d/RhiRenderer3D.h"
#include "render3d/Skeleton.h"

#include "rhi/Rhi.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
namespace {

constexpr int kShadow = 2048;

struct FrameBlock {
  Mat4 view_proj;
  Mat4 inv_view_proj;
  Mat4 light_vp[4];
  Vec4 cam, sun_dir, sun_color, sky_top, sky_horizon, ground, fog, fog_extra;
  Vec4 counts, flags, wind, timev, cascade_splits, atlas, clip, grade;
  Vec4 point_pos[8];
  Vec4 point_color[8];
  Vec4 spot_pos[4];
  Vec4 spot_dir[4];
  Vec4 spot_color[4];
  Vec4 area_pos[2];
  Vec4 area_u[2];
  Vec4 area_v[2];
  Vec4 area_color[2];
  Vec4 probe_pos[4];
  Vec4 probe_color[4];
};
static_assert(offsetof(FrameBlock, cam) == 384);

struct DrawBlock {
  Mat4 model;
  Vec4 albedo, emissive, mr, draw_flags, extra;
  Vec4 layer_tint[4];
  Vec4 layer_tiling, foliage_a, foliage_b;
};

struct DrawItem {
  std::uint32_t mesh = 0;
  std::uint32_t material = 0;
  Mat4 model;
  bool shadow = true;
  int bone = -1;
  int bones = 0;
};

struct GpuMesh {
  rhi::Buffer* vb = nullptr;
  rhi::Buffer* ib = nullptr;
  rhi::Buffer* skin = nullptr;
  std::uint32_t count = 0;
  std::uint32_t verts = 0;
  std::size_t bytes = 0;
};
struct GpuMat {
  MaterialDesc desc;
  rhi::Texture* albedo = nullptr;
  rhi::Texture* normal = nullptr;
  rhi::Texture* mr = nullptr;
  rhi::Texture* splat = nullptr;
  rhi::Texture* layer[4] = {};
  std::size_t bytes = 0;
  int textures = 0;
};

rhi::VertexAttr mesh_attr(std::uint32_t loc, std::uint32_t buf, std::uint32_t off, std::uint32_t n, bool inst) {
  rhi::VertexAttr a;
  a.location = loc;
  a.buffer = buf;
  a.offset = off;
  a.components = n;
  a.instance = inst;
  return a;
}

class RhiRenderer final : public Renderer3D {
 public:
  explicit RhiRenderer(rhi::Device* device) : device_(device) {}
  ~RhiRenderer() override { shutdown(); }

  const char* backend_name() const override {
    if (!device_) return "rhi";
    switch (device_->api()) {
      case rhi::Api::Vulkan: return "vulkan";
      case rhi::Api::D3D12: return "d3d12";
      case rhi::Api::OpenGL: return "opengl";
    }
    return "gl";
  }
  bool color_bottom_up() const override { return device_ && device_->bottom_up(); }

  bool init(std::string* error) override {
    if (!device_) {
      if (error) *error = "no graphics device";
      return false;
    }
    align_ = rhi::uniform_align();
    rhi::BufferDesc ubo;
    ubo.kind = rhi::BufferKind::Uniform;
    ubo.size = 8u << 20;
    ubo_ = device_->create_buffer(ubo, nullptr);
    const unsigned char white[4] = {255, 255, 255, 255};
    const unsigned char flatn[4] = {128, 128, 255, 255};
    white_ = tex2d(white, 1, 1, false);
    flat_ = tex2d(flatn, 1, 1, false);
    rhi::SamplerDesc sw;
    sw.wrap = rhi::Wrap::Repeat;
    sw.mips = true;
    wrap_ = device_->create_sampler(sw);
    sw.wrap = rhi::Wrap::Clamp;
    sw.mips = false;
    clamp_ = device_->create_sampler(sw);
    auto pipe = [&](const char* vs, const char* fs, rhi::PipelineDesc d) -> rhi::Pipeline* {
      d.vs = vs;
      d.fs = fs;
      std::string err;
      rhi::Pipeline* p = device_->create_pipeline(d, &err);
      if (!p && error && error->empty()) *error = err;
      return p;
    };
    rhi::PipelineDesc mesh;
    mesh.attr_count = 4;
    mesh.attrs[0] = mesh_attr(0, 0, offsetof(Vertex3D, position), 3, false);
    mesh.attrs[1] = mesh_attr(1, 0, offsetof(Vertex3D, normal), 3, false);
    mesh.attrs[2] = mesh_attr(2, 0, offsetof(Vertex3D, u), 2, false);
    mesh.attrs[3] = mesh_attr(3, 0, offsetof(Vertex3D, tangent), 4, false);
    mesh.strides[0] = sizeof(Vertex3D);
    mesh_ = pipe("mesh.vert", "mesh.frag", mesh);
    mesh.cull = rhi::Cull::None;
    mesh_2s_ = pipe("mesh.vert", "mesh.frag", mesh);
    mesh.blend = rhi::Blend::Alpha;
    mesh.depth_write = false;
    mesh_blend_ = pipe("mesh.vert", "mesh.frag", mesh);
    rhi::PipelineDesc skin = mesh;
    skin.blend = rhi::Blend::Off;
    skin.depth_write = true;
    skin.cull = rhi::Cull::Back;
    skin.attr_count = 6;
    skin.attrs[4] = mesh_attr(4, 1, offsetof(SkinVertex, joints), 4, false);
    skin.attrs[5] = mesh_attr(5, 1, offsetof(SkinVertex, weights), 4, false);
    skin.strides[1] = sizeof(SkinVertex);
    skin_ = pipe("mesh_skin.vert", "mesh.frag", skin);
    rhi::PipelineDesc depth;
    depth.depth_write = true;
    depth.polygon_offset = true;
    depth.attr_count = 2;
    depth.attrs[0] = mesh_attr(0, 0, offsetof(Vertex3D, position), 3, false);
    depth.attrs[1] = mesh_attr(2, 0, offsetof(Vertex3D, u), 2, false);
    depth.strides[0] = sizeof(Vertex3D);
    depth_ = pipe("depth.vert", "depth.frag", depth);
    depth.attr_count = 4;
    depth.attrs[2] = mesh_attr(4, 1, offsetof(SkinVertex, joints), 4, false);
    depth.attrs[3] = mesh_attr(5, 1, offsetof(SkinVertex, weights), 4, false);
    depth.strides[1] = sizeof(SkinVertex);
    depth_skin_ = pipe("depth_skin.vert", "depth.frag", depth);
    rhi::PipelineDesc fol = mesh;
    fol.blend = rhi::Blend::Off;
    fol.depth_write = true;
    fol.cull = rhi::Cull::None;
    fol.attr_count = 6;
    fol.attrs[4] = mesh_attr(6, 1, 0, 4, true);
    fol.attrs[5] = mesh_attr(7, 1, 16, 2, true);
    fol.strides[0] = sizeof(Vertex3D);
    fol.strides[1] = sizeof(float) * 6;
    foliage_ = pipe("foliage.vert", "foliage.frag", fol);
    rhi::PipelineDesc sky;
    sky.depth_test = false;
    sky.depth_write = false;
    sky.cull = rhi::Cull::None;
    sky_ = pipe("sky.vert", "sky.frag", sky);
    rhi::PipelineDesc line;
    line.topology = rhi::Topology::Lines;
    line.cull = rhi::Cull::None;
    line.depth_write = false;
    line.attr_count = 2;
    line.attrs[0] = mesh_attr(0, 0, 0, 3, false);
    line.attrs[1] = mesh_attr(1, 0, 12, 4, false);
    line.strides[0] = sizeof(float) * 7;
    line_ = pipe("line.vert", "line.frag", line);
    line.depth_test = false;
    line_top_ = pipe("line.vert", "line.frag", line);
    rhi::PipelineDesc bill;
    bill.cull = rhi::Cull::None;
    bill.depth_write = false;
    bill.blend = rhi::Blend::Alpha;
    bill.attr_count = 3;
    bill.attrs[0] = mesh_attr(0, 0, 0, 3, false);
    bill.attrs[1] = mesh_attr(1, 0, 12, 4, false);
    bill.attrs[2] = mesh_attr(2, 0, 28, 2, false);
    bill.strides[0] = sizeof(float) * 9;
    bill_ = pipe("bill.vert", "bill.frag", bill);
    rhi::PipelineDesc fs;
    fs.depth_test = false;
    fs.depth_write = false;
    fs.cull = rhi::Cull::None;
    post_ = pipe("fullscreen.vert", "post.frag", fs);
    ssao_ = pipe("fullscreen.vert", "ssao.frag", fs);
    bloom_ = pipe("fullscreen.vert", "bloom.frag", fs);
    overlay_ = pipe("overlay.vert", "overlay.frag", fs);
    rhi::PipelineDesc rt = mesh;
    rt.ray_query = true;
    mesh_rt_ = pipe("mesh.vert", "mesh_rt.frag", rt);
    if (!mesh_ || !depth_ || !sky_ || !line_) return false;
    rhi::TargetDesc sd;
    sd.width = sd.height = kShadow;
    sd.color = false;
    sd.depth = true;
    shadow_ = device_->create_target(sd, error);
    ready_ = shadow_ != nullptr;
    return ready_;
  }

  void shutdown() override {
    if (!device_ || shut_) return;
    shut_ = true;
    for (auto& [id, m] : meshes_) free_mesh(m);
    meshes_.clear();
    for (auto& [id, m] : materials_) free_mat(m);
    materials_.clear();
    device_->destroy_target(color_);
    device_->destroy_target(shadow_);
    device_->destroy_target(ao_);
    device_->destroy_target(bloom_t_);
    device_->destroy_target(shown_);
    color_ = shadow_ = ao_ = bloom_t_ = shown_ = nullptr;
    device_->destroy_buffer(ubo_);
    device_->destroy_buffer(dyn_);
    ubo_ = dyn_ = nullptr;
    device_->destroy_texture(white_);
    device_->destroy_texture(flat_);
    device_->destroy_texture(overlay_tex_);
    white_ = flat_ = overlay_tex_ = nullptr;
    device_->destroy_sampler(wrap_);
    device_->destroy_sampler(clamp_);
    for (rhi::Pipeline* p : {mesh_, mesh_2s_, mesh_blend_, skin_, depth_, depth_skin_, foliage_, sky_, line_, line_top_, bill_, post_, ssao_, bloom_, overlay_, mesh_rt_}) {
      device_->destroy_pipeline(p);
    }
    ready_ = false;
  }

  MeshHandle create_mesh(const MeshData& mesh) override {
    if (!ready_ || !mesh.valid()) return {};
    GpuMesh m;
    rhi::BufferDesc vb;
    vb.kind = rhi::BufferKind::Vertex;
    vb.size = mesh.vertices.size() * sizeof(Vertex3D);
    m.vb = device_->create_buffer(vb, mesh.vertices.data());
    rhi::BufferDesc ib;
    ib.kind = rhi::BufferKind::Index;
    ib.size = mesh.indices.size() * sizeof(std::uint32_t);
    m.ib = device_->create_buffer(ib, mesh.indices.data());
    if (mesh.skinned()) {
      vb.size = mesh.skin.size() * sizeof(SkinVertex);
      m.skin = device_->create_buffer(vb, mesh.skin.data());
    }
    m.count = static_cast<std::uint32_t>(mesh.indices.size());
    m.verts = static_cast<std::uint32_t>(mesh.vertices.size());
    m.bytes = vb.size + ib.size;
    const std::uint32_t id = ++next_;
    meshes_[id] = m;
    tris_[id] = mesh.indices.size() / 3;
    cpu_pos_[id] = mesh.vertices;
    cpu_idx_[id] = mesh.indices;
    return {id};
  }
  MaterialHandle create_material(const MaterialDesc& d) override {
    if (!ready_) return {};
    GpuMat m;
    m.desc = d;
    auto up = [&](const std::shared_ptr<const Image>& img) -> rhi::Texture* {
      if (!img || !img->valid()) return nullptr;
      m.bytes += static_cast<std::size_t>(img->width) * img->height * 4;
      ++m.textures;
      return tex2d(img->rgba.data(), img->width, img->height, true);
    };
    m.albedo = up(d.base_color_image);
    m.normal = up(d.normal_image);
    m.mr = up(d.metal_rough_image);
    if (d.terrain()) {
      m.splat = up(d.splat_image);
      for (int i = 0; i < 4; ++i) m.layer[i] = up(d.layer_images[i]);
    }
    const std::uint32_t id = ++next_;
    materials_[id] = m;
    return {id};
  }
  void destroy_mesh(MeshHandle h) override {
    auto it = meshes_.find(h.id);
    if (it == meshes_.end()) return;
    free_mesh(it->second);
    meshes_.erase(it);
    tris_.erase(h.id);
    cpu_pos_.erase(h.id);
    cpu_idx_.erase(h.id);
  }
  void destroy_material(MaterialHandle h) override {
    auto it = materials_.find(h.id);
    if (it == materials_.end()) return;
    free_mat(it->second);
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
    spots_.clear();
    areas_.clear();
    bones_.clear();
    lines_.clear();
    lines_top_.clear();
    bills_.clear();
    foliage_batches_.clear();
    stats_ = {};
    bump_ = 0;
  }
  void add_point_light(const PointLight& l) override {
    if (points_.size() < kMaxPointLights) points_.push_back(l);
  }
  void add_spot_light(const SpotLight& l) override {
    if (spots_.size() < 4) spots_.push_back(l);
  }
  void add_area_light(const AreaLight& l) override {
    if (areas_.size() < 2) areas_.push_back(l);
  }
  void draw(MeshHandle mesh, MaterialHandle material, const Transform& t) override {
    draw_matrix(mesh, material, t.matrix(), true);
  }
  void draw_matrix(MeshHandle mesh, MaterialHandle material, const Mat4& world, bool cast_shadow) override {
    if (!meshes_.count(mesh.id)) return;
    draws_.push_back({mesh.id, material.id, world, cast_shadow, -1, 0});
  }
  void draw_skinned(MeshHandle mesh, MaterialHandle material, const Mat4& world, const Mat4* palette,
                    std::size_t bones, bool cast_shadow) override {
    if (!meshes_.count(mesh.id)) return;
    DrawItem d{mesh.id, material.id, world, cast_shadow, -1, 0};
    if (palette && bones && meshes_[mesh.id].skin) {
      d.bone = static_cast<int>(bones_.size());
      d.bones = static_cast<int>(std::min<std::size_t>(bones, kMaxBones));
      bones_.insert(bones_.end(), palette, palette + d.bones);
    }
    draws_.push_back(d);
  }
  void draw_line(const Vec3& a, const Vec3& b, const float* rgba, bool depth_test) override {
    const float c[4] = {rgba ? rgba[0] : 1, rgba ? rgba[1] : 1, rgba ? rgba[2] : 1, rgba ? rgba[3] : 1};
    auto& list = depth_test ? lines_ : lines_top_;
    list.insert(list.end(), {a.x, a.y, a.z, c[0], c[1], c[2], c[3], b.x, b.y, b.z, c[0], c[1], c[2], c[3]});
  }
  void draw_billboards(const std::vector<Billboard>& b) override {
    if (bills_.size() + b.size() > 16384) return;
    bills_.insert(bills_.end(), b.begin(), b.end());
  }
  void draw_foliage(const FoliageBatch& b) override {
    if (!meshes_.count(b.mesh.id) || !b.instances || !b.count) return;
    foliage_batches_.push_back(b);
    foliage_batches_.back().instances = nullptr;
    foliage_cpu_.emplace_back(b.instances, b.instances + b.count);
    foliage_batches_.back().instances = foliage_cpu_.back().data();
    stats_.foliage += b.count;
  }
  void set_overlay(const unsigned char* rgba, int w, int h, std::uint64_t version) override {
    if (!rgba) {
      device_->destroy_texture(overlay_tex_);
      overlay_tex_ = nullptr;
      overlay_version_ = 0;
      return;
    }
    if (version == overlay_version_ && overlay_tex_) return;
    device_->destroy_texture(overlay_tex_);
    overlay_tex_ = tex2d(rgba, w, h, false);
    overlay_version_ = version;
  }

  FrameStats end_frame() override {
    if (!ready_) return stats_;
    device_->begin_frame();
    rhi::CommandList* cmd = device_->commands();
    if (!cmd) {
      device_->end_frame();
      return stats_;
    }
    const Lighting& L = env_.lighting;
    int cascades = std::clamp(L.cascades, 1, 4);
    if (L.preset == 0) cascades = 1;
    const bool post = L.tonemap != 0 || L.bloom > 0 || L.ao != 0 || L.contrast != 1 || L.saturation != 1 || L.gain != 1;
    const bool shadows = env_.shadows && env_.sun_intensity > 0 && cascades > 0 && !L.raytracing;
    const Vec3 sun = normalize(env_.sun_direction);
    const Vec3 focus = camera_.target;
    const float sd = env_.shadow_distance;
    const Vec3 up = std::fabs(sun.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Mat4 light_view = Mat4::look_at(focus - sun * (sd * 2.0f), focus, up);
    FrameBlock frame{};
    for (int i = 0; i < cascades; ++i) {
      const float scale = sd * (static_cast<float>(i + 1) / static_cast<float>(cascades));
      frame.light_vp[i] = Mat4::ortho(-scale, scale, -scale, scale, 0.1f, sd * 4.0f) * light_view;
      (&frame.cascade_splits.x)[i] = scale;
    }
    const int tiles = cascades <= 1 ? 1 : 2;
    if (shadows && shadow_) {
      rhi::PassDesc sp;
      sp.target = shadow_;
      sp.color = false;
      sp.clear_color = false;
      sp.clear_depth = true;
      cmd->begin_pass(sp);
      cmd->set_pipeline(depth_);
      for (int c = 0; c < cascades; ++c) {
        const int side = kShadow / tiles;
        cmd->set_viewport((c % tiles) * side, (c / tiles) * side, side, side);
        for (const DrawItem& d : draws_) {
          if (!d.shadow) continue;
          DrawBlock db{};
          db.model = d.model;
          db.extra.x = static_cast<float>(c);
          upload_draw(cmd, db, d);
          const GpuMesh& m = meshes_[d.mesh];
          cmd->set_pipeline(d.bone >= 0 ? depth_skin_ : depth_);
          cmd->set_vertex_buffer(0, m.vb);
          if (m.skin) cmd->set_vertex_buffer(1, m.skin);
          cmd->set_index_buffer(m.ib);
          cmd->draw_indexed(m.count, 1, 0);
          ++stats_.shadow_casters;
        }
      }
      cmd->end_pass();
    }
    ensure_color();
    rhi::PassDesc mp;
    mp.target = offscreen_ ? color_ : nullptr;
    mp.rgba[0] = env_.sky_horizon[0];
    mp.rgba[1] = env_.sky_horizon[1];
    mp.rgba[2] = env_.sky_horizon[2];
    mp.rgba[3] = 1;
    cmd->begin_pass(mp);
    cmd->set_viewport(0, 0, width_, height_);
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Mat4 view = camera_.view();
    const Mat4 proj = camera_.projection(aspect);
    frame.view_proj = proj * view;
    frame.inv_view_proj = frame.view_proj.inverse();
    frame.cam = {camera_.position.x, camera_.position.y, camera_.position.z, 1};
    frame.sun_dir = {sun.x, sun.y, sun.z, 0};
    frame.sun_color = {env_.sun_color[0] * env_.sun_intensity, env_.sun_color[1] * env_.sun_intensity,
                       env_.sun_color[2] * env_.sun_intensity, 0};
    frame.sky_top = {env_.sky_top[0], env_.sky_top[1], env_.sky_top[2], 1};
    frame.sky_horizon = {env_.sky_horizon[0], env_.sky_horizon[1], env_.sky_horizon[2], 1};
    frame.ground = {env_.ground[0], env_.ground[1], env_.ground[2], 1};
    frame.fog = {env_.fog_color[0], env_.fog_color[1], env_.fog_color[2], env_.fog_density};
    frame.fog_extra = {env_.fog_height, env_.fog_falloff, 1.0f / kShadow, env_.ambient};
    frame.counts = {static_cast<float>(points_.size()), static_cast<float>(spots_.size()),
                    static_cast<float>(areas_.size()), static_cast<float>(cascades)};
    frame.flags = {shadows ? 1.0f : 0.0f, L.filter == 1 ? 1.0f : 0.0f, post ? (L.tonemap == 2 ? 2.0f : 1.0f) : 0.0f,
                   L.exposure};
    float dx = env_.wind_dir[0], dz = env_.wind_dir[1];
    const float wl = std::sqrt(dx * dx + dz * dz);
    if (wl > 1e-5f) { dx /= wl; dz /= wl; } else { dx = 1; dz = 0; }
    frame.wind = {dx, dz, env_.wind_strength, env_.wind_gust};
    frame.timev = {0, L.pcss, 0.4f, L.ao ? 1.0f : 0.0f};
    frame.atlas = {static_cast<float>(tiles), static_cast<float>(tiles), static_cast<float>(kShadow),
                   device_->shadow_flip_v() ? 1.0f : 0.0f};
    device_->clip_params(&frame.clip.x);
    frame.clip.w = L.hdri.empty() ? 0.0f : 1.0f;
    frame.grade = {L.contrast, L.saturation, L.gain, L.bloom};
    for (std::size_t i = 0; i < points_.size(); ++i) {
      const PointLight& l = points_[i];
      frame.point_pos[i] = {l.position.x, l.position.y, l.position.z, 1};
      frame.point_color[i] = {l.color[0] * l.intensity, l.color[1] * l.intensity, l.color[2] * l.intensity,
                              std::max(0.1f, l.range)};
    }
    for (std::size_t i = 0; i < spots_.size(); ++i) {
      const SpotLight& l = spots_[i];
      const Vec3 dir = normalize(l.direction);
      frame.spot_pos[i] = {l.position.x, l.position.y, l.position.z, 1};
      frame.spot_dir[i] = {dir.x, dir.y, dir.z, l.outer_cos};
      frame.spot_color[i] = {l.color[0] * l.intensity, l.color[1] * l.intensity, l.color[2] * l.intensity, l.inner_cos};
    }
    for (std::size_t i = 0; i < areas_.size(); ++i) {
      const AreaLight& l = areas_[i];
      frame.area_pos[i] = {l.position.x, l.position.y, l.position.z, 1};
      frame.area_u[i] = {l.axis_u.x, l.axis_u.y, l.axis_u.z, 0};
      frame.area_v[i] = {l.axis_v.x, l.axis_v.y, l.axis_v.z, 0};
      frame.area_color[i] = {l.color[0] * l.intensity, l.color[1] * l.intensity, l.color[2] * l.intensity, 1};
    }
    const std::size_t frame_off = bump(sizeof(FrameBlock));
    device_->update_buffer(ubo_, &frame, sizeof(frame), frame_off);
    auto bind_frame = [&]() {
      cmd->set_uniform(0, ubo_, frame_off, sizeof(FrameBlock));
      cmd->set_texture(6, device_->target_depth(shadow_), clamp_);
      cmd->set_texture(3, white_, wrap_);
      cmd->set_texture(4, flat_, wrap_);
      cmd->set_texture(5, white_, wrap_);
    };
    cmd->set_pipeline(sky_);
    bind_frame();
    cmd->draw(3, 1, 0);
    auto draw_meshes = [&](bool blend) {
      for (const DrawItem& d : draws_) {
        const GpuMat* mat = materials_.count(d.material) ? &materials_[d.material] : nullptr;
        const MaterialDesc& md = mat ? mat->desc : fallback_;
        const bool is_blend = md.alpha_mode == MaterialDesc::kAlphaBlend;
        if (is_blend != blend) continue;
        rhi::Pipeline* p = d.bone >= 0 ? skin_ : (is_blend ? mesh_blend_ : (md.double_sided ? mesh_2s_ : mesh_));
        if (L.raytracing && mesh_rt_ && !is_blend && d.bone < 0) p = mesh_rt_;
        cmd->set_pipeline(p);
        bind_frame();
        DrawBlock db{};
        db.model = d.model;
        db.albedo = {md.base_color[0], md.base_color[1], md.base_color[2], md.base_color[3]};
        db.emissive = {md.emissive[0], md.emissive[1], md.emissive[2], md.unlit ? 1.0f : 0.0f};
        db.mr = {md.metallic, md.roughness, md.alpha_cutoff, L.pom};
        db.draw_flags = {mat && mat->normal ? 1.0f : 0.0f, static_cast<float>(md.alpha_mode), d.bone >= 0 ? 1.0f : 0.0f,
                         md.terrain() ? 1.0f : 0.0f};
        db.extra.y = L.pom > 0 ? 1.0f : 0.0f;
        if (md.terrain()) {
          for (int i = 0; i < 4; ++i) {
            db.layer_tint[i] = {md.layer_tint[i][0], md.layer_tint[i][1], md.layer_tint[i][2], md.layer_tint[i][3]};
            (&db.layer_tiling.x)[i] = md.layer_tiling[i] > 0 ? md.layer_tiling[i] : 4;
          }
        }
        upload_draw(cmd, db, d);
        cmd->set_texture(3, mat && mat->albedo ? mat->albedo : white_, wrap_);
        cmd->set_texture(4, mat && mat->normal ? mat->normal : flat_, wrap_);
        cmd->set_texture(5, mat && mat->mr ? mat->mr : white_, wrap_);
        cmd->set_texture(6, device_->target_depth(shadow_), clamp_);
        if (mat && mat->splat) cmd->set_texture(7, mat->splat, wrap_);
        for (int i = 0; i < 4; ++i)
          if (mat && mat->layer[i]) cmd->set_texture(8 + i, mat->layer[i], wrap_);
        const GpuMesh& m = meshes_[d.mesh];
        cmd->set_vertex_buffer(0, m.vb);
        if (m.skin) cmd->set_vertex_buffer(1, m.skin);
        cmd->set_index_buffer(m.ib);
        cmd->draw_indexed(m.count, 1, 0);
        ++stats_.draws;
        stats_.triangles += tris_[d.mesh];
        if (d.bone >= 0) ++stats_.skinned;
      }
    };
    draw_meshes(false);
    if (foliage_ && !foliage_batches_.empty()) {
      cmd->set_pipeline(foliage_);
      bind_frame();
      for (const FoliageBatch& b : foliage_batches_) {
        if (!meshes_.count(b.mesh.id)) continue;
        DrawBlock db{};
        db.foliage_a = {b.color_a[0], b.color_a[1], b.color_a[2], b.mesh_height};
        db.foliage_b = {b.color_b[0], b.color_b[1], b.color_b[2], 1};
        db.extra.z = b.part_colors ? 1.0f : 0.0f;
        db.extra.w = b.sway;
        upload_draw(cmd, db, DrawItem{});
        std::vector<float> inst;
        inst.reserve(b.count * 6);
        for (std::size_t i = 0; i < b.count; ++i) {
          const FoliageInstance3D& f = b.instances[i];
          inst.insert(inst.end(), {f.x, f.y, f.z, f.yaw, f.scale, f.fade});
        }
        if (!dyn_ || dyn_size_ < inst.size() * 4) {
          device_->destroy_buffer(dyn_);
          rhi::BufferDesc bd;
          bd.kind = rhi::BufferKind::Vertex;
          dyn_size_ = inst.size() * 4 + 256;
          bd.size = dyn_size_;
          dyn_ = device_->create_buffer(bd, nullptr);
        }
        device_->update_buffer(dyn_, inst.data(), inst.size() * 4, 0);
        const GpuMesh& m = meshes_[b.mesh.id];
        cmd->set_vertex_buffer(0, m.vb);
        cmd->set_vertex_buffer(1, dyn_);
        cmd->set_index_buffer(m.ib);
        cmd->draw_indexed(m.count, static_cast<std::uint32_t>(b.count), 0);
      }
    }
    draw_meshes(true);
    auto lines = [&](std::vector<float>& v, rhi::Pipeline* p) {
      if (v.empty() || !p) return;
      cmd->set_pipeline(p);
      bind_frame();
      DrawBlock db{};
      upload_draw(cmd, db, DrawItem{});
      if (!dyn_ || dyn_size_ < v.size() * 4) {
        device_->destroy_buffer(dyn_);
        rhi::BufferDesc bd;
        bd.kind = rhi::BufferKind::Vertex;
        dyn_size_ = v.size() * 4 + 256;
        bd.size = dyn_size_;
        dyn_ = device_->create_buffer(bd, nullptr);
      }
      device_->update_buffer(dyn_, v.data(), v.size() * 4, 0);
      cmd->set_vertex_buffer(0, dyn_);
      cmd->set_index_buffer(nullptr);
      cmd->draw(static_cast<std::uint32_t>(v.size() / 7), 1, 0);
      stats_.lines += v.size() / 14;
    };
    lines(lines_, line_);
    lines(lines_top_, line_top_);
    if (!bills_.empty() && bill_) {
      std::vector<float> v;
      const Vec3 right = normalize(cross(camera_.forward(), camera_.up));
      const Vec3 upv = normalize(cross(right, camera_.forward()));
      for (const Billboard& b : bills_) {
        const float hs = b.size * 0.5f;
        const Vec3 c = b.position;
        const Vec3 corners[4] = {c - right * hs - upv * hs, c + right * hs - upv * hs, c + right * hs + upv * hs,
                                 c - right * hs + upv * hs};
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int k : idx) {
          v.insert(v.end(), {corners[k].x, corners[k].y, corners[k].z, b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3], uv[k][0], uv[k][1]});
        }
      }
      cmd->set_pipeline(bill_);
      bind_frame();
      DrawBlock db{};
      upload_draw(cmd, db, DrawItem{});
      device_->update_buffer(dyn_, v.data(), v.size() * 4, 0);
      cmd->set_vertex_buffer(0, dyn_);
      cmd->set_index_buffer(nullptr);
      cmd->draw(static_cast<std::uint32_t>(v.size() / 9), 1, 0);
    }
    if (overlay_tex_ && overlay_) {
      cmd->set_pipeline(overlay_);
      cmd->set_texture(3, overlay_tex_, clamp_);
      cmd->draw(3, 1, 0);
    }
    cmd->end_pass();
    if (post && offscreen_ && color_ && post_) {
      if (L.ao && ssao_) {
        ensure_aux(ao_, width_ / 2, height_ / 2);
        rhi::PassDesc ap;
        ap.target = ao_;
        cmd->begin_pass(ap);
        cmd->set_viewport(0, 0, width_ / 2, height_ / 2);
        cmd->set_pipeline(ssao_);
        bind_frame();
        cmd->set_texture(6, device_->target_depth(color_), clamp_);
        cmd->draw(3, 1, 0);
        cmd->end_pass();
      }
      if (L.bloom > 0 && bloom_) {
        ensure_aux(bloom_t_, width_ / 2, height_ / 2);
        rhi::PassDesc bp;
        bp.target = bloom_t_;
        cmd->begin_pass(bp);
        cmd->set_viewport(0, 0, width_ / 2, height_ / 2);
        cmd->set_pipeline(bloom_);
        cmd->set_uniform(0, ubo_, frame_off, sizeof(FrameBlock));
        cmd->set_texture(3, device_->target_color(color_), clamp_);
        cmd->draw(3, 1, 0);
        cmd->end_pass();
      }
    }
    show_post_ = false;
    if (post && offscreen_ && color_ && post_) {
      ensure_aux(shown_, width_, height_);
      if (shown_) {
        rhi::PassDesc pp;
        pp.target = shown_;
        cmd->begin_pass(pp);
        cmd->set_viewport(0, 0, width_, height_);
        cmd->set_pipeline(post_);
        cmd->set_uniform(0, ubo_, frame_off, sizeof(FrameBlock));
        cmd->set_texture(3, device_->target_color(color_), clamp_);
        cmd->set_texture(13, ao_ ? device_->target_color(ao_) : white_, clamp_);
        cmd->set_texture(14, bloom_t_ ? device_->target_color(bloom_t_) : white_, clamp_);
        cmd->draw(3, 1, 0);
        cmd->end_pass();
        show_post_ = true;
      }
    }
    stats_.lights = points_.size() + spots_.size() + areas_.size() + (env_.sun_intensity > 0 ? 1 : 0);
    stats_.meshes = meshes_.size();
    stats_.materials = materials_.size();
    device_->end_frame();
    return stats_;
  }

  std::uint64_t color_texture() const override {
    rhi::RenderTarget* shown = show_post_ && shown_ ? shown_ : color_;
    return shown ? device_->imgui_texture(device_->target_color(shown)) : 0;
  }
  bool read_pixels(std::vector<unsigned char>* rgba, int* w, int* h) const override {
    rhi::RenderTarget* shown = show_post_ && shown_ ? shown_ : color_;
    return shown && device_->read_color(shown, rgba, w, h);
  }

 private:
  rhi::Texture* tex2d(const void* rgba, int w, int h, bool mips) {
    rhi::TextureDesc td;
    td.width = w;
    td.height = h;
    td.mips = mips;
    return device_->create_texture(td, rgba);
  }
  void free_mesh(GpuMesh& m) {
    device_->destroy_buffer(m.vb);
    device_->destroy_buffer(m.ib);
    device_->destroy_buffer(m.skin);
  }
  void free_mat(GpuMat& m) {
    device_->destroy_texture(m.albedo);
    device_->destroy_texture(m.normal);
    device_->destroy_texture(m.mr);
    device_->destroy_texture(m.splat);
    for (auto* t : m.layer) device_->destroy_texture(t);
  }
  std::size_t bump(std::size_t size) {
    const std::size_t a = align_ ? align_ : 256;
    const std::size_t aligned = (size + a - 1) & ~(a - 1);
    if (bump_ + aligned > (8u << 20)) bump_ = 0;
    const std::size_t off = bump_;
    bump_ += aligned;
    return off;
  }
  void upload_draw(rhi::CommandList* cmd, const DrawBlock& db, const DrawItem& d) {
    const std::size_t off = bump(sizeof(DrawBlock));
    device_->update_buffer(ubo_, &db, sizeof(db), off);
    cmd->set_uniform(1, ubo_, off, sizeof(DrawBlock));
    if (d.bone >= 0) {
      Mat4 palette[128];
      for (int i = 0; i < 128; ++i) palette[i] = Mat4::identity();
      const int n = std::min(d.bones, 128);
      for (int i = 0; i < n; ++i) palette[i] = bones_[static_cast<std::size_t>(d.bone + i)];
      const std::size_t bo = bump(sizeof(palette));
      device_->update_buffer(ubo_, palette, sizeof(palette), bo);
      cmd->set_uniform(2, ubo_, bo, sizeof(palette));
    }
  }
  void ensure_color() {
    if (!offscreen_) return;
    if (color_ && cw_ == width_ && ch_ == height_) return;
    device_->destroy_target(color_);
    rhi::TargetDesc td;
    td.width = width_;
    td.height = height_;
    std::string err;
    color_ = device_->create_target(td, &err);
    cw_ = width_;
    ch_ = height_;
  }
  void ensure_aux(rhi::RenderTarget*& t, int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    device_->destroy_target(t);
    rhi::TargetDesc td;
    td.width = w;
    td.height = h;
    td.depth = false;
    std::string err;
    t = device_->create_target(td, &err);
  }

  rhi::Device* device_ = nullptr;
  bool ready_ = false;
  bool shut_ = false;
  bool offscreen_ = true;
  Environment env_{};
  Camera3D camera_{};
  int width_ = 1;
  int height_ = 1;
  int cw_ = 0;
  int ch_ = 0;
  FrameStats stats_{};
  std::uint32_t next_ = 0;
  std::size_t align_ = 256;
  std::size_t bump_ = 0;
  std::size_t dyn_size_ = 0;
  rhi::Buffer* ubo_ = nullptr;
  rhi::Buffer* dyn_ = nullptr;
  rhi::Texture* white_ = nullptr;
  rhi::Texture* flat_ = nullptr;
  rhi::Texture* overlay_tex_ = nullptr;
  rhi::Sampler* wrap_ = nullptr;
  rhi::Sampler* clamp_ = nullptr;
  rhi::Pipeline* mesh_ = nullptr;
  rhi::Pipeline* mesh_2s_ = nullptr;
  rhi::Pipeline* mesh_blend_ = nullptr;
  rhi::Pipeline* mesh_rt_ = nullptr;
  rhi::Pipeline* skin_ = nullptr;
  rhi::Pipeline* depth_ = nullptr;
  rhi::Pipeline* depth_skin_ = nullptr;
  rhi::Pipeline* foliage_ = nullptr;
  rhi::Pipeline* sky_ = nullptr;
  rhi::Pipeline* line_ = nullptr;
  rhi::Pipeline* line_top_ = nullptr;
  rhi::Pipeline* bill_ = nullptr;
  rhi::Pipeline* post_ = nullptr;
  rhi::Pipeline* ssao_ = nullptr;
  rhi::Pipeline* bloom_ = nullptr;
  rhi::Pipeline* overlay_ = nullptr;
  rhi::RenderTarget* color_ = nullptr;
  rhi::RenderTarget* shadow_ = nullptr;
  rhi::RenderTarget* ao_ = nullptr;
  rhi::RenderTarget* bloom_t_ = nullptr;
  rhi::RenderTarget* shown_ = nullptr;
  bool show_post_ = false;
  std::unordered_map<std::uint32_t, GpuMesh> meshes_;
  std::unordered_map<std::uint32_t, GpuMat> materials_;
  std::unordered_map<std::uint32_t, std::size_t> tris_;
  std::unordered_map<std::uint32_t, std::vector<Vertex3D>> cpu_pos_;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> cpu_idx_;
  std::vector<DrawItem> draws_;
  std::vector<PointLight> points_;
  std::vector<SpotLight> spots_;
  std::vector<AreaLight> areas_;
  std::vector<Mat4> bones_;
  std::vector<float> lines_;
  std::vector<float> lines_top_;
  std::vector<Billboard> bills_;
  std::vector<FoliageBatch> foliage_batches_;
  std::vector<std::vector<FoliageInstance3D>> foliage_cpu_;
  MaterialDesc fallback_{};
  std::uint64_t overlay_version_ = 0;
};

}  // namespace

std::unique_ptr<Renderer3D> make_rhi_renderer(rhi::Device* device) {
  return std::make_unique<RhiRenderer>(device);
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
