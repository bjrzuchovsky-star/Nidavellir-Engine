#pragma once

// Heightmap terrain: a square grid of heights (metres) plus a 4-layer
// splat map, edited with sculpt / paint brushes, cut into LOD chunk meshes
// for the renderer and handed to Jolt as a heightfield. Foliage instances
// (terrain/Foliage.h) live in the same data file (.tsterrain).
//
// Coordinates are terrain-local: the grid is centred on the owning
// entity's position, x and z run from -size/2 to +size/2, y is height.
// Rotation and scale of the entity are ignored (terrains are axis-aligned).

#include "render3d/Mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace terrain {

using render3d::Vec3;

inline constexpr int kLayers = 4;

struct FoliageInstance {
  std::uint16_t type = 0;  // index into the component's foliage types
  float x = 0, y = 0, z = 0;  // local; y follows the ground (refit after sculpting)
  float yaw = 0;           // radians
  float scale = 1;
  bool operator==(const FoliageInstance& o) const = default;
};

// Grid-space rectangle of touched vertices (inclusive); empty when i0 > i1.
struct Rect {
  int i0 = 1, j0 = 1, i1 = 0, j1 = 0;
  bool empty() const { return i0 > i1 || j0 > j1; }
  void merge(const Rect& o);
};

// What an edit touched (caches replay the recent log instead of rebuilding).
struct Edit {
  std::uint64_t revision = 0;
  Rect rect;
  bool heights = false, splat = false, foliage = false;
};

struct TerrainData {
  int res = 0;          // vertices per side: 2^n + 1 (33 .. 1025)
  float size = 0;       // metres across
  std::vector<float> heights;          // res * res, row-major (z rows, x columns)
  std::vector<std::uint8_t> splat;     // res * res * 4 layer weights (sum ~255)
  std::vector<FoliageInstance> foliage;
  std::uint64_t revision = 0;          // bumped by every edit (caches watch it)
  std::vector<Edit> edits;             // recent edits (bounded), oldest first

  bool valid() const {
    return res >= 2 && size > 0 && heights.size() == static_cast<std::size_t>(res) * res &&
           splat.size() == heights.size() * 4;
  }
  float cell() const { return size / static_cast<float>(res - 1); }
  float& h(int i, int j) { return heights[static_cast<std::size_t>(j) * res + i]; }
  float h(int i, int j) const { return heights[static_cast<std::size_t>(j) * res + i]; }
  // Grid coordinates of a local position (fractional).
  float gx(float x) const { return (x + size * 0.5f) / cell(); }
  float gz(float z) const { return (z + size * 0.5f) / cell(); }
  float wx(int i) const { return -size * 0.5f + static_cast<float>(i) * cell(); }
  float wz(int j) const { return -size * 0.5f + static_cast<float>(j) * cell(); }
};

// Record an edit (bumps revision). A full-grid rect for wholesale changes.
void note_edit(TerrainData& t, const Rect& r, bool heights, bool splat, bool foliage);
Rect full_rect(const TerrainData& t);
// Union of what changed after `since`. False when the log no longer
// reaches back that far (rebuild everything).
bool edits_since(const TerrainData& t, std::uint64_t since, Rect* heights, bool* splat, bool* foliage);

bool valid_resolution(int res);
// Flat terrain at height 0 painted with layer 0.
void reset(TerrainData& t, int res, float size);
// Rolling hills: fractal value noise, amplitude metres, feature size metres.
void generate_hills(TerrainData& t, std::uint32_t seed, float amplitude, float feature);

float height_at(const TerrainData& t, float x, float z);   // bilinear, clamped to the edge
Vec3 normal_at(const TerrainData& t, float x, float z);
bool inside(const TerrainData& t, float x, float z);
// Splat weights (0..1) at a local point.
void weights_at(const TerrainData& t, float x, float z, float out[kLayers]);
// Ray against the heightfield (local space). t_out = distance along dir.
bool raycast(const TerrainData& t, const Vec3& origin, const Vec3& dir, float max_dist,
             float* t_out);

// ---- Brushes ------------------------------------------------------------

enum class BrushOp { Raise, Lower, Smooth, Flatten, Noise, Paint };
const char* brush_op_name(BrushOp op);
bool brush_op_from(const std::string& name, BrushOp* out);

struct Brush {
  BrushOp op = BrushOp::Raise;
  float radius = 6.0f;     // metres
  float strength = 4.0f;   // metres / second (sculpt) or weight / second (paint, x0.25)
  float hardness = 0.5f;   // 0 soft falloff .. 1 hard edge
  float target = 0.0f;     // flatten height (metres)
  int layer = 1;           // paint layer 0..3
  float noise_scale = 4.0f;  // noise feature size (metres)
  std::uint32_t seed = 1;
};

// Falloff weight 0..1 at distance d of a brush.
float brush_weight(const Brush& b, float d);
// One brush step of dt seconds centred at local (x, z). Returns the dirty
// rect (heights or splat). Foliage on sculpted ground is refit.
Rect apply_brush(TerrainData& t, const Brush& b, float x, float z, float dt);
void refit_foliage(TerrainData& t, const Rect& r);

// ---- Chunks / LOD -------------------------------------------------------

struct ChunkLayout {
  int quads = 32;   // grid quads per chunk side (power of two)
  int count = 0;    // chunks per side
  int lods = 1;     // levels: lod n uses every 2^n-th vertex
};
ChunkLayout chunk_layout(const TerrainData& t, int chunk_quads);
// The chunk mesh at a LOD (local positions, uv = whole-terrain 0..1 for
// the splat map, smooth normals, skirts hanging `skirt` metres down to hide
// cracks between LODs).
render3d::MeshData build_chunk(const TerrainData& t, const ChunkLayout& l, int cx, int cz, int lod,
                               float skirt);
// LOD for a chunk whose centre is `distance` metres away; distances[i] is
// where lod i+1 starts (count entries, ascending).
int select_lod(float distance, const float* distances, int count, int lods);
// Chunks touched by a grid rect.
void chunks_in(const ChunkLayout& l, const Rect& r, std::vector<int>* out);  // cz * count + cx
// Local-space bounds of a chunk.
render3d::Aabb chunk_bounds(const TerrainData& t, const ChunkLayout& l, int cx, int cz);

// Splat weights as an RGBA image (res x res), for the GPU.
render3d::Image splat_image(const TerrainData& t);
// Procedural layer textures for projects without art: "builtin:grass",
// "builtin:dirt", "builtin:rock", "builtin:sand", "builtin:snow".
bool builtin_layer_image(const std::string& name, render3d::Image* out);
const std::vector<std::string>& builtin_layer_names();

// ---- Files --------------------------------------------------------------

// Binary .tsterrain: "TSTR", version, res, size, heights, splat, foliage.
std::string encode(const TerrainData& t);
bool decode(const std::string& bytes, TerrainData* out, std::string* error);
bool save_file(const std::string& path, const TerrainData& t, std::string* error);
bool load_file(const std::string& path, TerrainData* out, std::string* error);

// Deterministic hash helpers shared with Foliage.
std::uint32_t hash3(std::uint32_t a, std::uint32_t b, std::uint32_t c);
float hash_unit(std::uint32_t a, std::uint32_t b, std::uint32_t c);
float value_noise(float x, float z, std::uint32_t seed);  // 0..1 smooth

}  // namespace terrain
}  // namespace tombstone
}  // namespace ts
