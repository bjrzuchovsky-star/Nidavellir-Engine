#include "Smoke3D.h"

#include "core/PngWrite.h"
#include "render3d/Camera3D.h"
#include "render3d/Mesh.h"
#include "render3d/Renderer3D.h"
#include "render3d/RhiRenderer3D.h"
#include "render3d/lighting/Lighting.h"
#include "rhi/Rhi.h"

#include <cstdlib>
#include <filesystem>
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
  renderer->shutdown();
  device.reset();
  if (code == 0) std::cout << "[smoke-lighting] " << name << " modules ok\n";
  return code;
}

}  // namespace

int run_lighting_smoke() {
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
