// Model bounds regressions (see SmokeBounds.h). Generated fixtures only.
#include "SmokeBounds.h"

#include "render3d/ColliderFit.h"
#include "render3d/ModelImport.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace r3 = ts::tombstone::render3d;
using r3::Vec3;

namespace {

const char* W = "bounds";

int fail(const std::string& what) {
  std::cerr << "[smoke] " << W << " FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near3(const Vec3& a, const Vec3& b, float eps = 1e-3f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
std::string str(const Vec3& v) {
  return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
}
std::string str(const r3::Aabb& b) { return str(b.min) + ".." + str(b.max); }

void spit(const fs::path& p, const std::string& s) {
  std::ofstream o(p, std::ios::binary);
  o << s;
}

// Rest-posed extents computed independently (CPU reference skinning
// through each primitive's palette), over the vertices triangles use.
r3::Aabb drawn_extents(const r3::Model& m) {
  std::vector<r3::Mat4> full, pal;
  if (m.skinned()) r3::skin_palette(m.skeleton, r3::rest_pose(m.skeleton), &full);
  r3::Aabb b{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
  for (const r3::ModelPrimitive& p : m.primitives) {
    r3::primitive_palette(p, full, &pal);
    for (std::uint32_t i : p.mesh.indices) {
      Vec3 q = p.mesh.vertices[i].position;
      if (p.mesh.skinned()) q = r3::skin_point(pal, p.mesh.skin[i].joints, p.mesh.skin[i].weights, q);
      b.min = r3::vmin(b.min, q);
      b.max = r3::vmax(b.max, q);
    }
  }
  return b;
}

// Axis-aligned box mesh (12 triangles), optionally skinned to one joint.
void add_box(r3::MeshData* m, const Vec3& lo, const Vec3& hi, int joint) {
  const std::uint32_t base = static_cast<std::uint32_t>(m->vertices.size());
  for (int i = 0; i < 8; ++i) {
    r3::Vertex3D v;
    v.position = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
    v.normal = {0, 1, 0};
    m->vertices.push_back(v);
    if (joint >= 0) {
      r3::SkinVertex s;
      s.joints[0] = static_cast<float>(joint);
      m->skin.push_back(s);
    }
  }
  const int f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                        {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  for (const auto& t : f) {
    for (int c = 0; c < 3; ++c) m->indices.push_back(base + static_cast<std::uint32_t>(t[c]));
  }
}

// 1. In memory: an FBX-style character rig. Vertices are in centimetres
// (a 100 x 180 x 30 "T-pose" block), the armature root is scaled 0.01
// and the inverse binds are identity, so the drawn model is 1 x 1.8 x 0.3 m.
// A helper bone sits 500 m away with no weights; a stray vertex no
// triangle uses sits at 10 km.
int check_scaled_rig() {
  r3::Model m;
  m.format = "test";
  r3::Joint root;
  root.name = "Armature";
  root.s = {0.01f, 0.01f, 0.01f};
  r3::Joint hips;
  hips.name = "hips";
  hips.parent = 0;
  r3::Joint ik;
  ik.name = "ik_target";
  ik.parent = 0;
  ik.t = {50000.0f, 0, 0};  // 500 m in armature space
  m.skeleton.joints = {root, hips, ik};
  // Inverse binds: identity for the root / hips (the rig's "bind" is the
  // scaled armature), so palette = 0.01 x.
  r3::ModelPrimitive p;
  p.mesh.name = "body";
  add_box(&p.mesh, {-50, 0, -15}, {50, 180, 15}, 1);
  // Torso narrower than the arms: a second block 40 wide, to give the
  // capsule something to fit.
  for (int k = 0; k < 5; ++k) add_box(&p.mesh, {-20, 0, -15}, {20, 170, 15}, 1);
  r3::Vertex3D stray;
  stray.position = {1e6f, 1e6f, 1e6f};
  p.mesh.vertices.push_back(stray);
  p.mesh.skin.push_back(r3::SkinVertex{});
  m.primitives.push_back(p);
  r3::update_bounds(&m);
  const r3::Aabb want{{-0.5f, 0, -0.15f}, {0.5f, 1.8f, 0.15f}};
  if (!near3(m.bounds.min, want.min) || !near3(m.bounds.max, want.max)) {
    return fail("scaled rig bounds " + str(m.bounds) + ", want " + str(want));
  }
  // The old (raw vertex) bounds were 100x too big and reached the stray vertex.
  const r3::Aabb legacy = r3::legacy_vertex_bounds(m);
  if (legacy.max.x < 1000.0f) return fail("legacy bounds should show the old bug: " + str(legacy));
  // Physics sees the same shape.
  r3::MeshData rest;
  r3::rest_pose_mesh(m, &rest);
  r3::Aabb rb{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
  for (std::uint32_t i : rest.indices) {
    rb.min = r3::vmin(rb.min, rest.vertices[i].position);
    rb.max = r3::vmax(rb.max, rest.vertices[i].position);
  }
  if (!near3(rb.min, want.min) || !near3(rb.max, want.max)) return fail("rest_pose_mesh " + str(rb));
  // Unit scale + Z up baked afterwards: still tight.
  r3::Model z = m;
  r3::bake_import_transform(&z, 2.0f, false);
  if (!near3(z.bounds.max, want.max * 2.0f) || !near3(z.bounds.min, want.min * 2.0f)) {
    return fail("scaled rig x2 bounds " + str(z.bounds));
  }
  // Fitted colliders: auto = capsule for a skinned model, upright, the full
  // height, radius well under half the T-pose width.
  ts::tombstone::scene3d::Collider3D c;
  if (!r3::fit_collider(m, r3::ColliderFit::Auto, &c)) return fail("fit auto");
  if (c.shape != ts::tombstone::scene3d::ColliderShape3D::Capsule || !near(c.size.y, 1.8f) ||
      !(c.size.x < 0.45f && c.size.x >= 0.15f) || !near3(c.offset, {0, 0.9f, 0}) || c.fit != "auto") {
    return fail("capsule fit: r " + std::to_string(c.size.x) + " h " + std::to_string(c.size.y) +
                " at " + str(c.offset));
  }
  if (!r3::fit_collider(m, r3::ColliderFit::Box, &c) ||
      c.shape != ts::tombstone::scene3d::ColliderShape3D::Box || !near3(c.size, want.extent()) ||
      !near3(c.offset, want.center())) {
    return fail("box fit " + str(c.size) + " at " + str(c.offset));
  }
  if (r3::fit_collider(m, r3::ColliderFit::None, &c)) return fail("fit none made a collider");
  return 0;
}

// 2. The same rig as Collada through Assimp: armature node scaled 0.01,
// identity inverse binds, a 100 x 100 x 200 (Z up) bar.
const char* kScaledDae = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="meter" meter="1"/><up_axis>Z_UP</up_axis></asset>
  <library_geometries>
    <geometry id="bar-mesh" name="bar">
      <mesh>
        <source id="bar-pos">
          <float_array id="bar-pos-array" count="24">0 0 0 100 0 0 100 100 0 0 100 0 0 0 200 100 0 200 100 100 200 0 100 200</float_array>
          <technique_common><accessor source="#bar-pos-array" count="8" stride="3"><param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common>
        </source>
        <vertices id="bar-verts"><input semantic="POSITION" source="#bar-pos"/></vertices>
        <triangles count="12"><input semantic="VERTEX" source="#bar-verts" offset="0"/>
          <p>0 2 1 0 3 2 4 5 6 4 6 7 0 1 5 0 5 4 1 2 6 1 6 5 2 3 7 2 7 6 3 0 4 3 4 7</p></triangles>
      </mesh>
    </geometry>
  </library_geometries>
  <library_controllers>
    <controller id="bar-skin"><skin source="#bar-mesh">
      <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>
      <source id="bar-joints"><Name_array id="bar-joints-array" count="1">root</Name_array>
        <technique_common><accessor source="#bar-joints-array" count="1" stride="1"><param name="JOINT" type="name"/></accessor></technique_common></source>
      <source id="bar-binds"><float_array id="bar-binds-array" count="16">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</float_array>
        <technique_common><accessor source="#bar-binds-array" count="1" stride="16"><param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
      <source id="bar-weights"><float_array id="bar-weights-array" count="1">1</float_array>
        <technique_common><accessor source="#bar-weights-array" count="1" stride="1"><param name="WEIGHT" type="float"/></accessor></technique_common></source>
      <joints><input semantic="JOINT" source="#bar-joints"/><input semantic="INV_BIND_MATRIX" source="#bar-binds"/></joints>
      <vertex_weights count="8"><input semantic="JOINT" source="#bar-joints" offset="0"/><input semantic="WEIGHT" source="#bar-weights" offset="1"/>
        <vcount>1 1 1 1 1 1 1 1</vcount><v>0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0</v></vertex_weights>
    </skin></controller>
  </library_controllers>
  <library_visual_scenes>
    <visual_scene id="Scene" name="Scene">
      <node id="Armature" name="Armature" type="NODE"><matrix sid="transform">0.01 0 0 0 0 0.01 0 0 0 0 0.01 0 0 0 0 1</matrix>
        <node id="root" name="root" sid="root" type="JOINT"><matrix sid="transform">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix></node>
        <node id="ik_target" name="ik_target" type="NODE"><matrix sid="transform">1 0 0 50000 0 1 0 0 0 0 1 0 0 0 0 1</matrix></node>
      </node>
      <node id="bar" name="bar" type="NODE"><instance_controller url="#bar-skin"><skeleton>#root</skeleton></instance_controller></node>
    </visual_scene>
  </library_visual_scenes>
  <scene><instance_visual_scene url="#Scene"/></scene>
</COLLADA>
)";

int check_dae(const fs::path& dir) {
  if (!r3::assimp_available()) {
    std::cout << "[smoke] bounds: Assimp off, Collada rig skipped\n";
    return 0;
  }
  const fs::path p = dir / "scaled_rig.dae";
  spit(p, kScaledDae);
  for (float unit : {1.0f, 0.5f}) {
    r3::ModelImportOptions opt;
    opt.unit_scale = unit;
    r3::Model m;
    std::string err;
    if (!r3::load_model(p.string(), opt, &m, &err)) return fail("dae load: " + err);
    if (!m.skinned()) return fail("dae: expected a skinned model");
    const r3::Aabb drawn = drawn_extents(m);
    if (!near3(m.bounds.min, drawn.min) || !near3(m.bounds.max, drawn.max)) {
      return fail("dae bounds " + str(m.bounds) + " != drawn " + str(drawn));
    }
    // Drawn: 1 x 2 x 1 m (Z up -> Y up) times the unit scale.
    const Vec3 ext = m.bounds.extent();
    if (!near(ext.x, 1.0f * unit, 2e-3f) || !near(ext.y, 2.0f * unit, 2e-3f) || !near(ext.z, 1.0f * unit, 2e-3f)) {
      return fail("dae extents " + str(ext) + " at unit scale " + std::to_string(unit));
    }
  }
  return 0;
}

// 2b. Blender FBX: a mesh parented to a bone is written relative to the
// armature plus the bone length (exporter quirk). A hand-written ASCII FBX
// with a skinned body (bone at y 2, connected end bone at +1) and a hat whose
// true place is y 3 (written as Lcl y 4 under the bone). Read literally the
// hat lands at y 6; a file from another tool keeps that literal reading.
std::string blender_fbx(const std::string& creator) {
  auto cube = [](float x0, float y0, float z0, float x1, float y1, float z1) {
    std::ostringstream v;
    for (int i = 0; i < 8; ++i) {
      v << (i ? "," : "") << ((i & 1) ? x1 : x0) << "," << ((i & 2) ? y1 : y0) << "," << ((i & 4) ? z1 : z0);
    }
    return v.str();
  };
  const char* quads = "0,2,3,-2,4,5,7,-7,0,1,5,-5,2,6,7,-4,0,4,6,-3,1,3,7,-6";
  auto model = [](int id, const char* name, const char* type, float ty) {
    std::ostringstream m;
    m << "\tModel: " << id << ", \"Model::" << name << "\", \"" << type << "\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n"
      << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0," << ty << ",0\n\t\t}\n\t}\n";
    return m.str();
  };
  auto geometry = [&](int id, const char* name, const std::string& verts) {
    return "\tGeometry: " + std::to_string(id) + ", \"Geometry::" + name + "\", \"Mesh\" {\n\t\tVertices: *24 {\n\t\t\ta: " +
           verts + "\n\t\t}\n\t\tPolygonVertexIndex: *24 {\n\t\t\ta: " + quads + "\n\t\t}\n\t\tGeometryVersion: 124\n\t}\n";
  };
  std::ostringstream f;
  f << "; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \""
    << creator << "\"\n}\nCreator: \"" << creator << "\"\nGlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
    << "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
    << "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
    << "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
    << "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n\t}\n}\nObjects:  {\n"
    << model(1, "Armature", "Null", 0) << "\tNodeAttribute: 10, \"NodeAttribute::bone\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n"
    << "\tNodeAttribute: 11, \"NodeAttribute::bone_end\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n"
    << model(2, "bone", "LimbNode", 2) << model(3, "bone_end", "LimbNode", 1) << model(4, "Body", "Mesh", 0)
    << geometry(40, "Body", cube(-0.5f, 0, -0.5f, 0.5f, 4, 0.5f)) << model(5, "Hat", "Mesh", 4)
    << geometry(50, "Hat", cube(-0.1f, -0.1f, -0.1f, 0.1f, 0.1f, 0.1f))
    << "\tDeformer: 60, \"Deformer::Skin\", \"Skin\" {\n\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n"
    << "\tDeformer: 61, \"SubDeformer::bone\", \"Cluster\" {\n\t\tVersion: 100\n\t\tIndexes: *8 {\n\t\t\ta: 0,1,2,3,4,5,6,7\n\t\t}\n"
    << "\t\tWeights: *8 {\n\t\t\ta: 1,1,1,1,1,1,1,1\n\t\t}\n\t\tTransform: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,-2,0,1\n\t\t}\n"
    << "\t\tTransformLink: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,2,0,1\n\t\t}\n\t}\n}\nConnections:  {\n"
    << "\tC: \"OO\",1,0\n\tC: \"OO\",2,1\n\tC: \"OO\",10,2\n\tC: \"OO\",3,2\n\tC: \"OO\",11,3\n\tC: \"OO\",4,1\n\tC: \"OO\",40,4\n"
    << "\tC: \"OO\",5,2\n\tC: \"OO\",50,5\n\tC: \"OO\",60,40\n\tC: \"OO\",61,60\n\tC: \"OO\",2,61\n}\n";
  return f.str();
}

int check_blender_fbx(const fs::path& dir) {
  if (!r3::assimp_available()) return 0;
  for (int blender = 1; blender >= 0; --blender) {
    const fs::path p = dir / (blender ? "blender_bone_child.fbx" : "other_bone_child.fbx");
    spit(p, blender_fbx(blender ? "Blender (stable FBX IO) - 5.1.0 - 5.15.0" : "Some Exporter 1.0"));
    r3::ModelImportOptions opt;
    r3::Model m;
    std::string err;
    if (!r3::load_model(p.string(), opt, &m, &err)) return fail("bone-child fbx load: " + err);
    const r3::ModelPrimitive* hat = nullptr;
    for (const auto& q : m.primitives) {
      if (q.mesh.name.find("Hat") != std::string::npos || q.node.find("Hat") != std::string::npos) hat = &q;
    }
    if (!hat || !m.skinned()) return fail("bone-child fbx: hat / skin missing (" + std::to_string(m.primitives.size()) + " prims)");
    r3::Aabb hb{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (const auto& v : hat->mesh.vertices) {
      hb.min = r3::vmin(hb.min, v.position);
      hb.max = r3::vmax(hb.max, v.position);
    }
    const float want = blender ? 3.0f : 6.0f;
    if (!near(hb.center().y, want, 2e-3f)) {
      return fail(std::string(blender ? "Blender" : "non-Blender") + " fbx hat at y " + std::to_string(hb.center().y) +
                  ", want " + std::to_string(want) + " (" + err + ")");
    }
    if (blender && !near(m.bounds.max.y, 4.0f, 2e-3f)) return fail("Blender fbx bounds " + str(m.bounds));
  }
  return 0;
}

// 3. glTF (rigid): bounds = vertex extents x unit scale.
int check_gltf(const fs::path& dir) {
  r3::GltfPart part;
  add_box(&part.mesh, {-1, 0, -2}, {3, 5, 2}, -1);
  const fs::path p = dir / "block.gltf";
  std::string err;
  if (!r3::write_gltf_embedded(p.string(), {part}, &err)) return fail("write gltf: " + err);
  for (float unit : {1.0f, 0.25f}) {
    r3::ModelImportOptions opt;
    opt.unit_scale = unit;
    r3::Model m;
    if (!r3::load_model(p.string(), opt, &m, &err)) return fail("gltf load: " + err);
    if (!near3(m.bounds.min, Vec3{-1, 0, -2} * unit) || !near3(m.bounds.max, Vec3{3, 5, 2} * unit)) {
      return fail("gltf bounds " + str(m.bounds) + " at unit scale " + std::to_string(unit));
    }
  }
  return 0;
}

// 4. OBJ with a UCX_ collision mesh and a far-away LOD1: not drawn, not
// in the bounds; the hull becomes collision; lod 1 swaps the LOD.
int check_special_meshes(const fs::path& dir) {
  if (!r3::assimp_available()) return 0;
  auto cube = [](const Vec3& lo, const Vec3& hi, int base) {
    std::string s;
    for (int i = 0; i < 8; ++i) {
      s += "v " + std::to_string((i & 1) ? hi.x : lo.x) + " " + std::to_string((i & 2) ? hi.y : lo.y) + " " +
           std::to_string((i & 4) ? hi.z : lo.z) + "\n";
    }
    const int f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                          {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (const auto& t : f) {
      s += "f " + std::to_string(base + t[0]) + " " + std::to_string(base + t[1]) + " " + std::to_string(base + t[2]) + "\n";
    }
    return s;
  };
  std::string obj = "# generated\n";
  obj += "o crate_LOD0\n" + cube({0, 0, 0}, {1, 1, 1}, 1);
  obj += "o UCX_crate_00\n" + cube({-1, -1, -1}, {2, 2, 2}, 9);
  obj += "o crate_LOD1\n" + cube({50, 0, 0}, {51, 1, 1}, 17);
  const fs::path p = dir / "crate_special.obj";
  spit(p, obj);
  r3::ModelImportOptions opt;
  r3::Model m;
  std::string err;
  if (!r3::load_model(p.string(), opt, &m, &err)) return fail("obj load: " + err);
  if (m.primitives.size() != 1 || m.collision_hulls.size() != 1 || m.lods != 2) {
    return fail("obj special meshes: " + std::to_string(m.primitives.size()) + " prims, " +
                std::to_string(m.collision_hulls.size()) + " hulls, " + std::to_string(m.lods) + " lods");
  }
  if (!near3(m.bounds.min, {0, 0, 0}) || !near3(m.bounds.max, {1, 1, 1})) return fail("obj LOD0 bounds " + str(m.bounds));
  // Auto collider picks the UCX_ hull.
  ts::tombstone::scene3d::Collider3D c;
  if (!r3::fit_collider(m, r3::ColliderFit::Auto, &c) ||
      c.shape != ts::tombstone::scene3d::ColliderShape3D::ConvexHull) {
    return fail("auto collider with UCX_ hull should be convex");
  }
  opt.lod = 1;
  if (!r3::load_model(p.string(), opt, &m, &err)) return fail("obj lod1 load: " + err);
  if (!near3(m.bounds.min, {50, 0, 0}) || !near3(m.bounds.max, {51, 1, 1})) return fail("obj LOD1 bounds " + str(m.bounds));
  opt.lod = 0;
  opt.collision = false;
  if (!r3::load_model(p.string(), opt, &m, &err)) return fail("obj load (no collision): " + err);
  if (!m.collision_hulls.empty() || m.primitives.size() != 1) return fail("collision off: UCX_ still imported");
  return 0;
}

// 5. 200 joints (more than a draw's palette): per-primitive joint maps,
// posed exactly like the full skeleton.
int check_big_skeleton() {
  r3::Model m;
  m.format = "test";
  const int n = 200;
  for (int i = 0; i < n; ++i) {
    r3::Joint j;
    j.name = "b" + std::to_string(i);
    j.parent = i - 1;
    j.t = i == 0 ? Vec3{} : Vec3{0, 0.01f, 0};
    m.skeleton.joints.push_back(j);
  }
  // Inverse binds from a rest pose with every joint 0.01 higher... and a
  // rest pose that bends: rotate each joint a little so the palette is
  // not identity.
  std::vector<r3::Mat4> bind;
  r3::joint_matrices(m.skeleton, r3::rest_pose(m.skeleton), &bind);
  for (int i = 0; i < n; ++i) m.skeleton.joints[i].inverse_bind = bind[i].inverse();
  for (int i = 1; i < n; ++i) m.skeleton.joints[i].r = r3::Quat::axis_angle({0, 0, 1}, 0.004f);
  r3::ModelPrimitive p;
  p.mesh.name = "rope";
  for (int i = 0; i < n; ++i) add_box(&p.mesh, {-0.05f, i * 0.01f, -0.05f}, {0.05f, i * 0.01f + 0.01f, 0.05f}, i);
  m.primitives.push_back(p);
  r3::update_bounds(&m);
  const r3::Aabb before = m.bounds;
  const r3::Aabb drawn_before = drawn_extents(m);
  r3::limit_skin_joints(&m);
  if (m.primitives.size() < 2) return fail("200-joint primitive was not split");
  for (const r3::ModelPrimitive& q : m.primitives) {
    if (q.joint_map.empty() || q.joint_map.size() > static_cast<std::size_t>(r3::kMaxBones)) {
      return fail("joint map size " + std::to_string(q.joint_map.size()));
    }
    for (const r3::SkinVertex& s : q.mesh.skin) {
      if (s.joints[0] >= static_cast<float>(q.joint_map.size())) return fail("local joint out of range");
    }
  }
  r3::update_bounds(&m);
  const r3::Aabb drawn_after = drawn_extents(m);
  if (!near3(before.min, m.bounds.min) || !near3(before.max, m.bounds.max) ||
      !near3(drawn_before.max, drawn_after.max) || !near3(drawn_before.min, drawn_after.min)) {
    return fail("split skeleton poses differently: " + str(before) + " vs " + str(m.bounds));
  }
  // The tip really is bent (the joints past 128 count).
  if (!(m.bounds.min.x < -0.3f)) return fail("bent rope bounds " + str(m.bounds));
  return 0;
}

}  // namespace

int run_bounds_smoke() {
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / "ts_smoke_bounds";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (int r = check_scaled_rig()) return r;
  if (int r = check_dae(dir)) return r;
  if (int r = check_blender_fbx(dir)) return r;
  if (int r = check_gltf(dir)) return r;
  if (int r = check_special_meshes(dir)) return r;
  if (int r = check_big_skeleton()) return r;
  fs::remove_all(dir, ec);
  std::cout << "[smoke] bounds OK (scaled rig, Collada, Blender FBX bone children, glTF, UCX_ / LOD, 200 joints, fitted colliders)\n";
  return 0;
}
