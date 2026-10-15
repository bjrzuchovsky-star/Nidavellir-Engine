// --smoke-physics, editor part: headless ImGui frames of the 2D and 3D
// physics inspectors (Rigid Body, Joint, compound collider), the Physics
// Settings window and the rotated 2D collision overlay. Catches ImGui
// ID-stack / Begin-End mismatches in the new panels.

#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "render3d/Scene3D.h"
#include "render3d/Gltf.h"

#include <imgui.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace ts::tombstone;

namespace {

int fail(const std::string& m) {
  std::cerr << "[smoke] physics editor FAIL: " << m << "\n";
  return 1;
}

template <class F>
void frames(int n, F&& body) {
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGuiContext* c = ImGui::CreateContext();
  ImGui::SetCurrentContext(c);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1600, 900);
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  unsigned char* px = nullptr;
  int fw = 0, fh = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
  for (int i = 0; i < n; ++i) {
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    body();
    ImGui::Render();
  }
  ImGui::DestroyContext(c);
  ImGui::SetCurrentContext(prev);
}

}  // namespace

int run_physics_editor_smoke() {
  const fs::path root = fs::absolute("./TombStoneProjects/_phys_editor_smoke");
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "p2d", ec);
  fs::create_directories(root / "p3d/assets", ec);

  {  // 3D
    editor::ProjectInfo info;
    info.id = "_phys_ed_3d";
    info.name = "Phys Ed 3D";
    info.kind = editor::ProjectKind::ThreeD;
    info.path = (root / "p3d").string();
    std::string err;
    render3d::write_sample_lantern_gltf((root / "p3d/assets/lantern.gltf").string(), &err);
    editor::Editor3DScreen e(info);
    e.on_enter();
    const scene3d::Entity3D* crate = e.scene().find_named("Crate");
    if (!crate) return fail("3D starter crate");
    scene3d::Entity3D c = *crate;
    c.collider = scene3d::Collider3D{};
    c.collider->shape = scene3d::Collider3D::Shape::Capsule;
    c.collider->parts.push_back({});
    c.rigid_body = scene3d::RigidBody3D{};
    c.joint = scene3d::Joint3D{};
    if (!e.replace_entity(c, "phys smoke")) return fail("3D replace");
    e.select(c.id);
    e.set_show_physics_settings(true);
    frames(3, [&] { e.on_update(1.0f / 60.0f); });
    e.on_exit();
  }
  {  // 2D
    editor::ProjectInfo info;
    info.id = "_phys_ed_2d";
    info.name = "Phys Ed 2D";
    info.path = (root / "p2d").string();
    editor::Editor2DScreen s(info);
    s.on_enter();
    auto& ents = s.workspace().entities();
    std::uint64_t pick = 0, other = 0;
    for (auto& en : ents) {
      if (en.tilemap || en.player) continue;
      if (!pick) {
        pick = en.id;
        en.collider = ColliderData{};
        en.rotation = 30.0f;
        en.body = RigidBody2DData{};
        en.joint = Joint2DData{};
      } else if (!other) {
        other = en.id;
        en.collider = ColliderData{};
        en.collider->shape = ColliderData::Shape::Circle;
      }
    }
    if (!pick) return fail("2D starter entity");
    if (other) s.workspace().find(pick)->joint->target = other;
    s.workspace().select(pick);
    s.set_show_collision(true);
    frames(3, [&] { s.on_update(1.0f / 60.0f); });
    s.on_exit();
  }
  fs::remove_all(root, ec);
  std::cout << "[smoke] physics editor OK (3D rigid body/joint/compound inspectors + settings, "
               "2D body/joint inspector + rotated overlay)\n";
  return 0;
}
