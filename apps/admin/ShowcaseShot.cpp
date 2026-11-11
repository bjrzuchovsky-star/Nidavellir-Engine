// ts_admin --showcase-shot <scene> <out.png>
// Store captures: the Forge editor around a populated scene. Respects
// --shot-size and --shot-clean. Does not touch the smoke pixel sizes.
#include "core/PngWrite.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/scripting/BrandBook.h"
#include "editor/settings/Settings.h"
#include "editor/ui/Theme.h"
#include "gfx/GlTextureUploader.h"
#include "editor/ux/ShotOpts.h"
#include "fx/Particles.h"
#include "plugins/survival/Sample.h"
#include "render3d/Gltf.h"
#include "render3d/Mesh.h"
#include "render3d/Scene3D.h"
#include "terrain/Foliage.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainView.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;
// ts::fx, not a nested name under ts::tombstone (MSVC would bind fx:: there).
namespace fx = ts::fx;

namespace {

void shot_window_px(int fallback_w, int fallback_h, int* w, int* h) {
  editor::shot::resolve(fallback_w, fallback_h, w, h);
  if (editor::shot::size_set()) {
#ifdef GLFW_COCOA_RETINA_FRAMEBUFFER
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
#endif
  }
}

void apply_forge() {
  editor::Settings defaults;
  defaults.theme = "forge";
  editor::theme::Apply(defaults.theme);
}

int save_png(const std::string& png, int fw, int fh, const std::vector<unsigned char>& px) {
  if (px.empty() || fw <= 0 || fh <= 0) return 1;
  std::vector<unsigned char> flipped(px.size());
  for (int y = 0; y < fh; ++y) {
    std::memcpy(&flipped[static_cast<std::size_t>(y) * fw * 4],
                &px[static_cast<std::size_t>(fh - 1 - y) * fw * 4], static_cast<std::size_t>(fw) * 4);
  }
  for (std::size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
  if (!write_png_rgba(png, fw, fh, flipped)) {
    std::cerr << "cannot write " << png << '\n';
    return 1;
  }
  return 0;
}

struct GlWindow {
  GLFWwindow* window = nullptr;
  bool started = false;
  ~GlWindow() {
    if (!started) return;
    if (ImGui::GetCurrentContext()) {
      ImGui_ImplOpenGL3_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImGui::DestroyContext();
    }
    gfx::set_texture_uploader(nullptr);
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
  }
};

bool open_gl(GlWindow* out, int def_w, int def_h, const char* title) {
  if (!glfwInit()) {
    std::cerr << "glfwInit failed\n";
    return false;
  }
  out->started = true;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  int W = 0, H = 0;
  shot_window_px(def_w, def_h, &W, &H);
  out->window = glfwCreateWindow(W, H, title, nullptr, nullptr);
  if (!out->window) {
    std::cerr << "glfwCreateWindow failed\n";
    return false;
  }
  glfwMakeContextCurrent(out->window);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  ImGui_ImplGlfw_InitForOpenGL(out->window, true);
  ImGui_ImplOpenGL3_Init("#version 330");
  static gfx::GlTextureUploader gl_uploader;
  gfx::set_texture_uploader(&gl_uploader);
  apply_forge();
  return true;
}

editor::ProjectInfo project_at(const fs::path& dir, const std::string& id, const std::string& name,
                               editor::ProjectKind kind, bool vr) {
  editor::ProjectInfo info;
  info.id = id;
  info.name = name;
  info.kind = kind;
  info.vr = vr;
  info.path = dir.string();
  return info;
}

void dress_town(editor::Editor3DScreen& e) {
  for (const scene3d::Entity3D& ent : e.scene().entities) {
    if (ent.light && ent.name.rfind("Lantern", 0) == 0) {
      scene3d::Entity3D copy = ent;
      copy.light->intensity = 14.0f;
      copy.light->range = 16.0f;
      copy.light->color[0] = 1.0f;
      copy.light->color[1] = 0.62f;
      copy.light->color[2] = 0.28f;
      e.replace_entity(copy, "Warm lantern");
      continue;
    }
    if (!ent.material || !ent.mesh || ent.mesh->source.rfind("primitive:", 0) != 0) continue;
    const bool cloth = ent.name == "Ground" || ent.name.find("Wall") != std::string::npos ||
                       ent.name.find("Saloon") != std::string::npos || ent.name.find("Bank") != std::string::npos ||
                       ent.name.find("Jail") != std::string::npos || ent.name.find("Store") != std::string::npos ||
                       ent.name.find("Tower") != std::string::npos || ent.name.find("Board") != std::string::npos ||
                       ent.name.find("Windmill") != std::string::npos;
    if (!cloth || ent.material->albedo_texture == "assets/boards.tga") continue;
    scene3d::Entity3D copy = ent;
    copy.material->albedo_texture = "assets/boards.tga";
    e.replace_entity(copy, "Boards on " + ent.name);
  }
}

bool write_boards_tga(const fs::path& path) {
  render3d::Image boards;
  boards.width = boards.height = 64;
  boards.rgba.assign(static_cast<std::size_t>(64 * 64 * 4), 255);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const bool seam = (y % 16) == 0 || ((x + (y / 16) * 21) % 64) == 0;
      const int grain = ((x * 7 + y * 3) % 11) * 3;
      unsigned char* px = &boards.rgba[(static_cast<std::size_t>(y) * 64 + x) * 4];
      px[0] = static_cast<unsigned char>(seam ? 70 : 168 + grain);
      px[1] = static_cast<unsigned char>(seam ? 46 : 118 + grain);
      px[2] = static_cast<unsigned char>(seam ? 28 : 72 + grain / 2);
    }
  }
  std::string bytes(18, '\0');
  bytes[2] = 2;
  bytes[12] = 64;
  bytes[14] = 64;
  bytes[16] = 32;
  bytes[17] = 0x28;
  for (std::size_t i = 0; i + 3 < boards.rgba.size(); i += 4) {
    bytes.push_back(static_cast<char>(boards.rgba[i + 2]));
    bytes.push_back(static_cast<char>(boards.rgba[i + 1]));
    bytes.push_back(static_cast<char>(boards.rgba[i + 0]));
    bytes.push_back(static_cast<char>(boards.rgba[i + 3]));
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
  return static_cast<bool>(out);
}

void ensure_prop_assets(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir / "assets", ec);
  if (!fs::exists(dir / "assets" / "boards.tga", ec)) write_boards_tga(dir / "assets" / "boards.tga");
  if (!fs::exists(dir / "assets" / "lantern.gltf", ec)) {
    std::string err;
    render3d::write_sample_lantern_gltf((dir / "assets" / "lantern.gltf").string(), &err);
  }
}

