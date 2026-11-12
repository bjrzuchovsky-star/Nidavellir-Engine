// FBX through ufbx (MIT), including FBX 6.x / 6100 and ASCII files that
// Assimp's FBX-DOM reader rejects. Meshes, skins and baked node animation
// land in the same Model the rest of the importer uses. Units come out in
// metres (ufbx target_unit_meters) and Y-up.

#include "render3d/ImportLimits.h"
#include "render3d/ModelImport.h"

#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

namespace {

std::string ustr(ufbx_string s) { return std::string(s.data, s.length); }

Mat4 mat_of(const ufbx_matrix& m) {
  Mat4 r;
  r.at(0, 0) = static_cast<float>(m.m00);
  r.at(1, 0) = static_cast<float>(m.m10);
  r.at(2, 0) = static_cast<float>(m.m20);
  r.at(0, 1) = static_cast<float>(m.m01);
  r.at(1, 1) = static_cast<float>(m.m11);
  r.at(2, 1) = static_cast<float>(m.m21);
  r.at(0, 2) = static_cast<float>(m.m02);
  r.at(1, 2) = static_cast<float>(m.m12);
  r.at(2, 2) = static_cast<float>(m.m22);
  r.at(0, 3) = static_cast<float>(m.m03);
  r.at(1, 3) = static_cast<float>(m.m13);
  r.at(2, 3) = static_cast<float>(m.m23);
  return r;
}

Vec3 vec_of(const ufbx_vec3& v) {
  return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

Quat quat_of(const ufbx_quat& q) {
  return normalize(Quat{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z),
                        static_cast<float>(q.w)});
}

}  // namespace

