#include "Smoke3D.h"

#include "core/PngWrite.h"
#include "render3d/HdrImage.h"
#include "render3d/Camera3D.h"
#include "render3d/Mesh.h"
#include "render3d/Renderer3D.h"
#include "render3d/RhiRenderer3D.h"
#include "render3d/lighting/Lighting.h"
#include "rhi/Rhi.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke-lighting] FAILED: " << what << '\n';
  return 1;
}

bool lit_enough(const std::vector<unsigned char>& px) {
  if (px.size() < 16) return false;
  double sum = 0;
  const std::size_t n = px.size() / 4;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char* p = px.data() + i * 4;
    sum += (p[0] * 3 + p[1] * 6 + p[2]) / 10.0;
  }
  return sum / static_cast<double>(n) > 8.0;
}

int render_one(ts::tombstone::rhi::Device* device, ts::tombstone::render3d::Renderer3D& renderer,
               const ts::tombstone::render3d::MeshHandle& cube, const ts::tombstone::render3d::MeshHandle& plane,
               const ts::tombstone::render3d::MaterialHandle& copper, const ts::tombstone::render3d::MaterialHandle& ground,
               const ts::tombstone::render3d::Lighting& lighting, const std::string& tag, const fs::path& dump) {
  using namespace ts::tombstone::render3d;
  Camera3D cam;
  cam.position = {3.2f, 2.1f, 4.0f};
  cam.target = {0, 0.4f, 0};
  Environment env;
  env.lighting = lighting;
  if (lighting.volumetric) env.fog_density = 0.015f;
  PointLight lamp;
  lamp.position = {1.4f, 2.2f, 1.1f};
  lamp.intensity = 5.0f;
  lamp.range = 8.0f;
  lamp.shadows = true;
  SpotLight spot;
  spot.position = {-1.2f, 2.4f, 0.4f};
  spot.direction = {0.4f, -1.0f, -0.2f};
  spot.shadows = true;
  AreaLight area;
  area.position = {0.2f, 2.8f, -0.6f};
  renderer.set_environment(env);
  renderer.begin_frame(cam, 160, 90);
  renderer.draw(plane, ground, {});
  Transform box;
  box.position = {0, 0.5f, 0};
  renderer.draw(cube, copper, box);
  renderer.add_point_light(lamp);
  renderer.add_spot_light(spot);
  renderer.add_area_light(area);
  const FrameStats st = renderer.end_frame();
  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!renderer.read_pixels(&px, &w, &h) || !lit_enough(px)) {
    return fail(tag + " readback was empty or black (" + std::to_string(st.draws) + " draws)");
  }
  if (st.draws < 1) return fail(tag + " drew nothing");
  if (device && device->api() == ts::tombstone::rhi::Api::D3D12 && device->debug_error_count() > 0) {
    return fail(tag + " debug layer reported " + std::to_string(device->debug_error_count()) + " error(s)");
  }
  fs::create_directories(dump);
  const std::string path = (dump / (tag + ".png")).string();
  if (!ts::tombstone::write_png_rgba(path, w, h, px)) return fail(tag + " screenshot failed");
  std::cout << "[smoke-lighting] " << tag << " " << w << "x" << h << " draws " << st.draws << " -> " << path << "\n";
  return 0;
}

