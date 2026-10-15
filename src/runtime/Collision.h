#pragma once

// Collision shapes shared by the runtime World, the editor's collider
// overlay (K) and ts_game: axis-aligned boxes in world space. No UI, no GL.

#include "scene/SceneData.h"
#include "scene/TileMap.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

struct WorldRect;

struct Aabb {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
};

// Touching edges do not overlap (riders stop flush against a wall and then
// slide along it), and neither do overlaps thinner than kContactSlop.
inline constexpr float kContactSlop = 1.0e-3f;
bool overlaps(const Aabb& a, const Aabb& b);

// The entity's collider box with its top-left corner at (x, y). Without a
// collider: the entity rect.
Aabb collider_box(const Entity2D& e, float x, float y);
inline Aabb collider_box(const Entity2D& e) {
  return collider_box(e, e.x, e.y);
}

// The collider as an oriented box: the collider rect turned by the entity
// rotation (degrees, clockwise on screen) about the entity centre. c / s
// are cos / sin of the turn. Riders still use collider_box (riders never
// turn); static solids, triggers and rigid bodies that are turned use this.
struct Obb {
  float cx = 0.0f, cy = 0.0f;  // centre
  float hx = 0.0f, hy = 0.0f;  // half extents (unrotated)
  float c = 1.0f, s = 0.0f;
};
bool turned(const Entity2D& e);  // rotation is not a multiple of 360
Obb collider_obb(const Entity2D& e, float x, float y);
inline Obb collider_obb(const Entity2D& e) { return collider_obb(e, e.x, e.y); }
Aabb bounds(const Obb& b);
// Separating-axis test (touching / thinner than kContactSlop is apart).
bool overlaps(const Aabb& a, const Obb& b);
// Shifts t of `a` along `axis` (0 = x, 1 = y) for which a overlaps b form
// the open interval (*lo, *hi). False when no shift overlaps.
bool overlap_shifts(const Aabb& a, const Obb& b, int axis, float* lo,
                    float* hi);

// What the collision overlay (editor K, ts_game K) outlines.
enum class OverlayKind { SolidTile, StaticSolid, DynamicSolid, Trigger };

struct OverlayBox {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  OverlayKind kind = OverlayKind::SolidTile;
  bool active = false;  // trigger with somebody inside (play only)
  std::uint64_t entity = 0;
  // Turned colliders: draw the box rotated by `rotation` degrees about its
  // centre ((x0 + x1) / 2, (y0 + y1) / 2); x0..y1 is the unrotated box.
  float rotation = 0.0f;
  bool body = false;  // a Box2D rigid body
  ColliderData::Shape shape = ColliderData::Shape::Box;
};

// The overlay shape's outline in world space, closed (box: 4 corners;
// circle / capsule: a polygon), turned by `rotation` about the centre.
std::vector<std::pair<float, float>> overlay_outline(const OverlayBox& b);



// Overlay for one entity drawn with its top-left corner at (x, y): its
// collider, and for a TileMap its solid tiles (each row merged into runs).
// Culled to `view`.
void append_overlay(const Entity2D& e, float x, float y,
                    const TileSolidity& solidity, const WorldRect& view,
                    std::vector<OverlayBox>* out);

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