bool load_ufbx_model(const std::string& path, const ModelImportOptions& opt, Model* out, std::string* error) {
  if (!out) return false;
  ufbx_load_opts opts = {};
  opts.target_axes = ufbx_axes_right_handed_y_up;
  opts.target_unit_meters = 1.0f;
  opts.generate_missing_normals = true;
  ufbx_error uerr = {};
  ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &uerr);
  if (!scene) {
    if (error) {
      *error = path + ": ufbx could not open this FBX (" +
               std::string(uerr.description.data, uerr.description.length) + ")";
    }
    return false;
  }
  if (import_was_cancelled()) {
    ufbx_free_scene(scene);
    if (error) *error = path + ": Import cancelled.";
    return false;
  }

  // Joints: every skinned bone, its ancestors, and any node an animation moves.
  std::unordered_map<const ufbx_node*, int> index;
  std::vector<const ufbx_node*> marked;
  auto mark = [&](const ufbx_node* n) {
    if (!n || index.count(n)) return;
    index.emplace(n, -1);
    marked.push_back(n);
  };
  for (size_t i = 0; i < scene->meshes.count; ++i) {
    const ufbx_mesh* mesh = scene->meshes.data[i];
    for (size_t s = 0; s < mesh->skin_deformers.count; ++s) {
      const ufbx_skin_deformer* skin = mesh->skin_deformers.data[s];
      for (size_t c = 0; c < skin->clusters.count; ++c) mark(skin->clusters.data[c]->bone_node);
    }
  }
  std::vector<ufbx_baked_anim*> baked;
  if (opt.animations) {
    ufbx_bake_opts bopts = {};
    bopts.resample_rate = 30.0;
    bopts.maximum_sample_rate = 60.0;
    bopts.minimum_sample_rate = 1.0;
    for (size_t i = 0; i < scene->anim_stacks.count; ++i) {
      ufbx_error berr = {};
      ufbx_baked_anim* anim = ufbx_bake_anim(scene, scene->anim_stacks.data[i]->anim, &bopts, &berr);
      if (!anim) continue;
      baked.push_back(anim);
      for (size_t n = 0; n < anim->nodes.count; ++n) {
        const uint32_t id = anim->nodes.data[n].typed_id;
        if (id < scene->nodes.count) mark(scene->nodes.data[id]);
      }
    }
  }
  for (const ufbx_node* n : marked) {
    for (const ufbx_node* p = n->parent; p; p = p->parent) mark(p);
  }
  std::vector<const ufbx_node*> order;
  std::function<void(const ufbx_node*)> walk = [&](const ufbx_node* n) {
    if (index.count(n) && index[n] < 0) {
      index[n] = static_cast<int>(order.size());
      order.push_back(n);
    }
    for (size_t i = 0; i < n->children.count; ++i) walk(n->children.data[i]);
  };
  if (scene->root_node) walk(scene->root_node);
  for (const ufbx_node* n : marked)
    if (index[n] < 0) {
      index[n] = static_cast<int>(order.size());
      order.push_back(n);
    }
  out->skeleton.joints.reserve(order.size());
  for (const ufbx_node* n : order) {
    Joint j;
    j.name = ustr(n->name);
    j.parent = -1;
    if (n->parent && index.count(n->parent)) j.parent = index[n->parent];
    j.t = vec_of(n->local_transform.translation);
    j.r = quat_of(n->local_transform.rotation);
    j.s = vec_of(n->local_transform.scale);
    const ufbx_skin_cluster* cluster = nullptr;
    for (size_t i = 0; i < scene->meshes.count && !cluster; ++i) {
      const ufbx_mesh* mesh = scene->meshes.data[i];
      for (size_t s = 0; s < mesh->skin_deformers.count && !cluster; ++s) {
        const ufbx_skin_deformer* skin = mesh->skin_deformers.data[s];
        for (size_t c = 0; c < skin->clusters.count; ++c) {
          if (skin->clusters.data[c]->bone_node == n) {
            cluster = skin->clusters.data[c];
            break;
          }
        }
      }
    }
    j.inverse_bind = cluster ? mat_of(cluster->geometry_to_bone) : mat_of(n->node_to_world).inverse();
    out->skeleton.joints.push_back(std::move(j));
  }

  bool too_big = false;
  for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
    if (import_was_cancelled()) break;
    const ufbx_mesh* mesh = scene->meshes.data[mi];
    if (mesh->num_indices == 0 || mesh->num_faces == 0) continue;
    if (mesh->num_indices > kMaxImportVertices || mesh->num_triangles > kMaxImportTriangles) {
      too_big = true;
      continue;
    }
    const ufbx_node* inst = mesh->instances.count ? mesh->instances.data[0] : nullptr;
    const bool skinned = mesh->skin_deformers.count > 0 && !out->skeleton.joints.empty();
    const ufbx_skin_deformer* skin = skinned ? mesh->skin_deformers.data[0] : nullptr;
    const Mat4 world = (!skinned && inst) ? mat_of(inst->geometry_to_world) : Mat4{};
    ModelPrimitive prim;
    prim.mesh.name = ustr(mesh->name);
    prim.material = 0;
    std::vector<uint32_t> tris(std::max<size_t>(mesh->max_face_triangles * 3, 3));
    for (size_t f = 0; f < mesh->faces.count; ++f) {
      const ufbx_face face = mesh->faces.data[f];
      if (face.num_indices < 3) continue;
      const uint32_t ntri = ufbx_triangulate_face(tris.data(), tris.size(), mesh, face);
      for (uint32_t t = 0; t < ntri; ++t) {
        for (int k = 0; k < 3; ++k) {
          const uint32_t corner = tris[t * 3 + k];
          if (corner >= mesh->num_indices || !mesh->vertex_position.exists) continue;
          const uint32_t pi = mesh->vertex_position.indices.data[corner];
          if (pi >= mesh->vertex_position.values.count) continue;
          Vertex3D vx;
          Vec3 p = vec_of(mesh->vertex_position.values.data[pi]);
          Vec3 nrm{0, 1, 0};
          if (mesh->vertex_normal.exists) {
            const uint32_t ni = mesh->vertex_normal.indices.data[corner];
            if (ni < mesh->vertex_normal.values.count) nrm = vec_of(mesh->vertex_normal.values.data[ni]);
          }
          if (!skinned) {
            p = world.transform_point(p);
            nrm = world.transform_dir(nrm);
          }
          vx.position = p;
          vx.normal = normalize(nrm);
          if (mesh->vertex_uv.exists && mesh->vertex_uv.indices.count > corner) {
            const uint32_t ui = mesh->vertex_uv.indices.data[corner];
            if (ui < mesh->vertex_uv.values.count) {
              vx.u = static_cast<float>(mesh->vertex_uv.values.data[ui].x);
              vx.v = static_cast<float>(mesh->vertex_uv.values.data[ui].y);
            }
          }
          const uint32_t logical = mesh->vertex_position.indices.data[corner];
          SkinVertex sv;
          if (skinned && logical < skin->vertices.count) {
            const ufbx_skin_vertex& wv = skin->vertices.data[logical];
            int slot = 0;
            for (uint32_t w = 0; w < wv.num_weights && slot < 4; ++w) {
              if (wv.weight_begin + w >= skin->weights.count) break;
              const ufbx_skin_weight& sw = skin->weights.data[wv.weight_begin + w];
              if (sw.cluster_index >= skin->clusters.count) continue;
              const ufbx_node* bone = skin->clusters.data[sw.cluster_index]->bone_node;
              auto it = index.find(bone);
              if (it == index.end() || it->second < 0) continue;
              sv.joints[slot] = static_cast<float>(it->second);
              sv.weights[slot] = static_cast<float>(sw.weight);
              ++slot;
            }
          }
          prim.mesh.vertices.push_back(vx);
          if (skinned) prim.mesh.skin.push_back(sv);
          prim.mesh.indices.push_back(static_cast<std::uint32_t>(prim.mesh.vertices.size() - 1));
        }
      }
    }
    if (prim.mesh.indices.size() >= 3) {
      if (out->materials.empty()) out->materials.push_back(MaterialDesc{});
      out->primitives.push_back(std::move(prim));
    }
  }

  for (size_t i = 0; i < baked.size() && i < scene->anim_stacks.count; ++i) {
    const ufbx_baked_anim* anim = baked[i];
    Clip clip;
    clip.name = ustr(scene->anim_stacks.data[i]->name);
    if (clip.name.empty()) clip.name = "clip";
    for (size_t n = 0; n < anim->nodes.count; ++n) {
      const ufbx_baked_node& bn = anim->nodes.data[n];
      if (bn.typed_id >= scene->nodes.count) continue;
      auto it = index.find(scene->nodes.data[bn.typed_id]);
      if (it == index.end() || it->second < 0) continue;
      auto push3 = [&](ChannelPath path, const ufbx_baked_vec3_list& keys) {
        if (keys.count == 0) return;
        Channel ch;
        ch.joint = it->second;
        ch.path = path;
        for (size_t k = 0; k < keys.count; ++k) {
          ch.times.push_back(static_cast<float>(keys.data[k].time));
          const ufbx_vec3& v = keys.data[k].value;
          ch.values.insert(ch.values.end(),
                           {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
        }
        if (!ch.times.empty()) clip.duration = std::max(clip.duration, ch.times.back());
        clip.channels.push_back(std::move(ch));
      };
      if (!bn.constant_translation) push3(ChannelPath::Translation, bn.translation_keys);
      if (!bn.constant_scale) push3(ChannelPath::Scale, bn.scale_keys);
      if (!bn.constant_rotation && bn.rotation_keys.count > 0) {
        Channel ch;
        ch.joint = it->second;
        ch.path = ChannelPath::Rotation;
        for (size_t k = 0; k < bn.rotation_keys.count; ++k) {
          ch.times.push_back(static_cast<float>(bn.rotation_keys.data[k].time));
          const ufbx_quat& v = bn.rotation_keys.data[k].value;
          ch.values.insert(ch.values.end(), {static_cast<float>(v.x), static_cast<float>(v.y),
                                             static_cast<float>(v.z), static_cast<float>(v.w)});
        }
        if (!ch.times.empty()) clip.duration = std::max(clip.duration, ch.times.back());
        clip.channels.push_back(std::move(ch));
      }
    }
    if (!clip.channels.empty()) out->clips.push_back(std::move(clip));
    ufbx_free_baked_anim(baked[i]);
  }
  ufbx_free_scene(scene);
  if (import_was_cancelled()) {
    if (error) *error = path + ": Import cancelled.";
    return false;
  }
  if (too_big && out->primitives.empty()) {
    if (error) *error = path + ": mesh is over the import limit";
    return false;
  }
  if (out->primitives.empty() && out->clips.empty()) {
    if (error) *error = path + ": no mesh and no animation clips were found";
    return false;
  }
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
