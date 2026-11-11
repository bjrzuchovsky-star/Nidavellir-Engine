#include "SmokeBigModel.h"

#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "editor/assets/ImportJob.h"
#include "render3d/ColliderFit.h"
#include "render3d/HullFit.h"
#include "render3d/ImportLimits.h"
#include "render3d/Mesh.h"
#include "render3d/ModelImport.h"
#include "render3d/UploadStage.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace r3 = ts::tombstone::render3d;
namespace ed = ts::tombstone::editor;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] bigmodel FAILED: " << what << '\n';
  return 1;
}

std::uint64_t peak_bytes() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmHWM:", 0) == 0) {
      std::uint64_t kb = 0;
      for (char c : line) {
        if (c >= '0' && c <= '9') kb = kb * 10 + static_cast<unsigned>(c - '0');
      }
      return kb * 1024ull;
    }
  }
  return 0;
}

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

void write_u32(std::ostream& o, std::uint32_t v) {
  const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                              static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
  o.write(reinterpret_cast<const char*>(b), 4);
}

void write_f32(std::ostream& o, float v) {
  static_assert(sizeof(float) == 4, "float");
  o.write(reinterpret_cast<const char*>(&v), 4);
}

bool write_tga(const fs::path& path, int w, int h, unsigned char r, unsigned char g, unsigned char b) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  unsigned char header[18] = {};
  header[2] = 2;
  header[12] = static_cast<unsigned char>(w & 255);
  header[13] = static_cast<unsigned char>((w >> 8) & 255);
  header[14] = static_cast<unsigned char>(h & 255);
  header[15] = static_cast<unsigned char>((h >> 8) & 255);
  header[16] = 32;
  header[17] = 0x28;
  out.write(reinterpret_cast<const char*>(header), 18);
  std::vector<unsigned char> row(static_cast<std::size_t>(w) * 4u);
  for (int x = 0; x < w; ++x) {
    row[static_cast<std::size_t>(x) * 4u] = b;
    row[static_cast<std::size_t>(x) * 4u + 1] = g;
    row[static_cast<std::size_t>(x) * 4u + 2] = r;
    row[static_cast<std::size_t>(x) * 4u + 3] = 255;
  }
  for (int y = 0; y < h; ++y) out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
  return static_cast<bool>(out);
}