void paint_prop(editor::Editor3DScreen& e, std::uint64_t id, const std::string& name,
                const render3d::Vec3& scale, const float rgb[3], const std::string& tex, float rough,
                float metal, const float* emissive) {
  const scene3d::Entity3D* cur = e.find(id);
  if (!cur) return;
  scene3d::Entity3D copy = *cur;
  copy.name = name;
  copy.transform.scale = scale;
  if (!copy.material) copy.material = scene3d::Material3D{};
  copy.material->albedo[0] = rgb[0];
  copy.material->albedo[1] = rgb[1];
  copy.material->albedo[2] = rgb[2];
  copy.material->roughness = rough;
  copy.material->metallic = metal;
  if (!tex.empty()) copy.material->albedo_texture = tex;
  if (emissive) {
    copy.material->emissive[0] = emissive[0];
    copy.material->emissive[1] = emissive[1];
    copy.material->emissive[2] = emissive[2];
  }
  e.replace_entity(copy, name);
}

void place_lantern(editor::Editor3DScreen& e, const render3d::Vec3& pos, const std::string& name) {
  const std::uint64_t id = e.create_empty(pos);
  const scene3d::Entity3D* cur = e.find(id);
  if (!cur) return;
  scene3d::Entity3D copy = *cur;
  copy.name = name;
  copy.mesh = scene3d::MeshRef{"assets/lantern.gltf", true};
  scene3d::Light3D light;
  light.intensity = 18.0f;
  light.range = 14.0f;
  light.color[0] = 1.0f;
  light.color[1] = 0.58f;
  light.color[2] = 0.22f;
  copy.light = light;
  e.replace_entity(copy, name);
}

