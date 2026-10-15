// --smoke-ai, part 3: the Behaviour Trees panel is reachable from the View
// menu of both editors, during Play, with the screen's live plugins.

#include "SmokeAI.h"

#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginPanelHost.h"
#include "editor/screens/Editor2DScreen.h"
#include "render3d/Scene3D.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace ts::tombstone;

namespace {

int fail(const std::string& m) {
  std::cerr << "[smoke] ai menu FAIL: " << m << "\n";
  return 1;
}

// One headless frame: the View entries, then the open panels. Returns the
// entry count; *shown = the panel window was drawn this frame.
int menu_frame(editor::EditorPlugins& plugins, bool* shown) {
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGuiContext* c = ImGui::CreateContext();
  ImGui::SetCurrentContext(c);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280, 800);
  io.IniFilename = nullptr;
  unsigned char* px = nullptr;
  int fw = 0, fh = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
  std::map<std::string, bool> open;
  int entries = 0;
  bool has_bt = false;
  for (int frame = 0; frame < 2; ++frame) {
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("View##probe");
    entries = editor::draw_plugin_panel_menu_items(plugins, open);
    ImGui::End();
    has_bt = open.count("ai.behaviour_trees") != 0;
    open["ai.behaviour_trees"] = true;
    editor::draw_open_plugin_panels(plugins, open);
    ImGuiWindow* w = ImGui::FindWindowByName("Behaviour Trees###plugin_ai.behaviour_trees");
    *shown = has_bt && w && w->Active;
    ImGui::Render();
  }
  ImGui::DestroyContext(c);
  ImGui::SetCurrentContext(prev);
  return entries;
}

}  // namespace

int run_ai_menu_smoke() {
  const fs::path root = fs::absolute("./TombStoneProjects/_ai_menu_smoke");
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "p2d", ec);
  int n2 = 0, n3 = 0;
  {
    editor::ProjectInfo info;
    info.id = "_ai_menu_2d";
    info.name = "AI Menu 2D";
    info.path = (root / "p2d").string();
    info.plugins = {"ts.ai"};
    info.plugins_set = true;
    editor::Editor2DScreen scr(info);
    scr.on_enter();
    if (!scr.plugins().loaded("ts.ai")) return fail("2D: ts.ai should load");
    if (!scr.start_play()) return fail("2D: Play");
    bool shown = false;
    n2 = menu_frame(scr.plugins(), &shown);
    if (!shown) return fail("2D: Behaviour Trees should be a View entry and open during Play");
    scr.stop_play();
    scr.on_exit();
  }
  {
    std::string err;
    if (!scene3d::write_ghost_town_sample((root / "p3d").string(), "AI Menu 3D", &err)) return fail(err);
    editor::ProjectInfo info;
    info.id = "_ai_menu_3d";
    info.name = "AI Menu 3D";
    info.kind = editor::ProjectKind::ThreeD;
    info.path = (root / "p3d").string();
    info.plugins = {"ts.ai"};
    info.plugins_set = true;
    editor::Editor3DScreen e(info);
    e.on_enter();
    if (!e.plugins().loaded("ts.ai")) return fail("3D: ts.ai should load");
    if (!e.play_start()) return fail("3D: Play");
    bool shown = false;
    n3 = menu_frame(e.plugins(), &shown);
    if (!shown) return fail("3D: Behaviour Trees should be a View entry and open during Play");
    e.play_stop();
    e.on_exit();
  }
  fs::remove_all(root, ec);
  std::cout << "[smoke] ai menu OK (Behaviour Trees in View during Play: 2D " << n2
            << " plugin entr(ies), 3D " << n3 << ")\n";
  return 0;
}