int open_one(ts::tombstone::rhi::Api api, bool required) {
  using namespace ts::tombstone;
  using namespace ts::tombstone::render3d;
  const char* name = rhi::api_name(api);
  if (!rhi::api_built(api)) {
    std::cout << "[smoke-lighting] " << name << " is not in this build\n";
    return required ? 1 : 0;
  }
  rhi::DeviceDesc desc;
  desc.api = api;
  desc.offscreen = true;
  desc.visible = false;
  desc.width = 160;
  desc.height = 90;
  std::string err;
  auto device = rhi::create_device(desc, &err);
  if (!device) {
    if (required || std::getenv("TS_SMOKE_REQUIRE_GL")) return fail(std::string(name) + " did not start: " + err);
    std::cout << "[smoke-lighting] skip " << name << " (" << err << ")\n";
    return 0;
  }
  if (device->api() != api) return fail(std::string(name) + " started as " + rhi::api_name(device->api()));
  auto renderer = make_rhi_renderer(device.get());
  if (!renderer->init(&err)) return fail(std::string(name) + " renderer: " + err);
  renderer->set_offscreen(true);
  MeshData cube_m = make_cube(1.0f);
  MeshData plane_m = make_plane(8.0f);
  auto cube = renderer->create_mesh(cube_m);
  auto plane = renderer->create_mesh(plane_m);
  MaterialDesc copper;
  copper.base_color[0] = 0.72f;
  copper.base_color[1] = 0.38f;
  copper.base_color[2] = 0.22f;
  copper.base_color[3] = 1;
  copper.roughness = 0.42f;
  copper.metallic = 0.15f;
  MaterialDesc ground;
  ground.base_color[0] = 0.55f;
  ground.base_color[1] = 0.52f;
  ground.base_color[2] = 0.46f;
  ground.base_color[3] = 1;
  ground.roughness = 0.9f;
  auto copper_h = renderer->create_material(copper);
  auto ground_h = renderer->create_material(ground);
  const fs::path dump = fs::path("smoke-dump") / rhi::api_flag(api);
  const std::string who = std::string(name) + " \"" + device->adapter_name() + "\" raytracing " +
                          (device->raytracing() ? "yes" : "no");
  std::cout << "[smoke-lighting] " << who << "\n";
  int code = render_one(device.get(), *renderer, cube, plane, copper_h, ground_h, {}, std::string(rhi::api_flag(api)) + "-classic", dump);
  for (int q = 1; q <= 5 && code == 0; ++q) {
    Lighting L;
    apply_lighting_quality(&L, q, device->raytracing());
    code = render_one(device.get(), *renderer, cube, plane, copper_h, ground_h, L,
                      std::string(rhi::api_flag(api)) + "-q" + std::to_string(q), dump);
  }
  for (const auto& mod : lighting_modules()) {
    if (code != 0) break;
    Lighting L;
    L.preset = 4;
    mod->set_enabled(&L, true);
    if (std::string(mod->id()) == "raytracing" && !device->raytracing()) {
      std::cout << "[smoke-lighting] " << name << " raytracing module stays on the raster path\n";
    }
    code = render_one(device.get(), *renderer, cube, plane, copper_h, ground_h, L,
                      std::string(rhi::api_flag(api)) + "-" + mod->id(), dump);
  }
  {
    render3d::ReflectionProbeDesc probe;
    probe.position = {0.0f, 1.2f, 0.0f};
    probe.radius = 8.0f;
    probe.resolution = 16;
    probe.id = 1;
    renderer->set_reflection_probes(&probe, 1);
    render3d::GiVolumeDesc volume;
    volume.origin = {-2.0f, 0.0f, -2.0f};
    volume.spacing = {2.0f, 2.0f, 2.0f};
    volume.nx = volume.ny = volume.nz = 1;
    volume.bake = true;
    volume.id = 7;
    renderer->set_gi_volume(volume);
    render3d::Lighting lit;
    lit.preset = 4;
    lit.reflections = 1;
    lit.gi = 1;
    lit.point_shadows = true;
    lit.spot_shadows = true;
    code = render_one(device.get(), *renderer, cube, plane, copper_h, ground_h, lit,
                      std::string(rhi::api_flag(api)) + "-probes", dump);
    std::vector<float> sh;
    if (code == 0 && (!renderer->take_gi_bake(&sh) || sh.size() < 12))
      code = fail(std::string(name) + " irradiance bake produced no coefficients");
    else if (code == 0)
      std::cout << "[smoke-lighting] " << name << " baked " << (sh.size() / 12) << " irradiance probes\n";
  }
  renderer->shutdown();
  device.reset();
  if (code == 0) std::cout << "[smoke-lighting] " << name << " modules ok\n";
  return code;
}

}  // namespace

