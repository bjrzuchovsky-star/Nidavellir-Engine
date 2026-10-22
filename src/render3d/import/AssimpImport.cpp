// Assimp bridge (OBJ / MTL, FBX, Collada, 3DS, PLY, STL, X, SMD, LWO...):
// an aiScene mapped into a render3d::Model - meshes (node transforms baked
// for rigid meshes, bind space for skinned ones), PBR-ish materials with
// external or embedded textures, the skeleton (bones + the nodes between
// them) with up to four weights per vertex, and node animations on the
// skeleton's joints. Rigid meshes get their node transforms baked; skinned
// meshes are posed by the skeleton (rest pose = bind pose). Assimp is BSD-3 (fetched by hash, importers only).
#include "render3d/ModelImport.h"
#include "render3d/import/SourceFormats.h"

#if TS_WITH_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/commonMetaData.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace ts {
namespace tombstone {
namespace render3d {

#if !TS_WITH_ASSIMP

bool assimp_available() { return false; }
bool load_assimp_model(const std::string& path, const ModelImportOptions&, Model*,
                       std::string* error) {
  if (error) *error = path + ": built without Assimp (TS_WITH_ASSIMP=OFF)";
  return false;
}

#else

namespace fs = std::filesystem;

namespace {

Mat4 to_mat(const aiMatrix4x4& a) {
  Mat4 m;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) m.at(r, c) = a[r][c];
  }
  return m;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

void append(std::string* s, const std::string& w) {
  if (!w.empty()) *s += (s->empty() ? "" : "; ") + w;
}

struct TextureLoader {
  const aiScene* scene = nullptr;
  fs::path dir;
  std::vector<fs::path> roots;
  std::map<std::string, std::shared_ptr<const Image>> cache;
  std::set<std::string> missing;

  bool load_file(const fs::path& p, Image* im) {
    std::string err;
    if (lower(p.extension().string()) == ".vtf") {
      std::ifstream in(p, std::ios::binary);
      std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      return source::decode_vtf(b.data(), b.size(), im, &err);
    }
    return load_image_file(p.string(), im, &err);
  }

  bool resolve(const std::string& raw, fs::path* out) {
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.rfind("file://", 0) == 0) s = s.substr(7);
    std::error_code ec;
    const fs::path given = fs::u8path(s);
    const fs::path name = given.filename();
    std::vector<fs::path> tries;
    if (given.is_absolute()) tries.push_back(given);
    tries.push_back(dir / given);
    tries.push_back(dir / name);
    tries.push_back(dir / "textures" / name);
    tries.push_back(dir / "Textures" / name);
    for (const fs::path& r : roots) {
      tries.push_back(r / given);
      tries.push_back(r / name);
    }
    for (const fs::path& t : tries) {
      if (fs::is_regular_file(t, ec)) {
        *out = t;
        return true;
      }
    }
    // Same name, other image type (a .tga reference with a .png beside it).
    for (const char* x : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".vtf"}) {
      fs::path alt = dir / name;
      alt.replace_extension(x);
      if (fs::is_regular_file(alt, ec)) {
        *out = alt;
        return true;
      }
    }
    return false;
  }

  std::shared_ptr<const Image> get(const aiString& ai) {
    const std::string key = ai.C_Str();
    if (key.empty()) return nullptr;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::shared_ptr<const Image> img;
    if (const aiTexture* t = scene->GetEmbeddedTexture(ai.C_Str())) {
      auto im = std::make_shared<Image>();
      if (t->mHeight == 0) {
        std::string err;
        if (load_image_memory(reinterpret_cast<const unsigned char*>(t->pcData), t->mWidth, im.get(), &err)) img = im;
      } else {
        im->width = static_cast<int>(t->mWidth);
        im->height = static_cast<int>(t->mHeight);
        im->rgba.resize(static_cast<std::size_t>(t->mWidth) * t->mHeight * 4);
        for (std::size_t i = 0, n = static_cast<std::size_t>(t->mWidth) * t->mHeight; i < n; ++i) {
          const aiTexel& x = t->pcData[i];
          im->rgba[i * 4] = x.r;
          im->rgba[i * 4 + 1] = x.g;
          im->rgba[i * 4 + 2] = x.b;
          im->rgba[i * 4 + 3] = x.a;
        }
        img = im;
      }
    } else {
      fs::path p;
      auto im = std::make_shared<Image>();
      if (resolve(key, &p) && load_file(p, im.get()) && im->valid()) img = im;
    }
    if (!img) missing.insert(fs::u8path(key).filename().string());
    cache[key] = img;
    return img;
  }
};