// Crates, barrels, a short fence and lanterns, packed around `at` so the
// shot is timber and iron rather than the distant grey boxes.
void fill_street(editor::Editor3DScreen& e, const render3d::Vec3& at) {
  const float wood[3] = {0.62f, 0.40f, 0.22f};
  const float dark[3] = {0.34f, 0.20f, 0.11f};
  const char* tex = "assets/boards.tga";
  auto box = [&](const char* name, render3d::Vec3 pos, render3d::Vec3 scale, const float* rgb) {
    paint_prop(e, e.create_primitive("cube", pos), name, scale, rgb, tex, 0.75f, 0.0f, nullptr);
  };
  auto barrel = [&](const char* name, render3d::Vec3 pos) {
    paint_prop(e, e.create_primitive("capsule", pos), name, {0.46f, 0.62f, 0.46f}, dark, tex, 0.65f, 0.08f,
               nullptr);
  };
  box("Crate", at + render3d::Vec3{-1.3f, 0.38f, 0.2f}, {0.75f, 0.75f, 0.75f}, wood);
  box("Crate Stack", at + render3d::Vec3{-1.15f, 1.05f, 0.15f}, {0.55f, 0.55f, 0.55f}, wood);
  box("Supply Crate", at + render3d::Vec3{1.15f, 0.32f, 0.55f}, {0.64f, 0.64f, 0.64f}, dark);
  barrel("Barrel", at + render3d::Vec3{0.15f, 0.45f, 1.05f});
  barrel("Barrel 2", at + render3d::Vec3{0.7f, 0.45f, 0.85f});
  for (int i = 0; i < 5; ++i) {
    const float x = -1.6f + static_cast<float>(i) * 0.8f;
    box("Fence Post", at + render3d::Vec3{x, 0.55f, -1.15f}, {0.12f, 1.1f, 0.12f}, dark);
  }
  box("Fence Rail", at + render3d::Vec3{0.0f, 0.85f, -1.15f}, {3.4f, 0.1f, 0.08f}, wood);
  box("Fence Rail Low", at + render3d::Vec3{0.0f, 0.4f, -1.15f}, {3.4f, 0.1f, 0.08f}, wood);
  place_lantern(e, at + render3d::Vec3{-1.8f, 0.0f, -0.2f}, "Porch Lantern");
  place_lantern(e, at + render3d::Vec3{1.7f, 0.0f, 0.1f}, "Street Lantern");
}

void use_wanderer_png(const fs::path& dir, editor::Editor2DScreen* live) {
  std::error_code ec;
  const fs::path src = dir / "assets" / "rider.png";
  const fs::path dst = dir / "assets" / "wanderer.png";
  if (fs::exists(src, ec)) fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  auto rewrite = [](const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string from = "rider.png";
    const std::string to = "wanderer.png";
    std::string out;
    out.reserve(text.size());
    bool changed = false;
    for (std::size_t i = 0; i < text.size();) {
      if (text.compare(i, from.size(), from) == 0) {
        out += to;
        i += from.size();
        changed = true;
      } else {
        out.push_back(text[i++]);
      }
    }
    if (!changed) return;
    std::ofstream file(p, std::ios::binary | std::ios::trunc);
    file << out;
  };
  rewrite(dir / "scene.json");
  rewrite(dir / "assets" / "rider.anim.json");
  if (!live) return;
  for (Entity2D& ent : live->workspace().entities()) {
    if (ent.sprite && ent.sprite->path.find("rider.png") != std::string::npos) ent.sprite->path = "assets/wanderer.png";
  }
}

