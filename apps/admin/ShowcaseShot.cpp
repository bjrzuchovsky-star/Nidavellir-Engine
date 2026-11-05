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
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;

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
  if (pick) e->select(pick);
  e->gizmo().mode = editor::GizmoMode::Transform;
  e->look_at({0.0f, 1.5f, 2.0f}, 32.0f, 18.0f, 18.0f);
  return grab(gl.window, *screen, png, 16, {});
}

int shot_fx(const fs::path& dir, const std::string& png, GlWindow& gl) {
  auto screen = std::make_unique<editor::Editor3DScreen>(
      project_at(dir, "showcase_fx", "Night Forge", editor::ProjectKind::ThreeD, false));
  editor::Editor3DScreen* e = screen.get();
  screen->on_enter();
  render3d::Environment env = e->scene().environment;
  env.sky_top[0] = 0.02f;
  env.sky_top[1] = 0.03f;
  env.sky_top[2] = 0.06f;
  env.sky_horizon[0] = 0.12f;
  env.sky_horizon[1] = 0.08f;
  env.sky_horizon[2] = 0.06f;
  env.ambient = 0.05f;
  env.sun_intensity = 0.05f;
  env.fog_color[0] = 0.05f;
  env.fog_color[1] = 0.05f;
  env.fog_color[2] = 0.08f;
  env.fog_density = 0.03f;
  e->set_environment(env, "Night");
  const render3d::Vec3 spots[] = {{-2.2f, 0.0f, 1.4f}, {2.4f, 0.0f, -0.6f}, {0.2f, 0.0f, -2.8f}};
  for (const render3d::Vec3& p : spots) {
    scene3d::Entity3D torch;
    torch.name = "Torch";
    torch.transform.position = p + render3d::Vec3{0, 1.1f, 0};
    if (std::optional<fx::EmitterDesc> fire = fx::preset("campfire", fx::Space::ThreeD)) {
      fire->rate = 40.0f;
      torch.particles = *fire;
    }
    e->add_entity(std::move(torch), "Torch");
    scene3d::Entity3D ember;
    ember.name = "Embers";
    ember.transform.position = p + render3d::Vec3{0, 1.4f, 0};
    if (std::optional<fx::EmitterDesc> sparks = fx::preset("sparks", fx::Space::ThreeD)) {
      ember.particles = *sparks;
    }
    e->add_entity(std::move(ember), "Embers");
    e->create_light(false, p + render3d::Vec3{0, 1.6f, 0});
  }
  e->look_at({0.0f, 1.2f, 0.0f}, 28.0f, 14.0f, 9.0f);
  return grab(gl.window, *screen, png, 36, {});
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
  e->select(hank);
  e->gizmo().mode = editor::GizmoMode::Transform;
  e->anim_preview().open = true;
  e->anim_preview().playing = true;
  e->anim_preview().clip = "wave";
  e->look_at({-3.0f, 1.0f, 0.5f}, -20.0f, 12.0f, 6.0f);
  return grab(gl.window, *screen, png, 24, [&](int) { e->anim_preview_tick(1.0f / 60.0f); });
}

int shot_survival(const fs::path& dir, const std::string& png, GlWindow& gl) {
  std::string err;
  if (!survival::write_frontier_sample(dir.string(), "Frontier", &err)) {
    std::cerr << "showcase survival: " << err << '\n';
    return 1;
  }
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
  e->look_at({0.0f, 1.6f, 0.0f}, 20.0f, 8.0f, 7.0f);
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
