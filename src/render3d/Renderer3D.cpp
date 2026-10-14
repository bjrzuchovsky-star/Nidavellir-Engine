#include "render3d/Renderer3D.h"

#include <unordered_map>

namespace ts {
namespace tombstone {
namespace render3d {

MeshData make_cube(float size) {
  MeshData m;
  m.name = "cube";
  const float h = size * 0.5f;
  const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                           {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const Vec3& n : normals) {
    // Two axes spanning the face.
    const Vec3 a = std::fabs(n.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 b = cross(n, a);
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const float sa[4] = {-1, 1, 1, -1};
    const float sb[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
      Vertex3D v;
      v.position = (n + a * sa[i] + b * sb[i]) * h;
      v.normal = n;
      v.u = uv[i][0];
      v.v = uv[i][1];
      m.vertices.push_back(v);
    }
    const std::uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    for (std::uint32_t i : idx) m.indices.push_back(base + i);
  }
  return m;
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