void haze(editor::Editor3DScreen& e) {
  render3d::Environment env = e.scene().environment;
  env.sky_top[0] = 0.28f;
  env.sky_top[1] = 0.42f;
  env.sky_top[2] = 0.62f;
  env.sky_horizon[0] = 0.86f;
  env.sky_horizon[1] = 0.78f;
  env.sky_horizon[2] = 0.62f;
  env.sun_direction = render3d::normalize(render3d::Vec3{-0.55f, -0.5f, 0.45f});
  env.sun_intensity = 2.6f;
  env.ambient = 0.4f;
  env.shadows = true;
  env.shadow_distance = 80.0f;
  env.fog_color[0] = 0.78f;
  env.fog_color[1] = 0.78f;
  env.fog_color[2] = 0.72f;
  env.fog_density = 0.01f;
  env.fog_height = 8.0f;
  env.fog_falloff = 0.04f;
  env.wind_strength = 0.4f;
  env.wind_gust = 0.35f;
  e.set_environment(env, "Forest haze");
}

void plant_forest(editor::Editor3DScreen& e, std::uint64_t id) {
  const scene3d::Entity3D* ent = e.find(id);
  if (!ent || !ent->terrain) return;
  std::shared_ptr<terrain::TerrainData> d = terrain::acquire(e.project().path, *ent->terrain);
  if (!d) return;
  for (int type = 0; type < static_cast<int>(ent->terrain->foliage.size()); ++type) {
    terrain::FoliageBrush b;
    b.type = type;
    b.radius = ent->terrain->size;
    b.fill = type == 0 ? 0.7f : 1.0f;
    b.seed = 40u + static_cast<unsigned>(type);
    terrain::paint_foliage(*d, ent->terrain->foliage, b, 0, 0);
  }
  std::string err;
  terrain::save_file((fs::path(e.project().path) / ent->terrain->data).string(), *d, &err);
}

