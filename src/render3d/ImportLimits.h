#pragma once

// Caps for large model imports. A character file can be a gigabyte of
// meshes and 4K textures; these keep one import from taking the editor
// down or stalling a frame for seconds. Callers that need the original
// pixels (a tiny test image) stay under the caps and are unchanged.

#include <cstddef>
#include <cstdint>

namespace ts {
namespace tombstone {
namespace render3d {

// Longest edge stored for a texture imported with a model. 0 in the
// .meta means "use this default". The hard cap is what we refuse to
// decode at all (a corrupt header, or an 16K plate).
inline constexpr int kDefaultMaxTextureEdge = 2048;
inline constexpr int kHardMaxTextureEdge = 8192;

// Above this, a fitted collider is a sampled convex hull, not a
// per-triangle decomposition of the full mesh.
inline constexpr std::size_t kColliderFullTriangleLimit = 12000;
// A triangle-mesh collider (static Jolt mesh) above this becomes the
// convex hull. Jolt on millions of triangles stalls and can abort.
inline constexpr std::size_t kMeshColliderTriangleLimit = 48000;
// One split part is simplified down to this many triangles.
inline constexpr std::size_t kSplitPartTriangleLimit = 80000;

// One mesh we will allocate. Past this, import fails with a message
// instead of asking the OS for gigabytes (overcommit can kill the process
// before bad_alloc).
inline constexpr std::size_t kMaxImportVertices = 8000000;
inline constexpr std::size_t kMaxImportTriangles = 16000000;

// GPU upload budget for one frame in the editor viewport. 0 = upload the
// whole resource (shots, tiny meshes).
inline constexpr std::size_t kUploadBudgetBytes = 4u * 1024u * 1024u;

// Scene loads of files at least this big run off the frame thread.
inline constexpr std::uintmax_t kAsyncModelBytes = 2ull * 1024ull * 1024ull;

// Editor: model files at least this big are imported on a worker.
inline constexpr std::uintmax_t kDeferImportBytes = 4ull * 1024ull * 1024ull;

// limit_cloud's careful (quadratic) reducer is only for clouds this small.
// Bigger clouds are stride-sampled in linear time.
inline constexpr std::size_t kHullQualityPointLimit = 2500;

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