std::shared_ptr<const Image> texture_of(const aiMaterial* m, std::initializer_list<aiTextureType> types,
                                        TextureLoader& tl, bool* named) {
  for (aiTextureType t : types) {
    aiString p;
    if (m->GetTextureCount(t) > 0 && m->GetTexture(t, 0, &p) == aiReturn_SUCCESS && p.length > 0) {
      if (named) *named = true;
      return tl.get(p);
    }
  }
  return nullptr;
}

// glTF layout: G = roughness, B = metalness.
std::shared_ptr<const Image> combine_metal_rough(const std::shared_ptr<const Image>& metal,
                                                 const std::shared_ptr<const Image>& rough) {
  if (!metal && !rough) return nullptr;
  const Image& ref = metal ? *metal : *rough;
  auto out = std::make_shared<Image>();
  out->width = ref.width;
  out->height = ref.height;
  out->rgba.assign(static_cast<std::size_t>(ref.width) * ref.height * 4, 255);
  auto sample = [](const Image& im, int x, int y, int w, int h) {
    const int sx = std::min(im.width - 1, x * im.width / std::max(1, w));
    const int sy = std::min(im.height - 1, y * im.height / std::max(1, h));
    return im.rgba[(static_cast<std::size_t>(sy) * im.width + sx) * 4];
  };
  for (int y = 0; y < ref.height; ++y) {
    for (int x = 0; x < ref.width; ++x) {
      unsigned char* p = &out->rgba[(static_cast<std::size_t>(y) * ref.width + x) * 4];
      p[0] = 0;
      p[1] = rough ? sample(*rough, x, y, ref.width, ref.height) : 255;
      p[2] = metal ? sample(*metal, x, y, ref.width, ref.height) : 255;
    }
  }
  return out;
}

