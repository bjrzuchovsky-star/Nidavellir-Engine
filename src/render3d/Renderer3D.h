#pragma once

// 3D renderer interface (sketch). No GPU backend yet: NullRenderer3D
// accepts and validates everything and counts what a frame would draw, so
// editor / runtime code can be written against the interface today.

#include "render3d/Camera3D.h"
#include "render3d/Mesh.h"

#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct FrameStats {
  std::size_t draws = 0;
  std::size_t triangles = 0;
};

class Renderer3D {
 public:
  virtual ~Renderer3D() = default;
  virtual const char* backend_name() const = 0;  // "null", later "gl", "vulkan"
  virtual bool init(std::string* error) = 0;
  virtual void shutdown() = 0;

  virtual MeshHandle create_mesh(const MeshData& mesh) = 0;
  virtual MaterialHandle create_material(const MaterialDesc& material) = 0;
  virtual void destroy_mesh(MeshHandle h) = 0;
  virtual void destroy_material(MaterialHandle h) = 0;

  virtual void begin_frame(const Camera3D& camera, int width, int height) = 0;
  virtual void draw(MeshHandle mesh, MaterialHandle material,
                    const Transform& transform) = 0;
  virtual FrameStats end_frame() = 0;
};

// Backend that draws nothing (the only one so far).
std::unique_ptr<Renderer3D> make_null_renderer3d();

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
