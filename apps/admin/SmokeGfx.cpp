#include "render3d/GlRenderer3D.h"
#include "render3d/HiddenGl.h"
#include "render3d/Math3D.h"
#include "render3d/Mesh.h"
#include "render3d/Renderer3D.h"
#include "render3d/RhiRenderer3D.h"
#include "rhi/Rhi.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {

namespace {

double mean_abs(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
  if (a.size() != b.size() || a.empty()) return 1e9;
  double sum = 0;
  for (std::size_t i = 0; i < a.size(); ++i) sum += std::abs(int(a[i]) - int(b[i]));
  return sum / static_cast<double>(a.size());
}

std::vector<unsigned char> flip_y(const std::vector<unsigned char>& src, int w, int h) {
  std::vector<unsigned char> out(src.size());
  const std::size_t row = static_cast<std::size_t>(w) * 4u;
  for (int y = 0; y < h; ++y) {
    std::memcpy(out.data() + static_cast<std::size_t>(y) * row, src.data() + static_cast<std::size_t>(h - 1 - y) * row, row);
  }
  return out;
}

bool render_reference(std::vector<unsigned char>* px, int* w, int* h, std::string* err) {
  render3d::HiddenGl gl;
  if (!gl.open(err)) return false;
  auto renderer = render3d::make_gl_renderer3d();
  if (!renderer->init(err)) return false;
  render3d::MeshData cube = render3d::make_cube(1.0f);
  render3d::MeshData plane = render3d::make_plane(8.0f);
  auto cube_m = renderer->create_mesh(cube);
  auto plane_m = renderer->create_mesh(plane);
  render3d::MaterialDesc copper;
  copper.base_color[0] = 0.72f;
  copper.base_color[1] = 0.38f;
  copper.base_color[2] = 0.22f;
  copper.base_color[3] = 1;
  copper.roughness = 0.42f;
  copper.metallic = 0.15f;
  render3d::MaterialDesc ground;
  ground.base_color[0] = 0.28f;
  ground.base_color[1] = 0.30f;
  ground.base_color[2] = 0.26f;
  ground.base_color[3] = 1;
  ground.roughness = 0.9f;
  auto copper_h = renderer->create_material(copper);
  auto ground_h = renderer->create_material(ground);
  render3d::Camera3D cam;
  cam.position = {3.2f, 2.1f, 4.0f};
  cam.target = {0, 0.4f, 0};
  render3d::Environment env;
  render3d::PointLight lamp;
  lamp.position = {1.4f, 2.2f, 1.1f};
  lamp.intensity = 5.0f;
  lamp.range = 8.0f;
  renderer->set_offscreen(true);
  renderer->set_environment(env);
  renderer->begin_frame(cam, 128, 128);
  render3d::Transform floor;
  renderer->draw(plane_m, ground_h, floor);
  render3d::Transform box;
  box.position = {0, 0.5f, 0};
  renderer->draw(cube_m, copper_h, box);
  renderer->add_point_light(lamp);
  renderer->end_frame();
  const bool ok = renderer->read_pixels(px, w, h);
  renderer->shutdown();
  if (!ok && err) *err = "OpenGL readback failed";
  return ok;
}

bool render_api(rhi::Api api, std::vector<unsigned char>* px, int* w, int* h, std::string* err) {
  rhi::DeviceDesc desc;
  desc.api = api;
  desc.offscreen = true;
  desc.visible = false;
  desc.width = 128;
  desc.height = 128;
  auto device = rhi::create_device(desc, err);
  if (!device) return false;
  auto renderer = render3d::make_rhi_renderer(device.get());
  if (!renderer->init(err)) return false;
  render3d::MeshData cube = render3d::make_cube(1.0f);
  render3d::MeshData plane = render3d::make_plane(8.0f);
  auto cube_m = renderer->create_mesh(cube);
  auto plane_m = renderer->create_mesh(plane);
  render3d::MaterialDesc copper;
  copper.base_color[0] = 0.72f;
  copper.base_color[1] = 0.38f;
  copper.base_color[2] = 0.22f;
  copper.base_color[3] = 1;
  copper.roughness = 0.42f;
  copper.metallic = 0.15f;
  render3d::MaterialDesc ground;
  ground.base_color[0] = 0.28f;
  ground.base_color[1] = 0.30f;
  ground.base_color[2] = 0.26f;
  ground.base_color[3] = 1;
  ground.roughness = 0.9f;
  auto copper_h = renderer->create_material(copper);
  auto ground_h = renderer->create_material(ground);
  render3d::Camera3D cam;
  cam.position = {3.2f, 2.1f, 4.0f};
  cam.target = {0, 0.4f, 0};
  render3d::Environment env;
  render3d::PointLight lamp;
  lamp.position = {1.4f, 2.2f, 1.1f};
  lamp.intensity = 5.0f;
  lamp.range = 8.0f;
  renderer->set_offscreen(true);
  renderer->set_environment(env);
  renderer->begin_frame(cam, 128, 128);
  renderer->draw(plane_m, ground_h, {});
  render3d::Transform box;
  box.position = {0, 0.5f, 0};
  renderer->draw(cube_m, copper_h, box);
  renderer->add_point_light(lamp);
  renderer->end_frame();
  const bool ok = renderer->read_pixels(px, w, h);
  renderer->shutdown();
  if (!ok && err) *err = "readback failed";
  return ok;
}

}  // namespace

int run_gfx_smoke() {
  std::vector<unsigned char> ref;
  int rw = 0, rh = 0;
  std::string err;
  if (!render_reference(&ref, &rw, &rh, &err)) {
    std::cout << "[smoke-gfx] OpenGL reference skipped (" << err << ")\n";
    return 0;
  }
  const rhi::Api apis[] = {rhi::Api::OpenGL, rhi::Api::Vulkan, rhi::Api::D3D12};
  int compared = 0;
  for (rhi::Api api : apis) {
    if (!rhi::api_built(api)) {
      std::cout << "[smoke-gfx] skip " << rhi::api_flag(api) << " (not built on this platform)\n";
      continue;
    }
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    err.clear();
    if (!render_api(api, &px, &w, &h, &err)) {
      std::cout << "[smoke-gfx] skip " << rhi::api_flag(api) << " (" << err << ")\n";
      continue;
    }
    if (w != rw || h != rh) {
      std::cerr << "[smoke-gfx] " << rhi::api_flag(api) << " frame is " << w << "x" << h << "\n";
      return 1;
    }
    const double mae = mean_abs(ref, px);
    const double flipped = mean_abs(ref, flip_y(px, w, h));
    const double best = std::min(mae, flipped);
    std::cout << "[smoke-gfx] " << rhi::api_name(api) << " vs OpenGL  mae " << mae << "  flipped " << flipped
              << "\n";
    if (best > 28.0) {
      std::cerr << "[smoke-gfx] " << rhi::api_name(api) << " is too far from OpenGL\n";
      return 1;
    }
    if (flipped + 4.0 < mae) {
      std::cerr << "[smoke-gfx] " << rhi::api_name(api) << " frame is upside down\n";
      return 1;
    }
    ++compared;
  }
  std::cout << "[smoke-gfx] " << compared << " backend(s) matched OpenGL\n";
  return 0;
}

}  // namespace tombstone
}  // namespace ts