MaterialDesc material_of(const aiMaterial* am, TextureLoader& tl) {
  MaterialDesc m;
  aiString name;
  if (am->Get(AI_MATKEY_NAME, name) == aiReturn_SUCCESS) m.name = name.C_Str();
  aiColor4D c;
  if (am->Get(AI_MATKEY_BASE_COLOR, c) == aiReturn_SUCCESS ||
      am->Get(AI_MATKEY_COLOR_DIFFUSE, c) == aiReturn_SUCCESS) {
    m.base_color[0] = c.r;
    m.base_color[1] = c.g;
    m.base_color[2] = c.b;
    m.base_color[3] = c.a;
  }
  float f = 1.0f;
  if (am->Get(AI_MATKEY_OPACITY, f) == aiReturn_SUCCESS && f >= 0.0f && f < 1.0f) m.base_color[3] = f;
  aiColor3D e;
  if (am->Get(AI_MATKEY_COLOR_EMISSIVE, e) == aiReturn_SUCCESS) {
    m.emissive[0] = e.r;
    m.emissive[1] = e.g;
    m.emissive[2] = e.b;
  }
  float metal = 0.0f, rough = -1.0f, shin = 0.0f;
  if (am->Get(AI_MATKEY_METALLIC_FACTOR, metal) == aiReturn_SUCCESS) m.metallic = std::clamp(metal, 0.0f, 1.0f);
  if (am->Get(AI_MATKEY_ROUGHNESS_FACTOR, rough) == aiReturn_SUCCESS && rough >= 0.0f) {
    m.roughness = std::clamp(rough, 0.0f, 1.0f);
  } else if (am->Get(AI_MATKEY_SHININESS, shin) == aiReturn_SUCCESS && shin > 0.0f) {
    m.roughness = std::clamp(std::sqrt(2.0f / (shin + 2.0f)), 0.05f, 1.0f);
  }
  int two = 0;
  if (am->Get(AI_MATKEY_TWOSIDED, two) == aiReturn_SUCCESS) m.double_sided = two != 0;
  bool named = false;
  m.base_color_image = texture_of(am, {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}, tl, &named);
  if (named && !m.base_color_image) {
    m.base_color_image = missing_texture_image();
    m.base_color[0] = m.base_color[1] = m.base_color[2] = 1.0f;
  }
  m.normal_image = texture_of(am, {aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA}, tl, nullptr);
  m.metal_rough_image = combine_metal_rough(texture_of(am, {aiTextureType_METALNESS}, tl, nullptr),
                                            texture_of(am, {aiTextureType_DIFFUSE_ROUGHNESS}, tl, nullptr));
  if (m.metal_rough_image) {
    // Factors multiply the maps (glTF rule): full strength unless given.
    if (am->Get(AI_MATKEY_METALLIC_FACTOR, metal) != aiReturn_SUCCESS) m.metallic = 1.0f;
    if (rough < 0.0f) m.roughness = 1.0f;
  }
  return m;
}

}  // namespace

bool assimp_available() { return true; }

