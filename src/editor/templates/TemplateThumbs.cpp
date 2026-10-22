#include "editor/templates/TemplateThumbs.h"

#include "core/PngWrite.h"
#include "editor/ProjectInfo.h"
#include "editor/projects/ProjectStore.h"
#include "editor/templates/TemplateStore.h"
#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "render3d/GlRenderer3D.h"
#include "render3d/Gltf.h"
#include "render3d/HiddenGl.h"
#include "render3d/Renderer3D.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "runtime/Input.h"
#include "runtime/World.h"
#include "scene/SceneJson.h"
#include "store/Json.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {
namespace template_thumbs {

namespace fs = std::filesystem;

namespace {

struct Canvas {
  int w = 0, h = 0;
  std::vector<float> px;  // rgb
  Canvas(int width, int height) : w(width), h(height), px(static_cast<std::size_t>(width) * height * 3) {}
  void blend(int x, int y, const float rgb[3], float a, bool additive) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) return;
    float* d = &px[(static_cast<std::size_t>(y) * w + x) * 3];
    for (int i = 0; i < 3; ++i) {
      d[i] = additive ? std::min(1.0f, d[i] + rgb[i] * a) : d[i] * (1.0f - a) + rgb[i] * a;
    }
  }
  std::vector<unsigned char> rgba() const {
    std::vector<unsigned char> out(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
      for (int c = 0; c < 3; ++c) {
        out[i * 4 + c] = static_cast<unsigned char>(std::clamp(px[i * 3 + c], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    return out;
  }
};

struct Image {
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;
};

class ImageCache {
 public:
  explicit ImageCache(std::string dir) : dir_(std::move(dir)) {}
  const Image* get(const std::string& rel) {
    auto it = cache_.find(rel);
    if (it != cache_.end()) return it->second.w ? &it->second : nullptr;
    Image img;
    const std::string path = runtime::resolve_asset(dir_, rel);
    render3d::Image decoded;
    std::string err;
    if (render3d::load_image_file(path, &decoded, &err) && decoded.valid()) {
      img.w = decoded.width;
      img.h = decoded.height;
      img.rgba = std::move(decoded.rgba);
    }
    auto& slot = cache_[rel] = std::move(img);
    return slot.w ? &slot : nullptr;
  }

 private:
  std::string dir_;
  std::map<std::string, Image> cache_;
};

bool write_png(const std::string& png, int w, int h, const std::vector<unsigned char>& rgba,
               std::string* error) {
  std::error_code ec;
  if (fs::path(png).has_parent_path()) fs::create_directories(fs::path(png).parent_path(), ec);
  if (!write_png_rgba(png, w, h, rgba)) {
    if (error) *error = "cannot write " + png;
    return false;
  }
  return true;
}

// --- 2D: the runtime draw list on a CPU canvas --------------------------------

bool render_2d(const std::string& dir, const std::vector<std::string>& plugins,
               const std::string& png, std::string* error) {
  scene_json::SceneDoc doc;
  std::string err;
  if (!scene_json::load_file(scene_json::scene_path_for_project(dir), &doc, &err)) {
    if (error) *error = err;
    return false;
  }
  // The claim's own plugins draw too (Airship Works ships, Frontier blocks).
  plugin::Catalog catalog;
  plugins::register_builtin_plugins(catalog, nullptr);
  plugin::PluginManager manager(catalog);
  plugin::PluginContext ctx;
  ctx.project_dir = dir;
  ctx.host = "smoke";
  manager.load(plugins, ctx);
  runtime::World world;
  world.set_extensions(manager.world_extensions());
  struct End {
    plugin::PluginManager& m;
    runtime::World& w;
    ~End() {
      if (m.in_play()) m.play_end();
      w.clear();
      w.set_extensions(nullptr);
      m.unload_all();
    }
  } end{manager, world};
  if (!world.build(doc.entities, doc.tile_solidity, dir, &err)) {
    if (error) *error = err;
    return false;
  }
  manager.play_begin(world);
  // A short ride so riders settle (platformers land on their ground).
  runtime::InputFrame idle;
  for (int i = 0; i < 45; ++i) world.step(idle);

  const int W = kWidth, H = kHeight;
  // Fit what is there: centre on player 0 (or the scene), zoom so the
  // content fills the card, keep the view inside the content when it can.
  float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
  bool any = false;
  const runtime::WorldRect cam_frame = runtime::World::view_rect(world.camera(), 640.0f, 360.0f);
  for (const runtime::Actor& a : world.actors()) {
    if (a.data.spawn) continue;
    if (a.data.camera) {
      if (world.player(0)) continue;
      // A camera with nowhere to go: its frame is part of the picture.
      if (!any) {
        bx0 = cam_frame.x0; by0 = cam_frame.y0; bx1 = cam_frame.x1; by1 = cam_frame.y1;
        any = true;
      } else {
        bx0 = std::min(bx0, cam_frame.x0); by0 = std::min(by0, cam_frame.y0);
        bx1 = std::max(bx1, cam_frame.x1); by1 = std::max(by1, cam_frame.y1);
      }
      continue;
    }
    const float x0 = a.data.x, y0 = a.data.y, x1 = a.data.x + a.data.w, y1 = a.data.y + a.data.h;
    if (!any) {
      bx0 = x0; by0 = y0; bx1 = x1; by1 = y1;
      any = true;
    } else {
      bx0 = std::min(bx0, x0); by0 = std::min(by0, y0);
      bx1 = std::max(bx1, x1); by1 = std::max(by1, y1);
    }
  }
  if (!any) {
    bx0 = -W * 0.5f; by0 = -H * 0.5f; bx1 = W * 0.5f; by1 = H * 0.5f;
  }
  const float bw = std::max(1.0f, bx1 - bx0), bh = std::max(1.0f, by1 - by0);
  const runtime::Actor* rider = world.player(0);
  float zoom = std::min(W / bw, H / bh) * 0.94f;
  if (rider) zoom = std::max(zoom, 0.75f);
  zoom = std::clamp(zoom, 0.25f, 2.5f);
  const float vw = W / zoom, vh = H / zoom;
  float cx = rider ? rider->data.center_x() : (bx0 + bx1) * 0.5f;
  float cy = rider ? rider->data.center_y() : (by0 + by1) * 0.5f;
  if (bw > vw) cx = std::clamp(cx, bx0 + vw * 0.5f, bx1 - vw * 0.5f);
  else cx = (bx0 + bx1) * 0.5f;
  if (bh > vh) cy = std::clamp(cy, by0 + vh * 0.5f, by1 - vh * 0.5f);
  else cy = (by0 + by1) * 0.5f;
  const runtime::WorldRect view{cx - vw * 0.5f, cy - vh * 0.5f, cx + vw * 0.5f, cy + vh * 0.5f};

  Canvas c(W, H);
  // Night-sky backdrop (the editor's Dusk charcoal into copper).
  for (int y = 0; y < H; ++y) {
    const float t = static_cast<float>(y) / (H - 1);
    const float top[3] = {0.11f, 0.10f, 0.14f}, low[3] = {0.20f, 0.15f, 0.16f};
    for (int x = 0; x < W; ++x) {
      float* d = &c.px[(static_cast<std::size_t>(y) * W + x) * 3];
      for (int i = 0; i < 3; ++i) d[i] = top[i] + (low[i] - top[i]) * t;
    }
  }
  auto to_px = [&](float wx, float wy, float* sx, float* sy) {
    *sx = (wx - view.x0) * zoom;
    *sy = (wy - view.y0) * zoom;
  };
  // Tile grids (what an empty TileMap looks like in the editor).
  for (const runtime::Actor& a : world.actors()) {
    if (!a.data.tilemap) continue;
    const TileMapData& t = *a.data.tilemap;
    const float grid[3] = {0.55f, 0.50f, 0.58f};
    for (int col = 0; col <= t.cols; ++col) {
      float sx, sy0, sy1, dummy;
      to_px(a.data.x + col * t.tile_size, a.data.y, &sx, &sy0);
      to_px(a.data.x, a.data.y + t.rows * t.tile_size, &dummy, &sy1);
      for (int y = static_cast<int>(sy0); y <= static_cast<int>(sy1); ++y) {
        c.blend(static_cast<int>(sx), y, grid, col == 0 || col == t.cols ? 0.55f : 0.22f, false);
      }
    }
    for (int row = 0; row <= t.rows; ++row) {
      float sy, sx0, sx1, dummy;
      to_px(a.data.x, a.data.y + row * t.tile_size, &sx0, &sy);
      to_px(a.data.x + t.cols * t.tile_size, a.data.y, &sx1, &dummy);
      for (int x = static_cast<int>(sx0); x <= static_cast<int>(sx1); ++x) {
        c.blend(x, static_cast<int>(sy), grid, row == 0 || row == t.rows ? 0.55f : 0.22f, false);
      }
    }
  }
  ImageCache images(dir);
  std::vector<runtime::DrawQuad> quads;
  world.build_draw_list(view, 1.0f,
                        [&](const std::string& rel, int* w, int* h) {
                          const Image* img = images.get(rel);
                          if (!img) return false;
                          *w = img->w;
                          *h = img->h;
                          return true;
                        },
                        &quads);
  for (const runtime::DrawQuad& q : quads) {
    if (q.blend == 2) continue;  // lightmap (lighting is off in thumbnails)
    float sx0, sy0, sx1, sy1;
    to_px(q.x0, q.y0, &sx0, &sy0);
    to_px(q.x1, q.y1, &sx1, &sy1);
    const Image* img = q.image && !q.missing ? images.get(*q.image) : nullptr;
    const int ix0 = std::max(0, static_cast<int>(std::floor(std::min(sx0, sx1))));
    const int ix1 = std::min(W, static_cast<int>(std::ceil(std::max(sx0, sx1))));
    const int iy0 = std::max(0, static_cast<int>(std::floor(std::min(sy0, sy1))));
    const int iy1 = std::min(H, static_cast<int>(std::ceil(std::max(sy0, sy1))));
    for (int y = iy0; y < iy1; ++y) {
      for (int x = ix0; x < ix1; ++x) {
        float rgb[3] = {q.rgba[0], q.rgba[1], q.rgba[2]};
        float a = q.rgba[3];
        if (img) {
          const float u = q.u0 + (q.u1 - q.u0) * ((x + 0.5f - sx0) / std::max(1e-3f, sx1 - sx0));
          const float v = q.v0 + (q.v1 - q.v0) * ((y + 0.5f - sy0) / std::max(1e-3f, sy1 - sy0));
          const int tx = std::clamp(static_cast<int>(u * img->w), 0, img->w - 1);
          const int ty = std::clamp(static_cast<int>(v * img->h), 0, img->h - 1);
          const unsigned char* s = &img->rgba[(static_cast<std::size_t>(ty) * img->w + tx) * 4];
          for (int i = 0; i < 3; ++i) rgb[i] *= s[i] / 255.0f;
          a *= s[3] / 255.0f;
        }
        c.blend(x, y, rgb, a, q.blend == 1);
      }
    }
  }
  // The play camera's frame when it has nowhere to go (camera-only claims).
  if (!rider) {
    for (const runtime::Actor& a : world.actors()) {
      if (!a.data.camera) continue;
      const runtime::WorldRect r = cam_frame;
      float sx0, sy0, sx1, sy1;
      to_px(r.x0, r.y0, &sx0, &sy0);
      to_px(r.x1, r.y1, &sx1, &sy1);
      const float amber[3] = {0.95f, 0.75f, 0.25f};
      for (int x = static_cast<int>(sx0); x <= static_cast<int>(sx1); ++x) {
        for (int k = 0; k < 2; ++k) {
          c.blend(x, static_cast<int>(sy0) + k, amber, 0.8f, false);
          c.blend(x, static_cast<int>(sy1) - k, amber, 0.8f, false);
        }
      }
      for (int y = static_cast<int>(sy0); y <= static_cast<int>(sy1); ++y) {
        for (int k = 0; k < 2; ++k) {
          c.blend(static_cast<int>(sx0) + k, y, amber, 0.8f, false);
          c.blend(static_cast<int>(sx1) - k, y, amber, 0.8f, false);
        }
      }
      break;
    }
  }
  return write_png(png, W, H, c.rgba(), error);
}

// --- 3D: the GL renderer, offscreen -------------------------------------------

bool render_3d(const std::string& dir, const std::string& png, std::string* error) {
  namespace r3 = render3d;
  scene3d::Scene3D s;
  std::string err;
  if (!scene3d::load_file(scene3d::scene_path(dir), &s, &err)) {
    if (error) *error = err;
    return false;
  }
  r3::HiddenGl ctx;
  if (!ctx.open(&err)) {
    if (error) *error = "GL context: " + err;
    return false;
  }
  auto renderer = r3::make_gl_renderer3d();
  if (!renderer->init(&err)) {
    if (error) *error = "GL renderer: " + err;
    return false;
  }
  renderer->set_offscreen(true);
  // Show the VR rig's hands (they are not entities in the scene).
  std::vector<scene3d::Entity3D> ents = s.entities;
  for (const scene3d::Entity3D& e : s.entities) {
    if (!e.vr_rig || e.vr_rig->hand_model.empty()) continue;
    for (int side = 0; side < 2; ++side) {
      scene3d::Entity3D hand;
      hand.id = s.next_id() + 100 + side;
      hand.name = side ? "Right Hand" : "Left Hand";
      hand.mesh = scene3d::MeshRef{e.vr_rig->hand_model, true};
      // Palm down, fingers ahead (-Z), reaching out a little apart.
      hand.transform.position = e.transform.position + r3::Vec3{side ? 0.16f : -0.16f, 1.15f, -0.3f};
      hand.transform.rotation = r3::Quat::from_euler_deg({15.0f, side ? -12.0f : 12.0f, side ? -20.0f : 20.0f});
      hand.transform.scale = {side ? 2.0f : -2.0f, 2.0f, 2.0f};
      ents.push_back(hand);
    }
  }
  // The editor camera; a near-empty scene gets it lowered and pulled in so
  // the sky shows, and a VR rig is framed on its hands.
  const scene3d::EditorCamera3D& ec = s.editor_camera;
  const bool sparse = s.entities.size() <= 6;
  float yaw = r3::radians(ec.yaw);
  float pitch = r3::radians(sparse ? std::min(ec.pitch, 16.0f) : ec.pitch);
  float d = sparse ? std::min(ec.distance, 10.0f) : ec.distance;
  r3::Vec3 target = ec.target + (sparse ? r3::Vec3{0.0f, 0.6f, 0.0f} : r3::Vec3{});
  for (const scene3d::Entity3D& e : s.entities) {
    if (sparse && e.vr_rig && !e.vr_rig->hand_model.empty()) {
      target = e.transform.position + r3::Vec3{0.0f, 1.12f, -0.42f};
      d = 1.0f;
      pitch = r3::radians(30.0f);
      yaw = 0.0f;
    }
  }
  r3::Camera3D cam;
  cam.target = target;
  cam.position = cam.target + r3::Vec3{std::sin(yaw) * std::cos(pitch) * d, std::sin(pitch) * d,
                                       std::cos(yaw) * std::cos(pitch) * d};
  cam.fov_y_degrees = 55.0f;
  cam.z_far = 400.0f;
  r3::SceneView view(renderer.get());
  view.set_project_dir(dir);
  const int W = kWidth * 2, H = kHeight * 2;  // 2x, box-filtered down
  for (int frame = 0; frame < 3; ++frame) {
    view.set_time(1.0f + frame * 0.016f);
    view.begin(s.environment, ents, cam, W, H);
    view.end();
  }
  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!renderer->read_pixels(&px, &w, &h) || w != W || h != H) {
    if (error) *error = "read_pixels failed";
    return false;
  }
  std::vector<unsigned char> out(static_cast<std::size_t>(kWidth) * kHeight * 4, 255);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      for (int c = 0; c < 3; ++c) {
        int sum = 0;
        for (int k = 0; k < 4; ++k) {
          const int sx = x * 2 + (k & 1);
          const int sy = (H - 1) - (y * 2 + (k >> 1));  // GL rows are bottom-up
          sum += px[(static_cast<std::size_t>(sy) * W + sx) * 4 + c];
        }
        out[(static_cast<std::size_t>(y) * kWidth + x) * 4 + c] = static_cast<unsigned char>(sum / 4);
      }
    }
  }
  return write_png(png, kWidth, kHeight, out, error);
}

}  // namespace

