#include "render3d/UploadStage.h"

#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

std::size_t mesh_upload_bytes(const MeshData& mesh) {
  return mesh.vertices.size() * sizeof(Vertex3D) + mesh.indices.size() * sizeof(std::uint32_t) +
         mesh.skin.size() * sizeof(SkinVertex);
}

MeshSlice plan_mesh_slice(const MeshData& mesh, std::size_t index_cursor, std::size_t budget) {
  MeshSlice s;
  s.index_begin = index_cursor;
  if (index_cursor >= mesh.indices.size()) return s;
  std::size_t remain = mesh.indices.size() - index_cursor;
  remain -= remain % 3;
  if (remain == 0) return s;
  const std::size_t tri_bytes = 3 * sizeof(Vertex3D) + 3 * sizeof(std::uint32_t) +
                                (mesh.skinned() ? 3 * sizeof(SkinVertex) : 0);
  std::size_t nidx = remain;
  if (budget > 0 && tri_bytes > 0) {
    const std::size_t max_tri = std::max<std::size_t>(1, budget / tri_bytes);
    nidx = std::min(nidx, max_tri * 3);
  }
  s.index_count = nidx;
  s.bytes = (nidx / 3) * tri_bytes;
  return s;
}

bool bake_mesh_slice(const MeshData& src, const MeshSlice& slice, MeshData* dst) {
  if (!dst) return false;
  *dst = MeshData{};
  dst->name = src.name;
  if (slice.index_count < 3 || slice.index_begin >= src.indices.size()) return false;
  const std::size_t end = std::min(src.indices.size(), slice.index_begin + slice.index_count);
  const bool skin = src.skinned();
  std::unordered_map<std::uint32_t, std::uint32_t> remap;
  remap.reserve((end - slice.index_begin) / 2 + 1);
  auto add_vert = [&](std::uint32_t i) -> std::uint32_t {
    auto it = remap.find(i);
    if (it != remap.end()) return it->second;
    const std::uint32_t nv = static_cast<std::uint32_t>(dst->vertices.size());
    dst->vertices.push_back(src.vertices[i]);
    if (skin) dst->skin.push_back(i < src.skin.size() ? src.skin[i] : SkinVertex{});
    remap.emplace(i, nv);
    return nv;
  };
  for (std::size_t k = slice.index_begin; k + 2 < end; k += 3) {
    const std::uint32_t a = src.indices[k], b = src.indices[k + 1], c = src.indices[k + 2];
    if (a >= src.vertices.size() || b >= src.vertices.size() || c >= src.vertices.size()) continue;
    dst->indices.push_back(add_vert(a));
    dst->indices.push_back(add_vert(b));
    dst->indices.push_back(add_vert(c));
  }
  return dst->valid();
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
