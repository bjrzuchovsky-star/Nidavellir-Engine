#pragma once

// 3D navigation: a Recast navmesh built from static triangles (ground,
// floors, walls, crates), Detour path queries, off-mesh links (jumps,
// ladders, drops) and a DetourCrowd for steering with local avoidance.
// Recast / Detour (zlib licence) are fetched by hash at configure time.

#include "ai/AiMath.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace ai {

struct NavBuildConfig {
  float cell_size = 0.2f;      // xz voxel (m)
  float cell_height = 0.1f;    // y voxel (m)
  float agent_height = 1.8f;
  float agent_radius = 0.4f;
  float max_climb = 0.45f;     // step height
  float max_slope = 45.0f;     // degrees
  float region_min = 4.0f;     // cells (sqrt of area)
  float region_merge = 20.0f;
  float edge_max_len = 12.0f;  // m
  float edge_max_error = 1.3f;
  float detail_sample_dist = 6.0f;
  float detail_max_error = 1.0f;
  bool operator==(const NavBuildConfig& o) const = default;
};

struct OffMeshLink {
  V3 a, b;
  float radius = 0.5f;
  bool bidirectional = true;
  std::uint32_t user_id = 0;
};

struct NavGeometry {
  std::vector<float> verts;  // x y z
  std::vector<int> tris;     // 3 indices each
  void add_tri(const V3& a, const V3& b, const V3& c);
  // Axis-aligned box (all six faces, outward winding).
  void add_box(const V3& center, const V3& half, float yaw_rad = 0.0f);
  // Flat quad (top face only) centred at `center`.
  void add_plane(const V3& center, float half_x, float half_z, float yaw_rad = 0.0f);
  std::size_t tri_count() const { return tris.size() / 3; }
};

struct NavPath {
  bool found = false;
  bool partial = false;          // the goal was unreachable: ends nearest
  std::vector<V3> points;        // straight path (corners), start..end
  std::vector<std::uint8_t> offmesh;  // 1 where the segment starting there is an off-mesh link
  float length() const;
};

struct NavStats {
  int polys = 0;
  int verts = 0;
  int detail_tris = 0;
  int offmesh = 0;
  double build_ms = 0.0;
};

class NavMesh {
 public:
  NavMesh();
  ~NavMesh();
  NavMesh(const NavMesh&) = delete;
  NavMesh& operator=(const NavMesh&) = delete;

  bool build(const NavGeometry& geo, const NavBuildConfig& cfg, const std::vector<OffMeshLink>& links,
             std::string* error);
  bool ready() const;
  void clear();
  const NavStats& stats() const { return stats_; }
  const NavBuildConfig& config() const { return cfg_; }

  // Closest point on the mesh within `extents` (half box); false if none.
  bool nearest(const V3& p, V3* out, const V3& extents = {2.0f, 4.0f, 2.0f}) const;
  NavPath find_path(const V3& from, const V3& to) const;
  // Walkable straight line? (Detour raycast on the surface.)
  bool raycast(const V3& from, const V3& to, V3* hit = nullptr) const;
  // Random point within `radius` of `center` (deterministic for a seed).
  bool random_point(const V3& center, float radius, std::uint64_t* seed, V3* out) const;

  // Debug: every detail triangle (9 floats each) and every polygon edge.
  void debug_triangles(std::vector<float>* tris) const;
  void debug_edges(std::vector<V3>* segments) const;
  const std::vector<OffMeshLink>& links() const { return links_; }

  struct Impl;
  Impl* impl() const { return impl_.get(); }

 private:
  std::unique_ptr<Impl> impl_;
  NavStats stats_;
  NavBuildConfig cfg_;
  std::vector<OffMeshLink> links_;
};

struct CrowdAgentParams {
  float radius = 0.4f;
  float height = 1.8f;
  float max_speed = 3.5f;
  float max_accel = 10.0f;
  bool avoidance = true;
  int avoidance_quality = 2;   // 0 low .. 3 high
  float separation = 2.0f;     // weight
};

class NavCrowd {
 public:
  explicit NavCrowd(NavMesh& mesh, int max_agents = 64, float max_radius = 1.0f);
  ~NavCrowd();
  NavCrowd(const NavCrowd&) = delete;
  NavCrowd& operator=(const NavCrowd&) = delete;

  int add(const V3& pos, const CrowdAgentParams& params);  // -1 when full / off mesh
  void remove(int agent);
  bool set_target(int agent, const V3& target);
  void stop(int agent);
  void set_params(int agent, const CrowdAgentParams& params);
  void update(float dt);
  // Teleport (re-snap to the mesh).
  bool warp(int agent, const V3& pos);

  bool active(int agent) const;
  V3 position(int agent) const;
  V3 velocity(int agent) const;
  bool on_offmesh(int agent) const;
  // Reached the target (within its radius) or no target.
  bool arrived(int agent, float tolerance = 0.3f) const;
  // Corners the agent is steering toward (debug).
  std::vector<V3> corners(int agent) const;
  int count() const;
  float min_clearance() const;  // smallest gap between active agents

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ai
}  // namespace ts
