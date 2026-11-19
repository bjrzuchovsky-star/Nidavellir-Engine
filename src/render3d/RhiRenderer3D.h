#pragma once

#include "render3d/Renderer3D.h"

namespace ts {
namespace tombstone {
namespace rhi {
class Device;
}
namespace render3d {

// Forward renderer on the RHI. The same shaders run on OpenGL, Vulkan and
// Direct3D 12. Classic lighting (one cascade, 3x3 PCF, Reinhard) is the
// default so a scene matches the OpenGL renderer.
std::unique_ptr<Renderer3D> make_rhi_renderer(rhi::Device* device);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