bool render_project(const std::string& project_dir, const std::string& png, std::string* error) {
  ProjectInfo info;
  std::string err;
  if (!ProjectStore::load_project_json(project_dir, &info, &err)) {
    if (error) *error = err;
    return false;
  }
  return info.kind == ProjectKind::TwoD
             ? render_2d(project_dir, info.plugins_set ? info.plugins : std::vector<std::string>{},
                         png, error)
             : render_3d(project_dir, png, error);
}

int bake(const std::string& templates_root, std::vector<std::string>* errors) {
  int made = 0;
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(templates_root, ec)) {
    if (e.is_directory(ec) && fs::exists(e.path() / "template.json", ec)) dirs.push_back(e.path());
  }
  std::sort(dirs.begin(), dirs.end());
  for (const fs::path& d : dirs) {
    ProjectTemplate t;
    std::string err;
    if (!TemplateStore::load_template(d.string(), false, &t, &err)) {
      if (errors) errors->push_back(err);
      continue;
    }
    std::string src = t.project_dir;
    fs::path tmp;
    if (src.empty()) {
      if (t.generator.empty()) continue;  // a blank claim: the painted card
      tmp = fs::temp_directory_path() / ("ts_thumb_" + t.id);
      fs::remove_all(tmp, ec);
      if (!TemplateStore::run_generator(t.generator, tmp.string(), t.name, &err)) {
        if (errors) errors->push_back(t.id + ": " + err);
        continue;
      }
      src = tmp.string();
    }
    const fs::path png = d / "thumbnail.png";
    if (!render_project(src, png.string(), &err)) {
      if (errors) errors->push_back(t.id + ": " + err);
    } else {
      ++made;
    }
    if (!tmp.empty()) fs::remove_all(tmp, ec);
  }
  return made;
}

}  // namespace template_thumbs
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
