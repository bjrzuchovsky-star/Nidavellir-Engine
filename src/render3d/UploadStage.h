#pragma once

// Splits a mesh into frame-sized GPU uploads so one create_mesh of a
// multi-million-triangle model does not stall the frame. The editor's
// SceneView uses this; --smoke-bigmodel drives it headless.

#include "render3d/Mesh.h"

#include <cstddef>

namespace ts {
namespace tombstone {
namespace render3d {

struct MeshSlice {
  std::size_t index_begin = 0;  // into MeshData::indices, multiple of 3
  std::size_t index_count = 0;
  std::size_t bytes = 0;  // upper bound on the GPU buffer this slice needs
};

// Bytes a full upload of `mesh` would push (vertices + indices + skin).
std::size_t mesh_upload_bytes(const MeshData& mesh);

// The next slice starting at `index_cursor`. budget == 0 means the rest
// of the mesh. Always at least one triangle when any remain, so a tiny
// budget still makes progress.
MeshSlice plan_mesh_slice(const MeshData& mesh, std::size_t index_cursor, std::size_t budget);

// Compact the slice into its own mesh (vertices remapped). False when the
// slice has no valid triangle.
bool bake_mesh_slice(const MeshData& src, const MeshSlice& slice, MeshData* dst);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