int check_images() {
  const fs::path dir = "smoke-dump";
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "tiny.hdr", std::ios::binary);
    out << "#?RADIANCE\nFORMAT=32-bit_rgbe\n\n-Y 1 +X 4\n";
    const unsigned char px[] = {10, 20, 30, 128, 40, 50, 60, 129, 1, 2, 3, 140, 255, 255, 255, 150};
    out.write(reinterpret_cast<const char*>(px), sizeof(px));
  }
  ts::tombstone::render3d::Image img;
  float range = 0;
  std::string err;
  if (!ts::tombstone::render3d::load_hdr_rgba((dir / "tiny.hdr").string(), &img, &range, &err) || img.width != 4 ||
      range < 1.0f)
    return fail("Radiance HDR: " + err);
  {
    // Uncompressed one-pixel OpenEXR, half RGB = 1.
    std::ofstream out(dir / "tiny.exr", std::ios::binary);
    auto u32 = [&](std::uint32_t v) {
      unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                            static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
      out.write(reinterpret_cast<char*>(b), 4);
    };
    auto u64 = [&](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        char c = static_cast<char>((v >> (8 * i)) & 0xff);
        out.write(&c, 1);
      }
    };
    auto str = [&](const char* s) { out.write(s, std::strlen(s) + 1); };
    const unsigned char magic[] = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};
    out.write(reinterpret_cast<const char*>(magic), 8);
    str("channels");
    u32(1);  // chlist
    std::string chans;
    auto add = [&](const char* n) {
      chans.append(n);
      chans.push_back(0);
      chans.push_back(1);  // half
      chans.push_back(0);
      chans.push_back(0);
      chans.push_back(0);
      chans.append(4, 0);  // xSampling
      chans.push_back(1);
      chans.append(3, 0);
      chans.push_back(1);  // ySampling
      chans.append(3, 0);
    };
    add("B");
    add("G");
    add("R");
    chans.push_back(0);
    u32(static_cast<std::uint32_t>(chans.size()));
    out.write(chans.data(), chans.size());
    str("compression");
    u32(0);
    u32(1);
    out.put(0);
    str("dataWindow");
    u32(3);
    u32(16);
    for (int i = 0; i < 3; ++i) u32(0);
    u32(0);  // x1,y1 = 0 so 1x1. Wait I wrote 3 zeros then one more. That's x0 y0 x1. Need y1.
    // The loop wrote 3 zeros and then u32(0) is the 4th. That's 16 bytes. Good, all zero = 1x1.
    str("type");
    u32(1);
    const char* scan = "scanlineimage";
    u32(static_cast<std::uint32_t>(std::strlen(scan) + 1));
    out.write(scan, std::strlen(scan) + 1);
    out.put(0);  // end of header
    const auto table = static_cast<std::uint64_t>(out.tellp()) + 8;
    u64(table);
    u32(0);  // y
    u32(6);  // 3 channels * 2 bytes
    const unsigned char half_one[] = {0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c};
    out.write(reinterpret_cast<const char*>(half_one), 6);
  }
  if (!ts::tombstone::render3d::load_hdr_rgba((dir / "tiny.exr").string(), &img, &range, &err) || img.width != 1)
    return fail("OpenEXR: " + err);
  std::cout << "[smoke-lighting] HDR and EXR decode ok\n";
  return 0;
}

int run_lighting_smoke() {
  if (int code = check_images()) return code;
  using ts::tombstone::rhi::Api;
  Api only = Api::OpenGL;
  const bool pinned = ts::tombstone::rhi::cli_api(&only);
  int code = 0;
  if (!pinned || only == Api::OpenGL) code |= open_one(Api::OpenGL, pinned);
  if (!pinned || only == Api::Vulkan) code |= open_one(Api::Vulkan, pinned);
  if (!pinned || only == Api::D3D12) code |= open_one(Api::D3D12, pinned);
  if (code == 0) std::cout << "[smoke-lighting] OK\n";
  return code == 0 ? 0 : 1;
}
