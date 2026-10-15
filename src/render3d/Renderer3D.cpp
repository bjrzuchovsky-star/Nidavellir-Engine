#include "render3d/Renderer3D.h"
#include "profiler/Profiler.h"

#include <unordered_map>

namespace ts {
namespace tombstone {
namespace render3d {

void Renderer3D::draw_foliage(const FoliageBatch& b) {
  for (std::size_t i = 0; i < b.count && b.instances; ++i) {
    const FoliageInstance3D& f = b.instances[i];
    if (f.fade <= 0.0f) continue;
    const Mat4 m = Mat4::trs({f.x, f.y, f.z}, Quat::from_euler_deg({0, degrees(f.yaw), 0}),
                             {f.scale, f.scale, f.scale});
    draw_matrix(b.mesh, MaterialHandle{}, m, b.cast_shadow);
  }
}

namespace {

class NullRenderer3D final : public Renderer3D {
 public:
  const char* backend_name() const override { return "null"; }
  bool init(std::string* /*error*/) override {
    ready_ = true;
    return true;
  }
  void shutdown() override {
    meshes_.clear();
    materials_.clear();
    ready_ = false;
  }
  MeshHandle create_mesh(const MeshData& mesh) override {
    if (!ready_ || !mesh.valid()) return {};
    const std::uint32_t id = ++next_;
    meshes_[id] = mesh.indices.size() / 3;
    return {id};
  }
  MaterialHandle create_material(const MaterialDesc& /*material*/) override {
    if (!ready_) return {};
    const std::uint32_t id = ++next_;
    materials_[id] = true;
    return {id};
  }
  void destroy_mesh(MeshHandle h) override { meshes_.erase(h.id); }
  void destroy_material(MaterialHandle h) override { materials_.erase(h.id); }
  void begin_frame(const Camera3D& /*camera*/, int /*w*/, int /*h*/) override {
    frame_ = {};
  }
  void draw(MeshHandle mesh, MaterialHandle material,
            const Transform& /*transform*/) override {
    count(mesh, material);
  }
  void draw_matrix(MeshHandle mesh, MaterialHandle material,
                   const Mat4& /*world*/, bool /*cast_shadow*/) override {
    count(mesh, material);
  }
  void draw_skinned(MeshHandle mesh, MaterialHandle material, const Mat4& /*world*/,
                    const Mat4* palette, std::size_t bones, bool /*cast_shadow*/) override {
    const std::size_t before = frame_.draws;
    count(mesh, material);
    if (frame_.draws != before && palette && bones > 0) ++frame_.skinned;
  }
  void draw_foliage(const FoliageBatch& b) override {
    auto it = meshes_.find(b.mesh.id);
    if (it == meshes_.end() || !b.instances || b.count == 0) return;
    ++frame_.draws;
    frame_.foliage += b.count;
    frame_.triangles += it->second * b.count;
  }
  void add_point_light(const PointLight& /*light*/) override {
    if (frame_.lights < kMaxPointLights) ++frame_.lights;
  }
  void draw_line(const Vec3&, const Vec3&, const float*, bool) override {
    ++frame_.lines;
  }
  void count(MeshHandle mesh, MaterialHandle material) {
    auto it = meshes_.find(mesh.id);
    if (it == meshes_.end() || !materials_.count(material.id)) return;
    ++frame_.draws;
    frame_.triangles += it->second;
  }
  FrameStats end_frame() override {
    frame_.meshes = meshes_.size();
    frame_.materials = materials_.size();
    return frame_;
  }

 private:
  bool ready_ = false;
  std::uint32_t next_ = 0;
  std::unordered_map<std::uint32_t, std::size_t> meshes_;  // id -> triangles
  std::unordered_map<std::uint32_t, bool> materials_;
  FrameStats frame_;
};

}  // namespace

std::unique_ptr<Renderer3D> make_null_renderer3d() {
  return std::make_unique<NullRenderer3D>();
}

void report_frame_stats(const FrameStats& st) {
  profiler::Profiler& p = profiler::Profiler::get();
  if (!p.enabled()) return;
  p.counter("render.draws", static_cast<double>(st.draws));
  p.counter("render.triangles", static_cast<double>(st.triangles));
  p.counter("render.lights", static_cast<double>(st.lights));
  p.counter("render.shadow_casters", static_cast<double>(st.shadow_casters));
  p.counter("render.lines", static_cast<double>(st.lines));
  p.counter("render.skinned", static_cast<double>(st.skinned));
  p.counter("assets.meshes", static_cast<double>(st.meshes));
  p.counter("assets.materials", static_cast<double>(st.materials));
  p.counter("assets.textures", static_cast<double>(st.textures));
  p.counter("mem.gpu_mb", static_cast<double>(st.gpu_bytes) / (1024.0 * 1024.0));
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