// 1024x1024 quads = 2,097,152 triangles, a 160-bone skin, four 4K textures.
bool write_giant(const fs::path& dir, std::string* gltf_out) {
  constexpr int kSeg = 1024;
  const int vx = kSeg + 1;
  const int vy = kSeg + 1;
  const std::size_t nvert = static_cast<std::size_t>(vx) * static_cast<std::size_t>(vy);
  const std::size_t ntri = static_cast<std::size_t>(kSeg) * static_cast<std::size_t>(kSeg) * 2u;
  const std::size_t nidx = ntri * 3u;
  constexpr int kJoints = 160;

  for (int i = 0; i < 4; ++i) {
    const unsigned char tint = static_cast<unsigned char>(40 + i * 50);
    if (!write_tga(dir / ("tex" + std::to_string(i) + ".tga"), 4096, 4096, tint, 80, 40)) return false;
  }

  const fs::path bin_path = dir / "hero.bin";
  std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
  if (!bin) return false;
  auto pad4 = [&] {
    while (static_cast<std::uint64_t>(bin.tellp()) % 4) bin.put(0);
  };
  const auto off_pos = static_cast<std::uint64_t>(bin.tellp());
  for (int y = 0; y < vy; ++y) {
    for (int x = 0; x < vx; ++x) {
      const float fx = (static_cast<float>(x) / kSeg - 0.5f) * 2.0f;
      const float fz = (static_cast<float>(y) / kSeg - 0.5f) * 2.0f;
      write_f32(bin, fx);
      write_f32(bin, 0.05f * std::sin(fx * 6.0f) * std::cos(fz * 6.0f));
      write_f32(bin, fz);
    }
  }
  const auto len_pos = static_cast<std::uint64_t>(bin.tellp()) - off_pos;
  const auto off_nrm = static_cast<std::uint64_t>(bin.tellp());
  for (std::size_t i = 0; i < nvert; ++i) {
    write_f32(bin, 0);
    write_f32(bin, 1);
    write_f32(bin, 0);
  }
  const auto len_nrm = static_cast<std::uint64_t>(bin.tellp()) - off_nrm;
  const auto off_uv = static_cast<std::uint64_t>(bin.tellp());
  for (int y = 0; y < vy; ++y) {
    for (int x = 0; x < vx; ++x) {
      write_f32(bin, static_cast<float>(x) / kSeg);
      write_f32(bin, static_cast<float>(y) / kSeg);
    }
  }
  const auto len_uv = static_cast<std::uint64_t>(bin.tellp()) - off_uv;
  pad4();
  const auto off_j = static_cast<std::uint64_t>(bin.tellp());
  for (std::size_t i = 0; i < nvert; ++i) {
    const unsigned char j[4] = {0, 1, 0, 0};
    bin.write(reinterpret_cast<const char*>(j), 4);
  }
  const auto len_j = static_cast<std::uint64_t>(bin.tellp()) - off_j;
  const auto off_w = static_cast<std::uint64_t>(bin.tellp());
  for (std::size_t i = 0; i < nvert; ++i) {
    write_f32(bin, 1);
    write_f32(bin, 0);
    write_f32(bin, 0);
    write_f32(bin, 0);
  }
  const auto len_w = static_cast<std::uint64_t>(bin.tellp()) - off_w;
  pad4();
  const auto off_i = static_cast<std::uint64_t>(bin.tellp());
  for (int y = 0; y < kSeg; ++y) {
    for (int x = 0; x < kSeg; ++x) {
      const std::uint32_t a = static_cast<std::uint32_t>(y * vx + x);
      const std::uint32_t b = a + 1;
      const std::uint32_t c = a + static_cast<std::uint32_t>(vx);
      const std::uint32_t d = c + 1;
      write_u32(bin, a);
      write_u32(bin, c);
      write_u32(bin, b);
      write_u32(bin, b);
      write_u32(bin, c);
      write_u32(bin, d);
    }
  }
  const auto len_i = static_cast<std::uint64_t>(bin.tellp()) - off_i;
  pad4();
  const auto off_ibm = static_cast<std::uint64_t>(bin.tellp());
  for (int j = 0; j < kJoints; ++j) {
    for (int c = 0; c < 16; ++c) write_f32(bin, (c % 5) == 0 ? 1.0f : 0.0f);
  }
  const auto len_ibm = static_cast<std::uint64_t>(bin.tellp()) - off_ibm;
  const auto off_t = static_cast<std::uint64_t>(bin.tellp());
  write_f32(bin, 0);
  write_f32(bin, 1);
  const auto len_t = static_cast<std::uint64_t>(bin.tellp()) - off_t;
  const auto off_tv = static_cast<std::uint64_t>(bin.tellp());
  for (int k = 0; k < 2; ++k) {
    write_f32(bin, 0);
    write_f32(bin, 0.01f * k);
    write_f32(bin, 0);
  }
  const auto len_tv = static_cast<std::uint64_t>(bin.tellp()) - off_tv;
  const auto bin_size = static_cast<std::uint64_t>(bin.tellp());
  bin.close();
  if (!bin) return false;
  (void)nidx;

  std::string joints;
  for (int j = 0; j < kJoints; ++j) joints += (j ? "," : "") + std::to_string(1 + j);
  std::string children;
  for (int j = 1; j < kJoints; ++j) children += (j > 1 ? "," : "") + std::to_string(1 + j);
  std::string nodes;
  nodes += "{\"name\":\"Body\",\"mesh\":0,\"skin\":0}";
  nodes += ",{\"name\":\"root\",\"children\":[" + children + "]}";
  for (int j = 1; j < kJoints; ++j) nodes += ",{\"name\":\"bone" + std::to_string(j) + "\"}";

  std::string materials;
  std::string textures;
  std::string images;
  for (int i = 0; i < 4; ++i) {
    if (i) {
      materials += ",";
      textures += ",";
      images += ",";
    }
    materials += "{\"name\":\"m" + std::to_string(i) +
                 "\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" + std::to_string(i) + "}}}";
    textures += "{\"source\":" + std::to_string(i) + "}";
    images += "{\"uri\":\"tex" + std::to_string(i) + ".tga\"}";
  }

  auto acc = [](int view, int comp, std::size_t count, const char* type, const char* extra) {
    std::string s = "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(comp) +
                    ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
    if (extra && *extra) s += std::string(",") + extra;
    return s + "}";
  };
  std::string views;
  auto view = [&](std::uint64_t off, std::uint64_t len, int target) {
    if (!views.empty()) views += ",";
    views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(len);
    if (target) views += ",\"target\":" + std::to_string(target);
    views += "}";
  };
  view(off_pos, len_pos, 34962);
  view(off_nrm, len_nrm, 34962);
  view(off_uv, len_uv, 34962);
  view(off_j, len_j, 34962);
  view(off_w, len_w, 34962);
  view(off_i, len_i, 34963);
  view(off_ibm, len_ibm, 0);
  view(off_t, len_t, 0);
  view(off_tv, len_tv, 0);

  const std::string json =
      std::string("{\"asset\":{\"version\":\"2.0\",\"generator\":\"nidavellir-smoke\"},") +
      "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}]," +
      "\"nodes\":[" + nodes + "]," +
      "\"skins\":[{\"joints\":[" + joints + "],\"inverseBindMatrices\":6,\"skeleton\":1}]," +
      "\"meshes\":[{\"name\":\"body\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,"
      "\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"indices\":5,\"material\":0}]}]," +
      "\"materials\":[" + materials + "]," +
      "\"textures\":[" + textures + "]," +
      "\"images\":[" + images + "]," +
      "\"animations\":[{\"name\":\"idle\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}],"
      "\"samplers\":[{\"input\":7,\"output\":8}]}]," +
      "\"accessors\":[" +
      acc(0, 5126, nvert, "VEC3", "\"min\":[-1,-1,-1],\"max\":[1,1,1]") + "," +
      acc(1, 5126, nvert, "VEC3", nullptr) + "," + acc(2, 5126, nvert, "VEC2", nullptr) + "," +
      acc(3, 5121, nvert, "VEC4", nullptr) + "," + acc(4, 5126, nvert, "VEC2", nullptr) + "," +
      // WEIGHTS_0 is VEC4, not VEC2. Fixed below if I mistyped — see the string.
      acc(5, 5125, nidx, "SCALAR", nullptr) + "," + acc(6, 5126, static_cast<std::size_t>(kJoints), "MAT4", nullptr) +
      "," + acc(7, 5126, 2, "SCALAR", nullptr) + "," + acc(8, 5126, 2, "VEC3", nullptr) +
      "]," +
      "\"bufferViews\":[" + views + "]," +
      "\"buffers\":[{\"uri\":\"hero.bin\",\"byteLength\":" + std::to_string(bin_size) + "}]}";

  // WEIGHTS accessor was built as VEC2 by the comment-adjacent call. Rebuild
  // that one field so the file is valid: accessor index 4 must be VEC4.
  const std::string bad = acc(4, 5126, nvert, "VEC2", nullptr);
  const std::string good = acc(4, 5126, nvert, "VEC4", nullptr);
  std::string fixed = json;
  const auto at = fixed.find(bad);
  if (at == std::string::npos) return false;
  fixed.replace(at, bad.size(), good);

  const fs::path out = dir / "hero.gltf";
  std::ofstream jf(out, std::ios::binary | std::ios::trunc);
  jf << fixed;
  if (!jf) return false;
  *gltf_out = out.string();
  return true;
}

}  // namespace

