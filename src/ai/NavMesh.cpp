#include "ai/NavMesh.h"

#include "DetourCommon.h"
#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace ts {
namespace ai {

namespace {

constexpr int kMaxPath = 512;
constexpr unsigned short kWalkFlag = 0x01;
constexpr unsigned short kLinkFlag = 0x02;

// Detour's random query takes a plain function: a thread-local stream.
thread_local std::uint64_t t_rng = 1;
float frand() {
  t_rng ^= t_rng << 13;
  t_rng ^= t_rng >> 7;
  t_rng ^= t_rng << 17;
  return static_cast<float>(t_rng >> 40) / static_cast<float>(1ull << 24);
}

V3 vec(const float* p) { return {p[0], p[1], p[2]}; }

}  // namespace

// --- Geometry -------------------------------------------------------------------------

void NavGeometry::add_tri(const V3& a, const V3& b, const V3& c) {
  const int base = static_cast<int>(verts.size() / 3);
  for (const V3& p : {a, b, c}) {
    verts.push_back(p.x);
    verts.push_back(p.y);
    verts.push_back(p.z);
  }
  // Recast wants counter-clockwise seen from above for up-facing (it uses
  // the normal for slope): keep the caller's order.
  tris.push_back(base);
  tris.push_back(base + 1);
  tris.push_back(base + 2);
}

void NavGeometry::add_box(const V3& c, const V3& h, float yaw) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  auto P = [&](float x, float y, float z) {
    return V3{c.x + x * cy + z * sy, c.y + y, c.z - x * sy + z * cy};
  };
  const V3 v[8] = {P(-h.x, -h.y, -h.z), P(h.x, -h.y, -h.z), P(h.x, -h.y, h.z), P(-h.x, -h.y, h.z),
                   P(-h.x, h.y, -h.z),  P(h.x, h.y, -h.z),  P(h.x, h.y, h.z),  P(-h.x, h.y, h.z)};
  // Faces with normals pointing out (y up = counter-clockwise seen from +y
  // in Recast's convention: a, c, b order for the top).
  const int f[6][4] = {{4, 7, 6, 5}, {0, 1, 2, 3}, {0, 4, 5, 1}, {2, 6, 7, 3}, {1, 5, 6, 2}, {3, 7, 4, 0}};
  for (const auto& q : f) {
    add_tri(v[q[0]], v[q[1]], v[q[2]]);
    add_tri(v[q[0]], v[q[2]], v[q[3]]);
  }
}

void NavGeometry::add_plane(const V3& c, float hx, float hz, float yaw) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  auto P = [&](float x, float z) { return V3{c.x + x * cy + z * sy, c.y, c.z - x * sy + z * cy}; };
  const V3 a = P(-hx, -hz), b = P(hx, -hz), d = P(-hx, hz), e = P(hx, hz);
  add_tri(a, d, e);
  add_tri(a, e, b);
}

float NavPath::length() const {
  float len = 0.0f;
  for (std::size_t i = 1; i < points.size(); ++i) len += distance(points[i - 1], points[i]);
  return len;
}

// --- Mesh -------------------------------------------------------------------------------

struct NavMesh::Impl {
  dtNavMesh* mesh = nullptr;
  dtNavMeshQuery* query = nullptr;
  dtQueryFilter filter;
  rcPolyMeshDetail* detail = nullptr;
  ~Impl() { reset(); }
  void reset() {
    if (query) dtFreeNavMeshQuery(query);
    if (mesh) dtFreeNavMesh(mesh);
    if (detail) rcFreePolyMeshDetail(detail);
    query = nullptr;
    mesh = nullptr;
    detail = nullptr;
  }
};

NavMesh::NavMesh() : impl_(std::make_unique<Impl>()) {
  impl_->filter.setIncludeFlags(0xffff);
  impl_->filter.setExcludeFlags(0);
}
NavMesh::~NavMesh() = default;

bool NavMesh::ready() const { return impl_->query != nullptr; }

void NavMesh::clear() {
  impl_->reset();
  stats_ = {};
  links_.clear();
}

