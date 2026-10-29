#pragma once

// Polygon collider traced from a sprite's alpha (docs/physics.md). The
// outline follows the opaque pixels. A concave shape is cut into convex
// pieces small enough for Box2D (8 corners each). Holes are not traced:
// a ring becomes its outer silhouette.

#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

struct SpritePolyOptions {
  int alpha_threshold = 16;  // 0..255; pixels at or above count as solid
  int max_vertices = 8;      // per convex piece
  int max_pieces = 8;
};

// `rgba` is width * height * 4, top row first. Points are in pixels, origin
// at the image's top-left, y down, and cover the solid pixels (a full
// rectangle comes back as that rectangle). Empty when nothing is solid.
std::vector<std::vector<std::pair<float, float>>> trace_sprite_polygon(
    const unsigned char* rgba, int width, int height, const SpritePolyOptions& opt = {});

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