int run_big_model_smoke() {
  const fs::path root = fs::temp_directory_path() / "tombstone_bigmodel_smoke";
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path src = root / "src";
  const fs::path claim = root / "claim";
  fs::create_directories(src, ec);
  fs::create_directories(claim / "assets", ec);

  // Collider thinning has to stay fast on a cloud that used to be quadratic.
  {
    std::vector<r3::Vec3> cloud(100000);
    for (std::size_t i = 0; i < cloud.size(); ++i) {
      const float a = static_cast<float>(i) * 0.017f;
      cloud[i] = {std::sin(a), std::cos(a * 0.37f), static_cast<float>(i % 80) * 0.01f};
    }
    const auto t0 = Clock::now();
    const std::vector<r3::Vec3> hull = r3::convex_hull(cloud, 32);
    if (ms_since(t0) > 2000.0) return fail("convex hull of 100k points took too long");
    if (hull.size() < 4 || hull.size() > 32) return fail("hull vertex limit");
  }
  {
    r3::MeshData grid;
    constexpr int s = 80;
    for (int y = 0; y <= s; ++y) {
      for (int x = 0; x <= s; ++x) {
        r3::Vertex3D v;
        v.position = {static_cast<float>(x), 0, static_cast<float>(y)};
        v.normal = {0, 1, 0};
        grid.vertices.push_back(v);
      }
    }
    for (int y = 0; y < s; ++y) {
      for (int x = 0; x < s; ++x) {
        const auto a = static_cast<std::uint32_t>(y * (s + 1) + x);
        grid.indices.insert(grid.indices.end(), {a, a + static_cast<std::uint32_t>(s + 1), a + 1, a + 1,
                                                 a + static_cast<std::uint32_t>(s + 1),
                                                 a + static_cast<std::uint32_t>(s + 2)});
      }
    }
    r3::Model m;
    r3::ModelPrimitive p;
    p.mesh = std::move(grid);
    m.primitives.push_back(std::move(p));
    r3::update_bounds(&m);
    const auto t0 = Clock::now();
    if (!r3::ensure_fitted(&m, {})) return fail("heavy mesh produced no collider");
    if (ms_since(t0) > 3000.0) return fail("collider fit on a dense mesh took too long");
    if (m.fitted.convex.empty() || m.fitted.convex[0].size() > 64) return fail("dense mesh was not a limited hull");
    if (m.fitted.pieces.size() > 8) return fail("dense mesh decomposition exploded");
  }

  // Upload slices stay inside a frame budget and cover the mesh.
  {
    r3::MeshData mesh;
    constexpr int n = 200000;
    mesh.vertices.resize(static_cast<std::size_t>(n));
    mesh.indices.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      mesh.vertices[static_cast<std::size_t>(i)].position = {static_cast<float>(i), 0, 0};
      mesh.indices[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(i % 3 == 2 ? i - 2 : i);
    }
    // indices must be a triangle list; rebuild properly
    mesh.indices.clear();
    for (int i = 0; i + 2 < n; i += 3) {
      mesh.indices.push_back(static_cast<std::uint32_t>(i));
      mesh.indices.push_back(static_cast<std::uint32_t>(i + 1));
      mesh.indices.push_back(static_cast<std::uint32_t>(i + 2));
    }
    std::size_t cursor = 0;
    std::size_t slices = 0;
    double worst = 0;
    while (cursor < mesh.indices.size()) {
      const r3::MeshSlice sl = r3::plan_mesh_slice(mesh, cursor, r3::kUploadBudgetBytes);
      if (sl.index_count == 0 || sl.index_count % 3 != 0) return fail("empty upload slice");
      if (sl.bytes > r3::kUploadBudgetBytes * 2) return fail("upload slice over budget");
      const auto t0 = Clock::now();
      r3::MeshData chunk;
      if (!r3::bake_mesh_slice(mesh, sl, &chunk)) return fail("bake slice");
      worst = std::max(worst, ms_since(t0));
      cursor += sl.index_count;
      ++slices;
      if (slices > 100000) return fail("slice loop");
    }
    if (cursor != mesh.indices.size()) return fail("slices did not cover the mesh");
    if (slices < 2) return fail("a large mesh uploaded in one slice");
    if (worst > 40.0) return fail("one upload slice blocked for " + std::to_string(worst) + " ms");
  }

  std::cout << "[smoke] bigmodel: writing the synthetic model\n" << std::flush;
  std::string gltf;
  const auto wrote = Clock::now();
  if (!write_giant(src, &gltf)) return fail("could not write the synthetic model");
  std::cout << "[smoke] bigmodel: wrote in " << ms_since(wrote) / 1000.0 << " s\n" << std::flush;

  // A corrupt file is an error, not a crash.
  {
    const fs::path bad = src / "broken.glb";
    std::ofstream o(bad, std::ios::binary | std::ios::trunc);
    o << "not a glb, and not nearly long enough";
    o.close();
    r3::Model m;
    std::string err;
    bool threw = false;
    bool ok = true;
    try {
      ok = r3::load_model(bad.string(), &m, &err);
    } catch (...) {
      threw = true;
    }
    if (threw || ok || err.empty()) return fail("corrupt file should be a readable error");
  }

  const std::uint64_t before = peak_bytes();
  ed::ImportJob job;
  ed::ImportJob::Request req;
  req.project_dir = claim.string();
  req.sources = {gltf, (src / "hero.bin").string(), (src / "tex0.tga").string(), (src / "tex1.tga").string(),
                 (src / "tex2.tga").string(), (src / "tex3.tga").string()};
  req.dest_folder = "assets/models";
  req.settings.sort_imports = false;
  const auto t_start = Clock::now();
  job.start(std::move(req));
  if (ms_since(t_start) > 200.0) return fail("import start blocked the caller");

  int polls = 0;
  double worst_poll = 0;
  std::string saw_phase;
  const auto t_job = Clock::now();
  for (;;) {
    const auto t0 = Clock::now();
    const ed::ImportJob::State st = job.state();
    worst_poll = std::max(worst_poll, ms_since(t0));
    if (!st.phase.empty()) saw_phase = st.phase;
    if (st.finished) break;
    if (ms_since(t_job) > 180000.0) {
      job.cancel();
      return fail("import did not finish within 180 s");
    }
    ++polls;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ed::ImportJob::State done;
  if (!job.take(&done)) return fail("import result was not delivered");
  if (done.cancelled) return fail("import was cancelled: " + done.error);
  if (!done.ok) return fail(done.error.empty() ? "import failed" : done.error);
  if (polls < 3) return fail("import finished before the caller could keep polling");
  if (worst_poll > 30.0) return fail("a progress poll blocked for " + std::to_string(worst_poll) + " ms");
  if (saw_phase.empty()) return fail("import reported no phase");
  std::cout << "[smoke] bigmodel: imported in " << ms_since(t_job) / 1000.0 << " s, worst poll " << worst_poll
            << " ms\n"
            << std::flush;

  ed::assetdb::AssetDb db;
  db.set_importer(ed::assetdb::import_asset);
  db.open(claim.string());
  db.scan();
  const ed::assetdb::AssetDb::Entry* hero = nullptr;
  for (const auto& e : db.entries()) {
    if (e.rel.size() >= 10 && e.rel.find("hero.gltf") != std::string::npos) hero = &e;
  }
  if (!hero) return fail("imported model is not in the asset database");
  if (!hero->import_ok) return fail(hero->import_error.empty() ? "import marked failed" : hero->import_error);
  if (hero->meta.info.num("triangles", 0) < 2000000) return fail("triangle count");
  if (hero->meta.info.num("bones", 0) < 128) return fail("bone count");
  if (!hero->meta.info.flag("skinned", false)) return fail("model was not skinned");
  const std::string warn = hero->meta.info.str("warning");
  if (warn.find("downscaled") == std::string::npos) return fail("4K textures were not downscaled: " + warn);
  if (!hero->meta.info.has("fitted")) return fail("no fitted collider was cached");
  const auto& fitted = hero->meta.info["fitted"];
  if (!fitted.is_object() || !fitted["convex"].is_array() || fitted["convex"].size() < 1) {
    return fail("fitted collider has no hull");
  }
  if (fitted["convex"][0].size() > 64) return fail("fitted hull was not limited");

  const std::uint64_t after = peak_bytes();
  if (before && after > before && after - before > 1200ull * 1024ull * 1024ull) {
    return fail("peak memory grew by more than 1200 MB");
  }

  // Cancel mid-import. The same file is large enough that this happens
  // before it finishes.
  {
    std::atomic<bool> stop{false};
    r3::ImportMonitor mon;
    mon.user = &stop;
    mon.cancelled = [](void* p) { return static_cast<std::atomic<bool>*>(p)->load(); };
    mon.progress = [](void* p, int pct, const char*) {
      if (pct >= 30) static_cast<std::atomic<bool>*>(p)->store(true);
    };
    r3::set_import_monitor(&mon);
    r3::Model m;
    std::string err;
    const auto t0 = Clock::now();
    const bool ok = r3::load_model(gltf, &m, &err);
    r3::set_import_monitor(nullptr);
    if (ok) return fail("cancel did not stop the import");
    if (err.find("cancel") == std::string::npos && err.find("Cancel") == std::string::npos) {
      return fail("cancel error was: " + err);
    }
    if (ms_since(t0) > 60000.0) return fail("cancel took too long to stop the import");
  }

  fs::remove_all(root, ec);
  std::cout << "[smoke] bigmodel: OK\n";
  return 0;
}