int grab(GLFWwindow* window, editor::IScreen& screen, const std::string& png, int frames,
         const std::function<void(int)>& each) {
  int fw = 0, fh = 0;
  std::vector<unsigned char> px;
  for (int frame = 0; frame < frames && !glfwWindowShouldClose(window); ++frame) {
    glfwPollEvents();
    glfwGetFramebufferSize(window, &fw, &fh);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (each) each(frame);
    screen.on_update(1.0f / 60.0f);
    ImGui::Render();
    glViewport(0, 0, fw, fh);
    const ImVec4 clear = editor::theme::Charcoal();
    glClearColor(clear.x, clear.y, clear.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (frame == frames - 1) {
      px.assign(static_cast<std::size_t>(fw) * fh * 4, 0);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadBuffer(GL_BACK);
      glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    glfwSwapBuffers(window);
  }
  const int rc = save_png(png, fw, fh, px);
  if (rc == 0) std::cout << "Showcase screenshot: " << png << " (" << fw << "x" << fh << ")\n";
  return rc;
}

int shot_terrain(const fs::path& dir, const std::string& png, GlWindow& gl) {
  auto screen = std::make_unique<editor::Editor3DScreen>(
      project_at(dir, "showcase_terrain", "Ashen Timber", editor::ProjectKind::ThreeD, false));
  editor::Editor3DScreen* e = screen.get();
  screen->on_enter();
  const std::uint64_t id = e->create_terrain(129, 160.0f, true, "meadow");
  if (!id) {
    std::cerr << "showcase terrain: could not create the forest\n";
    return 1;
  }
  plant_forest(*e, id);
  haze(*e);
  e->set_show_terrain(true);
  e->look_at({10.0f, 6.0f, 8.0f}, 48.0f, 16.0f, 55.0f);
  e->select(id);
  return grab(gl.window, *screen, png, 12, [&](int frame) {
    if (frame == 2) e->anim_preview_tick(0.0f);
  });
}

int shot_props(const fs::path& dir, const std::string& png, GlWindow& gl) {
  std::string err;
  if (!scene3d::write_ghost_town_sample(dir.string(), "Ember Street", &err)) {
    std::cerr << "showcase props: " << err << '\n';
    return 1;
  }
  auto screen = std::make_unique<editor::Editor3DScreen>(
      project_at(dir, "showcase_props", "Ember Street", editor::ProjectKind::ThreeD, false));
  editor::Editor3DScreen* e = screen.get();
  screen->on_enter();
  std::uint64_t pick = 0;
  for (const scene3d::Entity3D& ent : e->scene().entities) {
    if (ent.name == "Old Hank" || ent.name == "Lantern 1") {
      pick = ent.id;
      if (ent.name == "Old Hank") break;
    }
  }
  dress_town(*e);
  fill_street(*e, {-4.6f, 0.0f, -0.4f});
  if (pick) e->select(pick);
  e->gizmo().mode = editor::GizmoMode::Transform;
  // Tight on the saloon front: lanterns, crates and the wanderer, not the far tanks.
  e->look_at({-4.6f, 1.4f, -0.4f}, 38.0f, 14.0f, 5.2f);
  return grab(gl.window, *screen, png, 16, {});
}

int shot_fx(const fs::path& dir, const std::string& png, GlWindow& gl) {
  auto screen = std::make_unique<editor::Editor3DScreen>(
      project_at(dir, "showcase_fx", "Night Forge", editor::ProjectKind::ThreeD, false));
  editor::Editor3DScreen* e = screen.get();
  screen->on_enter();
  ensure_prop_assets(dir);
  std::uint64_t rider = 0;
  for (const scene3d::Entity3D& ent : e->scene().entities) {
    if (ent.name == "Rider") rider = ent.id;
  }
  if (rider) {
    e->select(rider);
    e->delete_selected();
  }
  render3d::Environment env = e->scene().environment;
  env.sky_top[0] = 0.02f;
  env.sky_top[1] = 0.025f;
  env.sky_top[2] = 0.05f;
  env.sky_horizon[0] = 0.22f;
  env.sky_horizon[1] = 0.08f;
  env.sky_horizon[2] = 0.04f;
  env.ambient = 0.08f;
  env.sun_intensity = 0.02f;
  env.fog_color[0] = 0.18f;
  env.fog_color[1] = 0.07f;
  env.fog_color[2] = 0.03f;
  env.fog_density = 0.045f;
  e->set_environment(env, "Night");
  for (const scene3d::Entity3D& ent : e->scene().entities) {
    if (ent.name != "Ground" || !ent.material) continue;
    scene3d::Entity3D copy = ent;
    copy.material->albedo[0] = 0.28f;
    copy.material->albedo[1] = 0.16f;
    copy.material->albedo[2] = 0.09f;
    copy.material->albedo_texture = "assets/boards.tga";
    e->replace_entity(copy, "Forge floor");
    break;
  }
  const float iron[3] = {0.16f, 0.15f, 0.16f};
  const float coal[3] = {1.0f, 0.35f, 0.05f};
  const float glow[3] = {6.0f, 1.8f, 0.25f};
  const render3d::Vec3 forge{0.0f, 0.0f, 0.0f};
  paint_prop(*e, e->create_primitive("cube", forge + render3d::Vec3{0, 0.18f, 0.15f}), "Anvil Base",
             {1.5f, 0.36f, 0.7f}, iron, "", 0.45f, 0.85f, nullptr);
  paint_prop(*e, e->create_primitive("cube", forge + render3d::Vec3{-0.05f, 0.62f, 0.15f}), "Anvil",
             {0.85f, 0.42f, 0.48f}, iron, "", 0.35f, 0.9f, nullptr);
  paint_prop(*e, e->create_primitive("cube", forge + render3d::Vec3{0.55f, 0.72f, 0.15f}), "Anvil Horn",
             {0.55f, 0.16f, 0.16f}, iron, "", 0.3f, 0.95f, nullptr);
  paint_prop(*e, e->create_primitive("cube", forge + render3d::Vec3{0.0f, 0.7f, -0.85f}), "Forge",
             {1.6f, 1.4f, 1.1f}, iron, "assets/boards.tga", 0.8f, 0.4f, nullptr);
  paint_prop(*e, e->create_primitive("cube", forge + render3d::Vec3{0.0f, 0.55f, -0.28f}), "Forge Mouth",
             {0.7f, 0.45f, 0.18f}, coal, "", 1.0f, 0.0f, glow);
  paint_prop(*e, e->create_primitive("sphere", forge + render3d::Vec3{0.15f, 0.42f, 0.05f}), "Coals",
             {0.28f, 0.12f, 0.28f}, coal, "", 1.0f, 0.0f, glow);
  auto place = [&](const render3d::Vec3& pos, const std::string& name, const fx::EmitterDesc& desc) {
    const std::uint64_t id = e->create_empty(pos);
    const scene3d::Entity3D* cur = e->find(id);
    if (!cur) return;
    scene3d::Entity3D copy = *cur;
    copy.name = name;
    copy.particles = desc;
    e->replace_entity(copy, name);
  };
  if (std::optional<fx::EmitterDesc> embers = fx::preset("sparks", fx::Space::ThreeD)) {
    embers->rate = 420.0f;
    embers->max_particles = 1400;
    embers->radius = 0.35f;
    embers->direction[0] = 0.0f;
    embers->direction[1] = 1.0f;
    embers->direction[2] = 0.0f;
    embers->spread = 18.0f;
    embers->speed_min = 1.4f;
    embers->speed_max = 3.2f;
    embers->gravity[0] = 0.0f;
    embers->gravity[1] = 2.4f;
    embers->gravity[2] = 0.0f;
    embers->life_min = 1.1f;
    embers->life_max = 2.2f;
    embers->size[0] = 0.22f;
    embers->size[1] = 0.16f;
    embers->size[2] = 0.08f;
    embers->color_start[0] = 1.0f;
    embers->color_start[1] = 0.72f;
    embers->color_start[2] = 0.15f;
    embers->color_start[3] = 1.0f;
    embers->blend = fx::Blend::Additive;
    place(forge + render3d::Vec3{0, 0.7f, -0.2f}, "Embers", *embers);
  }
  const render3d::Vec3 torches[] = {{-1.35f, 0.0f, 0.35f}, {1.25f, 0.0f, -0.15f}};
  for (const render3d::Vec3& p : torches) {
    paint_prop(*e, e->create_primitive("cube", p + render3d::Vec3{0, 0.7f, 0}), "Torch Post", {0.12f, 1.4f, 0.12f},
               iron, "assets/boards.tga", 0.7f, 0.2f, nullptr);
    if (std::optional<fx::EmitterDesc> fire = fx::preset("campfire", fx::Space::ThreeD)) {
      fire->rate = 280.0f;
      fire->max_particles = 900;
      fire->radius = 0.12f;
      fire->direction[1] = 1.0f;
      fire->speed_min = 0.9f;
      fire->speed_max = 1.8f;
      fire->gravity[1] = 1.6f;
      fire->life_min = 0.55f;
      fire->life_max = 1.1f;
      fire->size[0] = 0.7f;
      fire->size[1] = 1.05f;
      fire->size[2] = 0.35f;
      fire->color_start[0] = 1.0f;
      fire->color_start[1] = 0.78f;
      fire->color_start[2] = 0.2f;
      fire->color_start[3] = 1.0f;
      fire->blend = fx::Blend::Additive;
      place(p + render3d::Vec3{0, 1.45f, 0}, "Torch", *fire);
    }
    const float flame_glow[3] = {8.0f, 2.4f, 0.3f};
    paint_prop(*e, e->create_primitive("sphere", p + render3d::Vec3{0, 1.5f, 0}), "Flame", {0.28f, 0.55f, 0.28f},
               coal, "", 1.0f, 0.0f, flame_glow);
    const std::uint64_t lamp = e->create_light(false, p + render3d::Vec3{0, 1.7f, 0});
    if (const scene3d::Entity3D* cur = e->find(lamp)) {
      scene3d::Entity3D copy = *cur;
      if (copy.light) {
        copy.light->intensity = 22.0f;
        copy.light->range = 16.0f;
        copy.light->color[0] = 1.0f;
        copy.light->color[1] = 0.5f;
        copy.light->color[2] = 0.15f;
      }
      e->replace_entity(copy, "Torch Light");
    }
  }
  place_lantern(*e, forge + render3d::Vec3{-0.9f, 0.0f, -0.9f}, "Forge Lantern");
  const std::uint64_t fill = e->create_light(false, forge + render3d::Vec3{0.2f, 1.6f, 1.2f});
  if (const scene3d::Entity3D* cur = e->find(fill)) {
    scene3d::Entity3D copy = *cur;
    copy.name = "Forge Glow";
    if (copy.light) {
      copy.light->intensity = 28.0f;
      copy.light->range = 18.0f;
      copy.light->color[0] = 1.0f;
      copy.light->color[1] = 0.45f;
      copy.light->color[2] = 0.12f;
    }
    e->replace_entity(copy, "Forge Glow");
  }
  e->look_at({0.05f, 0.85f, 0.0f}, 32.0f, 12.0f, 3.6f);
  return grab(gl.window, *screen, png, 48, {});
}

int shot_graph(const fs::path& dir, const std::string& png, GlWindow& gl) {
  std::error_code ec;
  fs::create_directories(dir / "scripts", ec);
  const char* graph = R"json({
  "graph": 1,
  "nodes": [
    {"id": 1, "type": "event.start", "x": 40, "y": 60},
    {"id": 2, "type": "value.number", "x": 40, "y": 220, "inputs": {"value": 3}},
    {"id": 3, "type": "cmp.gt", "x": 280, "y": 200, "inputs": {"b": 1}},
    {"id": 4, "type": "flow.branch", "x": 520, "y": 60},
    {"id": 5, "type": "toast", "x": 800, "y": 20, "inputs": {"text": "The forge is lit", "seconds": 2}},
    {"id": 6, "type": "log", "x": 800, "y": 200, "inputs": {"text": "Bank the coals"}},
    {"id": 7, "type": "fx.burst", "x": 1080, "y": 20, "inputs": {"preset": "campfire"}}
  ],
  "links": [
    {"from": [1, "out"], "to": [4, "in"]},
    {"from": [2, "value"], "to": [3, "a"]},
    {"from": [3, "value"], "to": [4, "condition"]},
    {"from": [4, "true"], "to": [5, "in"]},
    {"from": [4, "false"], "to": [6, "in"]},
    {"from": [5, "out"], "to": [7, "in"]}
  ]
}
)json";
  {
    std::ofstream out(dir / "scripts" / "forge.graph.json", std::ios::binary);
    out << graph;
  }
  auto screen = std::make_unique<editor::Editor2DScreen>(
      project_at(dir, "showcase_graph", "Brand Book", editor::ProjectKind::TwoD, false));
  screen->on_enter();
  use_wanderer_png(dir, screen.get());
  editor::BrandBook::get().open_graph(dir.string(), "Brand Book", "scripts/forge.graph.json");
  return grab(gl.window, *screen, png, 8, {});
}

