#include "render3d/Renderer3D.h"

#include <unordered_map>

namespace ts {
namespace tombstone {
namespace render3d {

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
  FrameStats end_frame() override { return frame_; }

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

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