bool NavMesh::build(const NavGeometry& geo, const NavBuildConfig& c, const std::vector<OffMeshLink>& links,
                    std::string* error) {
  const auto t0 = std::chrono::steady_clock::now();
  clear();
  cfg_ = c;
  links_ = links;
  std::string scratch;
  std::string& err = error ? *error : scratch;
  if (geo.tris.empty() || geo.verts.size() < 9) {
    err = "no static geometry to build a navmesh from";
    return false;
  }
  rcContext ctx(false);
  rcConfig cfg{};
  cfg.cs = c.cell_size;
  cfg.ch = c.cell_height;
  cfg.walkableSlopeAngle = c.max_slope;
  cfg.walkableHeight = static_cast<int>(std::ceil(c.agent_height / cfg.ch));
  cfg.walkableClimb = static_cast<int>(std::floor(c.max_climb / cfg.ch));
  cfg.walkableRadius = static_cast<int>(std::ceil(c.agent_radius / cfg.cs));
  cfg.maxEdgeLen = static_cast<int>(c.edge_max_len / cfg.cs);
  cfg.maxSimplificationError = c.edge_max_error;
  cfg.minRegionArea = static_cast<int>(c.region_min * c.region_min);
  cfg.mergeRegionArea = static_cast<int>(c.region_merge * c.region_merge);
  cfg.maxVertsPerPoly = 6;
  cfg.detailSampleDist = c.detail_sample_dist < 0.9f ? 0 : cfg.cs * c.detail_sample_dist;
  cfg.detailSampleMaxError = cfg.ch * c.detail_max_error;
  const int nverts = static_cast<int>(geo.verts.size() / 3);
  const int ntris = static_cast<int>(geo.tris.size() / 3);
  rcCalcBounds(geo.verts.data(), nverts, cfg.bmin, cfg.bmax);
  // Room above the highest floor for the agent.
  cfg.bmax[1] += c.agent_height;
  rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
  if (static_cast<long>(cfg.width) * cfg.height > 4096L * 4096L) {
    err = "navmesh area too large for one tile (" + std::to_string(cfg.width) + " x " + std::to_string(cfg.height) +
          " cells): raise the cell size";
    return false;
  }

  struct Guard {
    rcHeightfield* hf = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* cset = nullptr;
    rcPolyMesh* pmesh = nullptr;
    rcPolyMeshDetail* dmesh = nullptr;
    unsigned char* nav = nullptr;
    ~Guard() {
      rcFreeHeightField(hf);
      rcFreeCompactHeightfield(chf);
      rcFreeContourSet(cset);
      rcFreePolyMesh(pmesh);
      rcFreePolyMeshDetail(dmesh);
      if (nav) dtFree(nav);
    }
  } g;

  g.hf = rcAllocHeightfield();
  if (!g.hf || !rcCreateHeightfield(&ctx, *g.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
    err = "navmesh: heightfield";
    return false;
  }
  std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
  rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, geo.verts.data(), nverts, geo.tris.data(), ntris,
                          areas.data());
  if (!rcRasterizeTriangles(&ctx, geo.verts.data(), nverts, geo.tris.data(), areas.data(), ntris, *g.hf,
                            cfg.walkableClimb)) {
    err = "navmesh: rasterize";
    return false;
  }
  rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *g.hf);
  rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *g.hf);
  rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *g.hf);
  g.chf = rcAllocCompactHeightfield();
  if (!g.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *g.hf, *g.chf)) {
    err = "navmesh: compact heightfield";
    return false;
  }
  if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *g.chf)) {
    err = "navmesh: erode";
    return false;
  }
  if (!rcBuildDistanceField(&ctx, *g.chf) ||
      !rcBuildRegions(&ctx, *g.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
    err = "navmesh: regions";
    return false;
  }
  g.cset = rcAllocContourSet();
  if (!g.cset || !rcBuildContours(&ctx, *g.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *g.cset)) {
    err = "navmesh: contours";
    return false;
  }
  g.pmesh = rcAllocPolyMesh();
  if (!g.pmesh || !rcBuildPolyMesh(&ctx, *g.cset, cfg.maxVertsPerPoly, *g.pmesh)) {
    err = "navmesh: polygons";
    return false;
  }
  g.dmesh = rcAllocPolyMeshDetail();
  if (!g.dmesh ||
      !rcBuildPolyMeshDetail(&ctx, *g.pmesh, *g.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *g.dmesh)) {
    err = "navmesh: detail mesh";
    return false;
  }
  if (g.pmesh->npolys == 0) {
    err = "navmesh: nothing walkable (is there a floor wider than the agent?)";
    return false;
  }
  for (int i = 0; i < g.pmesh->npolys; ++i) {
    if (g.pmesh->areas[i] == RC_WALKABLE_AREA) g.pmesh->flags[i] = kWalkFlag;
  }

  std::vector<float> lverts, lrad;
  std::vector<unsigned short> lflags;
  std::vector<unsigned char> lareas, ldir;
  std::vector<unsigned int> lids;
  for (const OffMeshLink& l : links) {
    for (const V3& p : {l.a, l.b}) {
      lverts.push_back(p.x);
      lverts.push_back(p.y);
      lverts.push_back(p.z);
    }
    lrad.push_back(l.radius);
    lflags.push_back(kLinkFlag);
    lareas.push_back(RC_WALKABLE_AREA);
    ldir.push_back(l.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
    lids.push_back(l.user_id);
  }

  dtNavMeshCreateParams p{};
  p.verts = g.pmesh->verts;
  p.vertCount = g.pmesh->nverts;
  p.polys = g.pmesh->polys;
  p.polyAreas = g.pmesh->areas;
  p.polyFlags = g.pmesh->flags;
  p.polyCount = g.pmesh->npolys;
  p.nvp = g.pmesh->nvp;
  p.detailMeshes = g.dmesh->meshes;
  p.detailVerts = g.dmesh->verts;
  p.detailVertsCount = g.dmesh->nverts;
  p.detailTris = g.dmesh->tris;
  p.detailTriCount = g.dmesh->ntris;
  p.offMeshConVerts = lverts.empty() ? nullptr : lverts.data();
  p.offMeshConRad = lrad.empty() ? nullptr : lrad.data();
  p.offMeshConFlags = lflags.empty() ? nullptr : lflags.data();
  p.offMeshConAreas = lareas.empty() ? nullptr : lareas.data();
  p.offMeshConDir = ldir.empty() ? nullptr : ldir.data();
  p.offMeshConUserID = lids.empty() ? nullptr : lids.data();
  p.offMeshConCount = static_cast<int>(links.size());
  p.walkableHeight = c.agent_height;
  p.walkableRadius = c.agent_radius;
  p.walkableClimb = c.max_climb;
  rcVcopy(p.bmin, g.pmesh->bmin);
  rcVcopy(p.bmax, g.pmesh->bmax);
  p.cs = cfg.cs;
  p.ch = cfg.ch;
  p.buildBvTree = true;
  int nav_size = 0;
  if (!dtCreateNavMeshData(&p, &g.nav, &nav_size)) {
    err = "navmesh: Detour data (too many vertices?)";
    return false;
  }
  impl_->mesh = dtAllocNavMesh();
  if (!impl_->mesh || dtStatusFailed(impl_->mesh->init(g.nav, nav_size, DT_TILE_FREE_DATA))) {
    impl_->reset();
    err = "navmesh: Detour init";
    return false;
  }
  g.nav = nullptr;  // owned by the mesh now
  impl_->query = dtAllocNavMeshQuery();
  if (!impl_->query || dtStatusFailed(impl_->query->init(impl_->mesh, 4096))) {
    impl_->reset();
    err = "navmesh: query init";
    return false;
  }
  impl_->detail = g.dmesh;
  g.dmesh = nullptr;
  stats_.polys = g.pmesh->npolys;
  stats_.verts = g.pmesh->nverts;
  stats_.detail_tris = impl_->detail->ntris;
  stats_.offmesh = static_cast<int>(links.size());
  stats_.build_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

bool NavMesh::nearest(const V3& p, V3* out, const V3& ext) const {
  if (!ready()) return false;
  const float c[3] = {p.x, p.y, p.z}, e[3] = {ext.x, ext.y, ext.z};
  dtPolyRef ref = 0;
  float pt[3];
  if (dtStatusFailed(impl_->query->findNearestPoly(c, e, &impl_->filter, &ref, pt)) || !ref) return false;
  if (out) *out = vec(pt);
  return true;
}

NavPath NavMesh::find_path(const V3& from, const V3& to) const {
  NavPath out;
  if (!ready()) return out;
  const float ext[3] = {2.0f, 4.0f, 2.0f};
  const float s[3] = {from.x, from.y, from.z}, e[3] = {to.x, to.y, to.z};
  dtPolyRef sref = 0, eref = 0;
  float sp[3], ep[3];
  impl_->query->findNearestPoly(s, ext, &impl_->filter, &sref, sp);
  impl_->query->findNearestPoly(e, ext, &impl_->filter, &eref, ep);
  if (!sref || !eref) return out;
  dtPolyRef polys[kMaxPath];
  int npolys = 0;
  const dtStatus st = impl_->query->findPath(sref, eref, sp, ep, &impl_->filter, polys, &npolys, kMaxPath);
  if (dtStatusFailed(st) || npolys == 0) return out;
  float end[3];
  dtVcopy(end, ep);
  if (polys[npolys - 1] != eref) {
    out.partial = true;
    bool over = false;
    impl_->query->closestPointOnPoly(polys[npolys - 1], ep, end, &over);
  }
  float straight[kMaxPath * 3];
  unsigned char flags[kMaxPath];
  dtPolyRef refs[kMaxPath];
  int n = 0;
  impl_->query->findStraightPath(sp, end, polys, npolys, straight, flags, refs, &n, kMaxPath, 0);
  for (int i = 0; i < n; ++i) {
    out.points.push_back(vec(&straight[i * 3]));
    out.offmesh.push_back((flags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) ? 1 : 0);
  }
  out.found = n > 0;
  return out;
}

bool NavMesh::raycast(const V3& from, const V3& to, V3* hit) const {
  if (!ready()) return false;
  const float ext[3] = {1.0f, 2.0f, 1.0f};
  const float s[3] = {from.x, from.y, from.z}, e[3] = {to.x, to.y, to.z};
  dtPolyRef sref = 0;
  float sp[3];
  impl_->query->findNearestPoly(s, ext, &impl_->filter, &sref, sp);
  if (!sref) return false;
  float t = 0.0f, nrm[3];
  dtPolyRef path[64];
  int np = 0;
  impl_->query->raycast(sref, sp, e, &impl_->filter, &t, nrm, path, &np, 64);
  if (t >= 1.0f) {
    if (hit) *hit = to;
    return true;
  }
  if (hit) *hit = from + (to - from) * t;
  return false;
}

bool NavMesh::random_point(const V3& center, float radius, std::uint64_t* seed, V3* out) const {
  if (!ready()) return false;
  const float ext[3] = {2.0f, 4.0f, 2.0f};
  const float c[3] = {center.x, center.y, center.z};
  dtPolyRef ref = 0;
  float cp[3];
  impl_->query->findNearestPoly(c, ext, &impl_->filter, &ref, cp);
  if (!ref) return false;
  t_rng = (seed && *seed) ? *seed : 0x2545F4914F6CDD1Dull;
  // Detour samples whole polygons that touch the circle, so large polys can yield
  // points well outside it: resample a few times, then fall back to the centre.
  bool ok = false;
  for (int attempt = 0; attempt < 24 && !ok; ++attempt) {
    dtPolyRef rref = 0;
    float pt[3];
    const dtStatus st = impl_->query->findRandomPointAroundCircle(ref, cp, radius, &impl_->filter, frand, &rref, pt);
    if (dtStatusFailed(st) || !rref) break;
    const float dx = pt[0] - cp[0], dz = pt[2] - cp[2];
    if (dx * dx + dz * dz <= radius * radius) { *out = vec(pt); ok = true; }
  }
  if (seed) *seed = t_rng;
  if (!ok) *out = vec(cp);
  return true;
}

void NavMesh::debug_triangles(std::vector<float>* tris) const {
  tris->clear();
  const rcPolyMeshDetail* d = impl_->detail;
  if (!d) return;
  for (int m = 0; m < d->nmeshes; ++m) {
    const unsigned int* sub = &d->meshes[m * 4];
    const unsigned int bverts = sub[0], btris = sub[2], ntris = sub[3];
    for (unsigned int k = 0; k < ntris; ++k) {
      const unsigned char* t = &d->tris[(btris + k) * 4];
      for (int j = 0; j < 3; ++j) {
        const float* v = &d->verts[(bverts + t[j]) * 3];
        tris->push_back(v[0]);
        tris->push_back(v[1]);
        tris->push_back(v[2]);
      }
    }
  }
}

void NavMesh::debug_edges(std::vector<V3>* segs) const {
  segs->clear();
  if (!impl_->mesh) return;
  const dtNavMesh* m = impl_->mesh;
  for (int ti = 0; ti < m->getMaxTiles(); ++ti) {
    const dtMeshTile* tile = m->getTile(ti);
    if (!tile || !tile->header) continue;
    for (int i = 0; i < tile->header->polyCount; ++i) {
      const dtPoly& p = tile->polys[i];
      if (p.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
      for (int j = 0; j < p.vertCount; ++j) {
        const float* a = &tile->verts[p.verts[j] * 3];
        const float* b = &tile->verts[p.verts[(j + 1) % p.vertCount] * 3];
        segs->push_back(vec(a) + V3{0, 0.05f, 0});
        segs->push_back(vec(b) + V3{0, 0.05f, 0});
      }
    }
  }
}

// --- Crowd ----------------------------------------------------------------------------

struct NavCrowd::Impl {
  NavMesh* mesh = nullptr;
  dtCrowd* crowd = nullptr;
  std::vector<V3> targets;
  std::vector<bool> has_target;
  ~Impl() {
    if (crowd) dtFreeCrowd(crowd);
  }
};

namespace {
dtCrowdAgentParams to_dt(const CrowdAgentParams& p) {
  dtCrowdAgentParams a{};
  a.radius = p.radius;
  a.height = p.height;
  a.maxAcceleration = p.max_accel;
  a.maxSpeed = p.max_speed;
  a.collisionQueryRange = p.radius * 12.0f;
  a.pathOptimizationRange = p.radius * 30.0f;
  a.separationWeight = p.separation;
  a.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
  if (p.avoidance) a.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
  a.obstacleAvoidanceType = static_cast<unsigned char>(std::clamp(p.avoidance_quality, 0, 3));
  a.queryFilterType = 0;
  return a;
}
}  // namespace

NavCrowd::NavCrowd(NavMesh& mesh, int max_agents, float max_radius) : impl_(std::make_unique<Impl>()) {
  impl_->mesh = &mesh;
  impl_->crowd = dtAllocCrowd();
  if (!mesh.ready() || !impl_->crowd->init(max_agents, max_radius, mesh.impl()->mesh)) {
    dtFreeCrowd(impl_->crowd);
    impl_->crowd = nullptr;
    return;
  }
  // Avoidance presets (low .. high), like the Recast demo.
  dtObstacleAvoidanceParams params;
  std::memcpy(&params, impl_->crowd->getObstacleAvoidanceParams(0), sizeof(dtObstacleAvoidanceParams));
  params.velBias = 0.5f;
  params.adaptiveDivs = 5;
  params.adaptiveRings = 2;
  params.adaptiveDepth = 1;
  impl_->crowd->setObstacleAvoidanceParams(0, &params);
  params.adaptiveRings = 2;
  params.adaptiveDepth = 2;
  impl_->crowd->setObstacleAvoidanceParams(1, &params);
  params.adaptiveDivs = 7;
  params.adaptiveRings = 2;
  params.adaptiveDepth = 3;
  impl_->crowd->setObstacleAvoidanceParams(2, &params);
  params.adaptiveDivs = 7;
  params.adaptiveRings = 3;
  params.adaptiveDepth = 3;
  impl_->crowd->setObstacleAvoidanceParams(3, &params);
  impl_->targets.assign(static_cast<std::size_t>(max_agents), {});
  impl_->has_target.assign(static_cast<std::size_t>(max_agents), false);
}

NavCrowd::~NavCrowd() = default;

int NavCrowd::add(const V3& pos, const CrowdAgentParams& params) {
  if (!impl_->crowd) return -1;
  V3 on;
  if (!impl_->mesh->nearest(pos, &on)) return -1;
  const float p[3] = {on.x, on.y, on.z};
  const dtCrowdAgentParams a = to_dt(params);
  return impl_->crowd->addAgent(p, &a);
}

void NavCrowd::remove(int agent) {
  if (impl_->crowd && agent >= 0) impl_->crowd->removeAgent(agent);
}

bool NavCrowd::set_target(int agent, const V3& target) {
  if (!active(agent)) return false;
  const float ext[3] = {2.0f, 4.0f, 2.0f};
  const float t[3] = {target.x, target.y, target.z};
  dtPolyRef ref = 0;
  float pt[3];
  const dtNavMeshQuery* q = impl_->crowd->getNavMeshQuery();
  q->findNearestPoly(t, ext, impl_->crowd->getFilter(0), &ref, pt);
  if (!ref) return false;
  impl_->targets[static_cast<std::size_t>(agent)] = vec(pt);
  impl_->has_target[static_cast<std::size_t>(agent)] = true;
  return impl_->crowd->requestMoveTarget(agent, ref, pt);
}

void NavCrowd::stop(int agent) {
  if (!active(agent)) return;
  impl_->crowd->resetMoveTarget(agent);
  impl_->has_target[static_cast<std::size_t>(agent)] = false;
}

void NavCrowd::set_params(int agent, const CrowdAgentParams& params) {
  if (!active(agent)) return;
  const dtCrowdAgentParams a = to_dt(params);
  impl_->crowd->updateAgentParameters(agent, &a);
}

void NavCrowd::update(float dt) {
  if (impl_->crowd) impl_->crowd->update(dt, nullptr);
}

bool NavCrowd::warp(int agent, const V3& pos) {
  if (!active(agent)) return false;
  const dtCrowdAgentParams params = impl_->crowd->getAgent(agent)->params;
  const bool had = impl_->has_target[static_cast<std::size_t>(agent)];
  const V3 tgt = impl_->targets[static_cast<std::size_t>(agent)];
  impl_->crowd->removeAgent(agent);
  V3 on;
  if (!impl_->mesh->nearest(pos, &on)) return false;
  const float p[3] = {on.x, on.y, on.z};
  const int again = impl_->crowd->addAgent(p, &params);
  if (again != agent) return false;
  if (had) set_target(agent, tgt);
  return true;
}

bool NavCrowd::active(int agent) const {
  if (!impl_->crowd || agent < 0 || agent >= impl_->crowd->getAgentCount()) return false;
  const dtCrowdAgent* a = impl_->crowd->getAgent(agent);
  return a && a->active;
}

V3 NavCrowd::position(int agent) const {
  return active(agent) ? vec(impl_->crowd->getAgent(agent)->npos) : V3{};
}

V3 NavCrowd::velocity(int agent) const {
  return active(agent) ? vec(impl_->crowd->getAgent(agent)->vel) : V3{};
}

bool NavCrowd::on_offmesh(int agent) const {
  return active(agent) && impl_->crowd->getAgent(agent)->state == DT_CROWDAGENT_STATE_OFFMESH;
}

bool NavCrowd::arrived(int agent, float tol) const {
  if (!active(agent)) return true;
  if (!impl_->has_target[static_cast<std::size_t>(agent)]) return true;
  const dtCrowdAgent* a = impl_->crowd->getAgent(agent);
  return distance_xz(vec(a->npos), impl_->targets[static_cast<std::size_t>(agent)]) <= a->params.radius + tol &&
         a->state != DT_CROWDAGENT_STATE_OFFMESH;
}

std::vector<V3> NavCrowd::corners(int agent) const {
  std::vector<V3> out;
  if (!active(agent)) return out;
  const dtCrowdAgent* a = impl_->crowd->getAgent(agent);
  out.push_back(vec(a->npos));
  for (int i = 0; i < a->ncorners; ++i) out.push_back(vec(&a->cornerVerts[i * 3]));
  return out;
}

int NavCrowd::count() const {
  if (!impl_->crowd) return 0;
  int n = 0;
  for (int i = 0; i < impl_->crowd->getAgentCount(); ++i) n += active(i) ? 1 : 0;
  return n;
}

float NavCrowd::min_clearance() const {
  float best = std::numeric_limits<float>::infinity();
  if (!impl_->crowd) return best;
  const int n = impl_->crowd->getAgentCount();
  for (int i = 0; i < n; ++i) {
    if (!active(i)) continue;
    for (int j = i + 1; j < n; ++j) {
      if (!active(j)) continue;
      const dtCrowdAgent* a = impl_->crowd->getAgent(i);
      const dtCrowdAgent* b = impl_->crowd->getAgent(j);
      best = std::min(best, distance_xz(vec(a->npos), vec(b->npos)) - a->params.radius - b->params.radius);
    }
  }
  return best;
}

}  // namespace ai
}  // namespace ts