int shot_anim(const fs::path& dir, const std::string& png, GlWindow& gl) {
  std::string err;
  if (!scene3d::write_ghost_town_sample(dir.string(), "Old Hank", &err)) {
    std::cerr << "showcase anim: " << err << '\n';
    return 1;
  }
  auto screen = std::make_unique<editor::Editor3DScreen>(
      project_at(dir, "showcase_anim", "Old Hank", editor::ProjectKind::ThreeD, false));
  editor::Editor3DScreen* e = screen.get();
  screen->on_enter();
  std::uint64_t hank = 0;
  if (const scene3d::Entity3D* h = e->scene().find_named("Old Hank")) hank = h->id;
  if (!hank) {
    std::cerr << "showcase anim: Old Hank is not in this scene\n";
    return 1;
  }
  dress_town(*e);
  fill_street(*e, {-1.4f, 0.0f, 1.5f});
  e->select(hank);
  e->gizmo().mode = editor::GizmoMode::Transform;
  e->anim_preview().open = true;
  e->anim_preview().playing = true;
  e->anim_preview().clip = "wave";
  e->look_at({-3.0f, 1.15f, 0.5f}, -24.0f, 8.0f, 3.8f);
  return grab(gl.window, *screen, png, 24, [&](int) { e->anim_preview_tick(1.0f / 60.0f); });
}