bool load_assimp_model(const std::string& path, const ModelImportOptions& opt, Model* out,
                       std::string* error) {
  Assimp::Importer imp;
  imp.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
  imp.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);
  imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
  const unsigned flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                         aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights |
                         aiProcess_SortByPType | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                         aiProcess_ValidateDataStructure;
  const aiScene* sc = imp.ReadFile(fs::u8path(path).string(), flags);
  if (!sc || !sc->mRootNode || (sc->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
    if (error) *error = path + ": " + (imp.GetErrorString()[0] ? imp.GetErrorString() : "could not be read");
    return false;
  }
  std::string warn;
  TextureLoader tl;
  tl.scene = sc;
  tl.dir = fs::u8path(path).parent_path();
  for (const std::string& r : opt.material_paths) tl.roots.push_back(fs::u8path(r));
  for (unsigned i = 0; i < sc->mNumMaterials; ++i) out->materials.push_back(material_of(sc->mMaterials[i], tl));
  out->textures = tl.cache.size();

  // Nodes by name, globals, bone set.
  std::map<std::string, const aiNode*> by_name;
  std::map<const aiNode*, Mat4> global;
  std::function<void(const aiNode*, const Mat4&)> walk = [&](const aiNode* n, const Mat4& parent) {
    const Mat4 g = parent * to_mat(n->mTransformation);
    global[n] = g;
    by_name.emplace(n->mName.C_Str(), n);
    for (unsigned i = 0; i < n->mNumChildren; ++i) walk(n->mChildren[i], g);
  };
  walk(sc->mRootNode, Mat4{});
  std::map<std::string, Mat4> offsets;
  for (unsigned i = 0; i < sc->mNumMeshes; ++i) {
    const aiMesh* m = sc->mMeshes[i];
    for (unsigned b = 0; b < m->mNumBones; ++b) offsets.emplace(m->mBones[b]->mName.C_Str(), to_mat(m->mBones[b]->mOffsetMatrix));
  }
  // Skinned meshes go to model space through the first skinned mesh's node
  // transform G (usually identity, or a Z-up -> Y-up root); the inverse
  // binds get G^-1 behind them, so palette * (G v) = joint * offset * v.
  Mat4 skin_space;
  {
    std::function<bool(const aiNode*)> find = [&](const aiNode* n) {
      for (unsigned k = 0; k < n->mNumMeshes; ++k) {
        if (sc->mMeshes[n->mMeshes[k]]->mNumBones > 0) {
          skin_space = global[n];
          return true;
        }
      }
      for (unsigned i = 0; i < n->mNumChildren; ++i) {
        if (find(n->mChildren[i])) return true;
      }
      return false;
    };
    find(sc->mRootNode);
  }
  const Mat4 skin_space_inv = skin_space.inverse();
  std::set<const aiNode*> joints;
  for (const auto& [name, off] : offsets) {
    auto it = by_name.find(name);
    if (it == by_name.end()) continue;
    // The bone and every node up to the highest bone above it.
    std::vector<const aiNode*> chain;
    const aiNode* top = it->second;
    for (const aiNode* n = it->second; n; n = n->mParent) {
      chain.push_back(n);
      if (offsets.count(n->mName.C_Str())) top = n;
    }
    for (const aiNode* n : chain) {
      joints.insert(n);
      if (n == top) break;
    }
  }
  std::map<std::string, int> joint_index;
  if (!joints.empty()) {
    std::function<void(const aiNode*, int)> order = [&](const aiNode* n, int parent) {
      int me = parent;
      if (joints.count(n)) {
        Joint j;
        j.name = n->mName.C_Str();
        j.parent = parent;
        aiVector3D s, t;
        aiQuaternion r;
        n->mTransformation.Decompose(s, r, t);
        j.t = {t.x, t.y, t.z};
        j.r = normalize(Quat{r.x, r.y, r.z, r.w});
        j.s = {s.x, s.y, s.z};
        if (parent < 0 && n->mParent) j.base = global[n->mParent];
        auto off = offsets.find(j.name);
        j.inverse_bind = (off != offsets.end() ? off->second : global[n].inverse() * skin_space) * skin_space_inv;
        me = static_cast<int>(out->skeleton.joints.size());
        joint_index.emplace(j.name, me);
        out->skeleton.joints.push_back(std::move(j));
      }
      for (unsigned i = 0; i < n->mNumChildren; ++i) order(n->mChildren[i], me);
    };
    order(sc->mRootNode, -1);
  }

  // Blender FBX quirk: an object parented to a bone is written with its
  // transform relative to the armature (plus the bone's length along +Y),
  // not relative to the bone, so read literally its parts fly metres away
  // from the body. When the file says Blender wrote it and the armature
  // reading puts clearly more of those meshes on the skeleton, use it.
  int bone_fixes = 0;
  {
    aiString gen;
    const bool blender = sc->mMetaData && sc->mMetaData->Get(AI_METADATA_SOURCE_GENERATOR, gen) &&
                         lower(gen.C_Str()).find("blender") != std::string::npos;
    struct Box {
      Vec3 lo{0, 0, 0}, hi{0, 0, 0};
      int n = 0;
      void expand(const Vec3& p) {
        lo = n ? vmin(lo, p) : p;
        hi = n ? vmax(hi, p) : p;
        ++n;
      }
    };
    auto is_bone = [&](const aiNode* n) {
      if (!n || n->mNumMeshes) return false;
      for (const aiNode* q = n; q; q = q->mParent) {
        if (joints.count(q)) return true;
      }
      return false;
    };
    auto rigid = [&](const aiNode* n) {
      if (!n->mNumMeshes) return false;
      for (unsigned k = 0; k < n->mNumMeshes; ++k) {
        if (sc->mMeshes[n->mMeshes[k]]->mNumBones > 0) return false;
      }
      return true;
    };
    auto center_of = [&](const aiNode* n) {
      Box b;
      for (unsigned k = 0; k < n->mNumMeshes; ++k) {
        const aiMesh* am = sc->mMeshes[n->mMeshes[k]];
        for (unsigned v = 0; v < am->mNumVertices; ++v) b.expand({am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z});
      }
      return b.n ? (b.lo + b.hi) * 0.5f : Vec3{};
    };
    struct Cand {
      const aiNode* n;
      const aiNode* arm;
      Vec3 c;
    };
    std::vector<Cand> cands;
    std::map<const aiNode*, std::vector<std::size_t>> by_bone;
    if (blender && !joints.empty()) {
      for (const auto& [n, g] : global) {
        if (!rigid(n) || !is_bone(n->mParent)) continue;
        const aiNode* arm = n->mParent;
        while (arm && is_bone(arm)) arm = arm->mParent;
        if (!arm) continue;
        by_bone[n->mParent].push_back(cands.size());
        cands.push_back({n, arm, center_of(n)});
      }
    }
    if (!cands.empty()) {
      // Bone length: a connected child bone sits at (0, length, 0); else the
      // larger of the mean child bone distance (Blender's own importer) and
      // the length that lines the attached parts up with the bone in depth.
      std::map<const aiNode*, float> len;
      for (const auto& [b, list] : by_bone) {
        float connected = -1.0f, avg = 0.0f;
        int count = 0;
        for (unsigned i = 0; i < b->mNumChildren; ++i) {
          const aiNode* c = b->mChildren[i];
          if (!is_bone(c)) continue;
          const aiMatrix4x4& t = c->mTransformation;
          const float tx = t.a4, ty = t.b4, tz = t.c4;
          if (ty > 1e-6f && std::abs(tx) <= 0.02f * ty && std::abs(tz) <= 0.02f * ty) connected = std::max(connected, ty);
          avg += std::sqrt(tx * tx + ty * ty + tz * tz);
          ++count;
        }
        if (connected > 0.0f) {
          len[b] = connected;
          continue;
        }
        const aiNode* arm = cands[list.front()].arm;
        const float head_y = (global[arm].inverse() * global[b]).at(1, 3);
        float depth = 0.0f;
        for (std::size_t k : list) depth += to_mat(cands[k].n->mTransformation).transform_point(cands[k].c).y - head_y;
        depth /= static_cast<float>(list.size());
        len[b] = std::max(count ? avg / count : 0.0f, std::max(depth, 0.0f));
      }
      Box heads;
      for (const auto& [n, g] : global) {
        if (is_bone(n)) heads.expand(g.transform_point({0, 0, 0}));
      }
      const Vec3 e = heads.hi - heads.lo;
      const float pad = 0.25f * std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z) + 1e-4f;
      auto inside = [&](const Vec3& p) {
        return p.x >= heads.lo.x - pad && p.x <= heads.hi.x + pad && p.y >= heads.lo.y - pad &&
               p.y <= heads.hi.y + pad && p.z >= heads.lo.z - pad && p.z <= heads.hi.z + pad;
      };
      std::vector<Mat4> alt(cands.size());
      int lit_in = 0, alt_in = 0;
      for (std::size_t k = 0; k < cands.size(); ++k) {
        const Cand& c = cands[k];
        alt[k] = global[c.arm] * Mat4::translate({0, -len[c.n->mParent], 0}) * to_mat(c.n->mTransformation);
        lit_in += inside(global[c.n].transform_point(c.c)) ? 1 : 0;
        alt_in += inside(alt[k].transform_point(c.c)) ? 1 : 0;
      }
      if (alt_in > lit_in) {
        std::function<void(const aiNode*, const Mat4&)> rewalk = [&](const aiNode* n, const Mat4& g) {
          global[n] = g;
          for (unsigned i = 0; i < n->mNumChildren; ++i) rewalk(n->mChildren[i], g * to_mat(n->mChildren[i]->mTransformation));
        };
        for (std::size_t k = 0; k < cands.size(); ++k) rewalk(cands[k].n, alt[k]);
        bone_fixes = static_cast<int>(cands.size());
      }
    }
  }
  if (bone_fixes) {
    append(&warn, "Blender FBX: " + std::to_string(bone_fixes) +
                      " bone-parented meshes placed relative to the armature (exporter quirk)");
  }

  // Meshes.
  std::function<void(const aiNode*)> meshes = [&](const aiNode* n) {
    for (unsigned k = 0; k < n->mNumMeshes; ++k) {
      const aiMesh* am = sc->mMeshes[n->mMeshes[k]];
      if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) || am->mNumVertices == 0) continue;
      ModelPrimitive p;
      p.mesh.name = am->mName.length ? am->mName.C_Str() : n->mName.C_Str();
      for (const aiNode* q = n; q && q != sc->mRootNode; q = q->mParent) {
        const std::string nm = q->mName.C_Str();
        p.node = p.node.empty() ? nm : nm + "/" + p.node;
      }
      p.material = am->mMaterialIndex < out->materials.size() ? static_cast<int>(am->mMaterialIndex) : -1;
      const bool skinned = am->mNumBones > 0 && !joint_index.empty();
      const Mat4& g = skinned ? skin_space : global[n];
      const Mat4 nm = g.inverse();  // normals: inverse transpose
      for (unsigned v = 0; v < am->mNumVertices; ++v) {
        Vertex3D vx;
        Vec3 pos{am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z};
        Vec3 nrm = am->mNormals ? Vec3{am->mNormals[v].x, am->mNormals[v].y, am->mNormals[v].z} : Vec3{0, 1, 0};
        pos = g.transform_point(pos);
        nrm = Vec3{nm.at(0, 0) * nrm.x + nm.at(1, 0) * nrm.y + nm.at(2, 0) * nrm.z,
                   nm.at(0, 1) * nrm.x + nm.at(1, 1) * nrm.y + nm.at(2, 1) * nrm.z,
                   nm.at(0, 2) * nrm.x + nm.at(1, 2) * nrm.y + nm.at(2, 2) * nrm.z};
        vx.position = pos;
        vx.normal = normalize(nrm);
        if (am->mTextureCoords[0]) {
          vx.u = am->mTextureCoords[0][v].x;
          vx.v = am->mTextureCoords[0][v].y;
        }
        p.mesh.vertices.push_back(vx);
      }
      for (unsigned f = 0; f < am->mNumFaces; ++f) {
        const aiFace& face = am->mFaces[f];
        if (face.mNumIndices != 3) continue;
        for (int c = 0; c < 3; ++c) p.mesh.indices.push_back(face.mIndices[c]);
      }
      if (skinned) {
        std::vector<std::vector<std::pair<float, int>>> w(am->mNumVertices);
        for (unsigned b = 0; b < am->mNumBones; ++b) {
          const aiBone* bone = am->mBones[b];
          auto ji = joint_index.find(bone->mName.C_Str());
          if (ji == joint_index.end()) continue;
          for (unsigned k2 = 0; k2 < bone->mNumWeights; ++k2) {
            const aiVertexWeight& vw = bone->mWeights[k2];
            if (vw.mVertexId < am->mNumVertices && vw.mWeight > 0.0f) w[vw.mVertexId].emplace_back(vw.mWeight, ji->second);
          }
        }
        p.mesh.skin.resize(am->mNumVertices);
        for (unsigned v = 0; v < am->mNumVertices; ++v) {
          auto& list = w[v];
          std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
          SkinVertex s;
          float sum = 0.0f;
          for (std::size_t j = 0; j < 4; ++j) {
            s.joints[j] = j < list.size() ? static_cast<float>(list[j].second) : 0.0f;
            s.weights[j] = j < list.size() ? list[j].first : 0.0f;
            sum += s.weights[j];
          }
          if (sum > 0.0f) {
            for (float& x : s.weights) x /= sum;
          } else {
            s.weights[0] = 1.0f;  // unweighted: the first joint
          }
          p.mesh.skin[v] = s;
        }
      }
      compute_tangents(p.mesh);
      if (p.mesh.valid()) out->primitives.push_back(std::move(p));
    }
    for (unsigned i = 0; i < n->mNumChildren; ++i) meshes(n->mChildren[i]);
  };
  meshes(sc->mRootNode);

  // Animations on the skeleton's joints.
  if (opt.animations && !joint_index.empty()) {
    for (unsigned a = 0; a < sc->mNumAnimations; ++a) {
      const aiAnimation* an = sc->mAnimations[a];
      const double tps = an->mTicksPerSecond > 0 ? an->mTicksPerSecond : 25.0;
      Clip clip;
      clip.name = an->mName.length ? an->mName.C_Str() : "clip" + std::to_string(a);
      for (unsigned c = 0; c < an->mNumChannels; ++c) {
        const aiNodeAnim* na = an->mChannels[c];
        auto ji = joint_index.find(na->mNodeName.C_Str());
        if (ji == joint_index.end()) continue;
        if (na->mNumPositionKeys) {
          Channel ch;
          ch.joint = ji->second;
          ch.path = ChannelPath::Translation;
          for (unsigned k = 0; k < na->mNumPositionKeys; ++k) {
            ch.times.push_back(static_cast<float>(na->mPositionKeys[k].mTime / tps));
            const aiVector3D& v = na->mPositionKeys[k].mValue;
            ch.values.insert(ch.values.end(), {v.x, v.y, v.z});
          }
          clip.channels.push_back(std::move(ch));
        }
        if (na->mNumRotationKeys) {
          Channel ch;
          ch.joint = ji->second;
          ch.path = ChannelPath::Rotation;
          for (unsigned k = 0; k < na->mNumRotationKeys; ++k) {
            ch.times.push_back(static_cast<float>(na->mRotationKeys[k].mTime / tps));
            const aiQuaternion& q = na->mRotationKeys[k].mValue;
            ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
          }
          clip.channels.push_back(std::move(ch));
        }
        if (na->mNumScalingKeys) {
          Channel ch;
          ch.joint = ji->second;
          ch.path = ChannelPath::Scale;
          for (unsigned k = 0; k < na->mNumScalingKeys; ++k) {
            ch.times.push_back(static_cast<float>(na->mScalingKeys[k].mTime / tps));
            const aiVector3D& v = na->mScalingKeys[k].mValue;
            ch.values.insert(ch.values.end(), {v.x, v.y, v.z});
          }
          clip.channels.push_back(std::move(ch));
        }
      }
      for (const Channel& ch : clip.channels) {
        if (!ch.times.empty()) clip.duration = std::max(clip.duration, ch.times.back());
      }
      if (!clip.channels.empty()) out->clips.push_back(std::move(clip));
    }
  }
  if (!tl.missing.empty()) {
    std::string list;
    for (const std::string& m : tl.missing) list += (list.empty() ? "" : ", ") + m;
    append(&warn, "missing textures (magenta): " + list);
  }
  update_bounds(out);
  // Auto up axis: FBX says which way is up in its metadata.
  if (opt.up == UpAxis::Auto && sc->mMetaData) {
    int up = 1;
    if (sc->mMetaData->Get("UpAxis", up) && up == 2) bake_import_transform(out, 1.0f, true);
    double unit = 0.0;
    float unitf = 0.0f;
    if (sc->mMetaData->Get("UnitScaleFactor", unitf)) unit = unitf;
    else sc->mMetaData->Get("UnitScaleFactor", unit);
    if (unit > 0.0 && std::abs(unit - 1.0) < 1e-6 && opt.unit_scale == 1.0f) {
      append(&warn, "file units are centimetres (set Unit scale to 0.01 for metres)");
    }
  }
  if (error) *error = warn;
  return true;
}

#endif  // TS_WITH_ASSIMP

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