int shot_survival(const fs::path& dir, const std::string& png, GlWindow& gl) {
  std::string err;
  if (!survival::write_frontier_sample(dir.string(), "Frontier", &err)) {
    std::cerr << "showcase survival: " << err << '\n';
    return 1;
  }
  use_wanderer_png(dir, nullptr);
  auto screen = std::make_unique<editor::Editor2DScreen>(
      project_at(dir, "showcase_survival", "Frontier", editor::ProjectKind::TwoD, false));
  editor::Editor2DScreen* e = screen.get();
  screen->on_enter();
  const std::uint64_t map = e->workspace().create_tilemap("Frontier ground", 40, 24, 32);
  if (map) {
    e->workspace().fill_rect(map, 0, 0, 39, 23, 2);
    e->workspace().fill_rect(map, 4, 10, 36, 13, 5);
    e->workspace().fill_rect(map, 16, 4, 22, 20, 8);
    e->workspace().select(map);
    e->frame_selected();
  }
  return grab(gl.window, *screen, png, 8, {});
}

int shot_vr(const fs::path& dir, const std::string& png, GlWindow& gl) {
  std::string err;
  if (!scene3d::write_haunted_saloon_sample(dir.string(), "Haunted Saloon", &err)) {
    std::cerr << "showcase vr: " << err << '\n';
    return 1;
  }
  auto screen = std::make_unique<editor::Editor3DScreen>(
      project_at(dir, "showcase_vr", "Haunted Saloon", editor::ProjectKind::ThreeD, true));
  editor::Editor3DScreen* e = screen.get();
  screen->on_enter();
  ensure_prop_assets(dir);
  for (const scene3d::Entity3D& ent : e->scene().entities) {
    if (!ent.material || !ent.mesh) continue;
    const bool cloth = ent.name.find("Wall") != std::string::npos || ent.name.find("Floor") != std::string::npos ||
                       ent.name.find("Door") != std::string::npos || ent.name == "Bar" || ent.name == "Can Table";
    if (!cloth) continue;
    scene3d::Entity3D copy = ent;
    copy.material->albedo_texture = "assets/boards.tga";
    if (ent.name == "Bar" || ent.name.find("Floor") != std::string::npos) {
      copy.material->albedo[0] = 0.55f;
      copy.material->albedo[1] = 0.34f;
      copy.material->albedo[2] = 0.18f;
    }
    e->replace_entity(copy, "Boards on " + ent.name);
  }
  place_lantern(*e, {-2.2f, 0.0f, -3.2f}, "Bar Lantern");
  place_lantern(*e, {2.0f, 0.0f, -3.2f}, "Bar Lantern Right");
  for (const scene3d::Entity3D& ent : e->scene().entities) {
    if (ent.name.rfind("Oil Lamp", 0) != 0 || !ent.light) continue;
    scene3d::Entity3D copy = ent;
    copy.light->intensity = 16.0f;
    copy.light->range = 12.0f;
    copy.light->color[0] = 1.0f;
    copy.light->color[1] = 0.62f;
    copy.light->color[2] = 0.28f;
    e->replace_entity(copy, ent.name);
  }
  e->look_at({0.0f, 1.35f, -1.5f}, 8.0f, 6.0f, 5.5f);
  return grab(gl.window, *screen, png, 20, [&](int frame) {
    if (frame == 3 && !e->vr_previewing()) e->vr_preview_start(true);
  });
}

}  // namespace

int run_showcase_shot(const std::string& scene, const std::string& png) {
  const fs::path dir = fs::temp_directory_path() / ("ts_showcase_" + scene);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  GlWindow gl;
  if (!open_gl(&gl, 1920, 1080, "Nidavellir Showcase")) return 1;
  int rc = 1;
  if (scene == "terrain") rc = shot_terrain(dir, png, gl);
  else if (scene == "props") rc = shot_props(dir, png, gl);
  else if (scene == "fx") rc = shot_fx(dir, png, gl);
  else if (scene == "graph") rc = shot_graph(dir, png, gl);
  else if (scene == "anim") rc = shot_anim(dir, png, gl);
  else if (scene == "survival") rc = shot_survival(dir, png, gl);
  else if (scene == "vr") rc = shot_vr(dir, png, gl);
  else {
    std::cerr << "--showcase-shot wants terrain, props, fx, graph, anim, survival or vr\n";
    rc = 1;
  }
  fs::remove_all(dir, ec);
  return rc;
}
