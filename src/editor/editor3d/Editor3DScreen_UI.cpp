#include "editor/plugins/PluginPanelHost.h"
#include "editor/plugins/ProfilerPanels.h"
#include "editor/fx/FxInspector.h"
#include "editor/export/FreightOffice.h"
#include "editor/scripting/BrandBook.h"
#include "editor/editor3d/Editor3DScreen.h"

#include "ui/UiScript.h"
#include "editor/audio/EditorAudio.h"
#include "editor/ui/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

using render3d::Vec3;

namespace {
constexpr const char* kViewport = "Viewport###Viewport3D";
constexpr const char* kHierarchy = "Hierarchy###Hierarchy3D";
constexpr const char* kInspector = "Inspector###Inspector3D";
constexpr const char* kAssets = "Assets###Assets3D";
constexpr const char* kEnvironment = "Environment###Environment3D";
constexpr const char* kTelegraph = "Telegraph###Telegraph3D";
constexpr const char* kPlugins = "Plugins###Plugins3D";
constexpr const char* kPrefabs = "Prefabs###Prefabs3D";
constexpr const char* kDragPayload = "TS_ASSET3D";

ImU32 axis_color(int axis, float alpha = 1.0f) {
  const ImVec4 c = axis == 0 ? ImVec4(0.92f, 0.30f, 0.25f, alpha)
                   : axis == 1 ? ImVec4(0.45f, 0.85f, 0.30f, alpha)
                               : ImVec4(0.30f, 0.55f, 0.95f, alpha);
  return ImGui::GetColorU32(c);
}
}  // namespace

void Editor3DScreen::setup_dock_layout(unsigned int dockspace_id) {
  if (dock_initialized_) return;
  dock_initialized_ = true;
  if (ImGui::DockBuilderGetNode(dockspace_id) != nullptr) return;
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);
  ImGuiID main = dockspace_id, left = 0, right = 0, center = 0, left_bottom = 0,
          right_bottom = 0, bottom = 0;
  ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.20f, &left, &main);
  ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.28f, &right, &center);
  ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, &left_bottom, &left);
  ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.38f, &right_bottom, &right);
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, &bottom, &center);
  ImGui::DockBuilderDockWindow(kHierarchy, left);
  ImGui::DockBuilderDockWindow(kAssets, left_bottom);
  ImGui::DockBuilderDockWindow(kPlugins, left_bottom);
  ImGui::DockBuilderDockWindow(kPrefabs, left_bottom);
  ImGui::DockBuilderDockWindow(kViewport, center);
  ImGui::DockBuilderDockWindow(kTelegraph, bottom);
  ImGui::DockBuilderDockWindow(kInspector, right);
  ImGui::DockBuilderDockWindow(kEnvironment, right_bottom);
  ImGui::DockBuilderFinish(dockspace_id);
}

void Editor3DScreen::draw_ui() {
  handle_shortcuts();
  draw_menu_bar();
  draw_toolbar();
  draw_status_bar();

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float top = theme::metrics::kToolbarHeight, bot = theme::metrics::kStatusBarHeight;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + top));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - top - bot));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##Editor3DDockHost", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                   ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  const ImGuiID dock = ImGui::GetID("Editor3DDockspace");
  setup_dock_layout(dock);
  ImGui::DockSpace(dock, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::End();

  const bool locked = playing();
  if (ImGui::Begin(kHierarchy)) {
    ImGui::BeginDisabled(locked);
    draw_hierarchy();
    ImGui::EndDisabled();
  }
  ImGui::End();
  if (ImGui::Begin(kAssets)) draw_assets();
  ImGui::End();
  if (show_prefabs_) {
    if (ImGui::Begin(kPrefabs, &show_prefabs_)) {
      ImGui::BeginDisabled(locked);
      draw_prefabs_panel();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (ImGui::Begin(kPlugins)) {
    const PluginsPanelResult r = draw_plugins_panel(plugins_, plugins_panel_, playing());
    if (r.changed && plugins_.pending() && !playing()) plugins_.apply();
    if (r.rescan_requested && !playing()) plugins_.rescan();
  }
  ImGui::End();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const bool vis = ImGui::Begin(kViewport, nullptr,
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (vis) draw_viewport();
  ImGui::End();
  if (ImGui::Begin(kInspector)) {
    ImGui::BeginDisabled(locked);
    draw_inspector();
    ImGui::EndDisabled();
  }
  ImGui::End();
  if (ImGui::Begin(kEnvironment)) {
    ImGui::BeginDisabled(locked);
    draw_environment();
    ImGui::EndDisabled();
  }
  ImGui::End();
  if (ImGui::Begin(kTelegraph)) draw_telegraph();
  ImGui::End();
  if (show_mixer_) {
    ImGui::SetNextWindowSize(ImVec2(470, 470), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Saloon Band (Mixer)", &show_mixer_)) draw_mixer_panel();
    ImGui::End();
  }
  // Plugin panels (Behaviour Trees, ...): open in edit mode and during Play.
  draw_open_plugin_panels(plugins_, plugin_panel_open_);
  if (show_physics_) {
    ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Physics Settings", &show_physics_)) {
      ImGui::BeginDisabled(locked);
      draw_physics_settings();
      ImGui::EndDisabled();
    }
    ImGui::End();
  }
  if (anim_preview_.open) {
    ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Animation Preview", &anim_preview_.open)) draw_anim_preview();
    ImGui::End();
  }
  ui_preview_.draw(&show_ui_preview_, project_.path);
  draw_vr_mirror();
  FreightOffice::get().draw();
  BrandBook::get().draw();
}

void Editor3DScreen::draw_menu_bar() {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu("Claim")) {
    if (ImGui::MenuItem("Save scene", "Ctrl+S")) {
      std::string err;
      if (save_scene(&err)) note("Saved scene.json");
      else note("Save failed: " + err, runtime::LogLevel::Error);
    }
    if (ImGui::MenuItem("Reload from disk", nullptr, false, !playing())) {
      std::string err;
      if (!reload_scene(&err)) note("Reload failed: " + err, runtime::LogLevel::Error);
    }
    if (ImGui::MenuItem("Rescan assets")) rescan_assets();
    if (ImGui::MenuItem("Export... (Freight Office)", nullptr, false, !playing()))
      FreightOffice::get().open(project_.path, project_.name);
    if (ImGui::MenuItem("Brand Book... (visual scripts)")) BrandBook::get().open(project_.path, project_.name);
    ImGui::Separator();
    if (ImGui::MenuItem("Back to Claims")) request_back_to_projects();
    if (ImGui::MenuItem("Quit")) request_quit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem(("Undo " + undo_label()).c_str(), "Ctrl+Z", false, can_undo() && !playing()))
      undo();
    if (ImGui::MenuItem(("Redo " + redo_label()).c_str(), "Ctrl+Y", false, can_redo() && !playing()))
      redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection().empty())) duplicate_selected();
    if (ImGui::MenuItem("Delete", "Del", false, !selection().empty())) delete_selected();
    ImGui::Separator();
    const bool has = !selection().empty() && !playing();
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, has)) copy_selected();
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, has)) cut_selected();
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, !playing() && !clipboard_text().empty())) paste();
    ImGui::Separator();
    if (ImGui::MenuItem("Group", "Ctrl+G", false, has)) group_selected();
    if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, has)) ungroup_selected();
    if (ImGui::MenuItem("Lock", "Ctrl+L", false, has)) lock_selected(true);
    if (ImGui::MenuItem("Unlock", "Ctrl+Shift+L", false, has)) lock_selected(false);
    if (ImGui::MenuItem("Hide", "H", false, has)) hide_selected(true);
    if (ImGui::MenuItem("Show", "Shift+H", false, has)) hide_selected(false);
    if (ImGui::MenuItem("Drop to ground", "End", false, has)) drop_to_ground();
    if (ImGui::BeginMenu("Align / Distribute", has && selection().size() >= 2)) {
      static const char* kAx[3] = {"X", "Y", "Z"};
      for (int a = 0; a < 3; ++a) {
        if (ImGui::MenuItem((std::string("Min ") + kAx[a]).c_str())) align_selected(a, -1);
        if (ImGui::MenuItem((std::string("Centre ") + kAx[a]).c_str())) align_selected(a, 0);
        if (ImGui::MenuItem((std::string("Max ") + kAx[a]).c_str())) align_selected(a, 1);
        if (ImGui::MenuItem((std::string("Distribute ") + kAx[a]).c_str(), nullptr, false,
                            selection().size() >= 3)) {
          distribute_selected(a);
        }
        if (a < 2) ImGui::Separator();
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Create")) {
    const Vec3 at = scene_.editor_camera.target;
    for (const char* p : {"cube", "plane", "sphere", "capsule"})
      if (ImGui::MenuItem(p)) create_primitive(p, {at.x, 0.0f, at.z});
    ImGui::Separator();
    if (ImGui::MenuItem("Point light")) create_light(false, {at.x, 2.5f, at.z});
    if (ImGui::MenuItem("Sun (directional)")) create_light(true, {at.x, 8.0f, at.z});
    if (ImGui::MenuItem("Empty")) create_empty({at.x, 0.0f, at.z});
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Grid", "G", &scene_.show_grid);
    ImGui::MenuItem("Colliders + joints", nullptr, &show_colliders_);
    ImGui::MenuItem("Physics Settings", nullptr, &show_physics_);
    if (ImGui::MenuItem("Physics debug draw (Play)", nullptr, scene_.physics.debug_draw, !playing())) {
      const scene3d::Scene3D before = scene_;
      scene_.physics.debug_draw = !scene_.physics.debug_draw;
      push_undo("Physics debug draw", before);
    }
    ImGui::MenuItem("Saloon Band (Mixer)", nullptr, &show_mixer_);
    ImGui::MenuItem("UI Preview", nullptr, &show_ui_preview_);
    ImGui::MenuItem("Animation Preview", nullptr, &anim_preview_.open);
    ImGui::MenuItem("Prefabs", nullptr, &show_prefabs_);
    if (plugins_.is_open() && !plugins_.manager().editor_panels().empty()) {
      ImGui::Separator();
      draw_plugin_panel_menu_items(plugins_, plugin_panel_open_);
      ImGui::Separator();
    }
    if (ImGui::MenuItem("Frame selection", "F")) frame_selection();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Help")) {
    theme::KeyHint("RMB / Alt+LMB", "Orbit");
    theme::KeyHint("MMB", "Pan");
    theme::KeyHint("Wheel", "Zoom");
    theme::KeyHint("RMB + WASD/QE", "Fly");
    theme::KeyHint("LMB", "Select (Ctrl: add / remove)");
    theme::KeyHint("W / E / R", "Move / rotate / scale gizmo");
    theme::KeyHint("Shift+G", "Toggle snap");
    theme::KeyHint("F", "Frame selection");
    theme::KeyHint("Ctrl+Z / Ctrl+Y", "Undo / redo");
    theme::KeyHint("Ctrl+D / Del", "Duplicate / delete");
    theme::KeyHint("F5", "Play / Stop (Stop restores the scene)");
    theme::KeyHint("WASD Space E", "Ride: move, jump, interact");
    theme::KeyHint("C", "Free camera while playing");
    theme::KeyHint("Hold F / G", "Carry a physics body / throw it");
    ImGui::EndMenu();
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - 360.0f);
  ImGui::TextDisabled("%s  [3D]%s", project_.name.c_str(), playing() ? "  [RIDING]" : "");
  ImGui::EndMainMenuBar();
}

void Editor3DScreen::draw_toolbar() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, theme::metrics::kToolbarHeight));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::HeaderBg());
  ImGui::Begin("##Editor3DToolbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoNav);
  if (playing()) {
    if (theme::SecondaryButton("Stop", ImVec2(64, 0))) play_stop();
    ImGui::SameLine();
    bool paused = world_.paused();
    if (ImGui::Checkbox("Pause", &paused)) world_.set_paused(paused);
    ImGui::SameLine();
    ImGui::Checkbox("Free cam (C)", &free_camera_in_play_);
  } else if (vr_project()) {
    if (theme::PrimaryButton("Preview in headset")) vr_preview_start(false);
    ImGui::SameLine();
    if (theme::SecondaryButton("Play (simulator)")) vr_preview_start(true);
  } else if (theme::PrimaryButton("Play", ImVec2(64, 0))) {
    play_start();
  }
  ImGui::SameLine(0, 18);
  ImGui::BeginDisabled(playing());
  const struct { GizmoMode m; const char* l; } modes[] = {
      {GizmoMode::Translate, "Move (W)"}, {GizmoMode::Rotate, "Rotate (E)"}, {GizmoMode::Scale, "Scale (R)"}};
  for (const auto& m : modes) {
    if (ImGui::RadioButton(m.l, gizmo_.mode == m.m)) gizmo_.mode = m.m;
    ImGui::SameLine();
  }
  bool snap = scene_.snap;
  if (ImGui::Checkbox("Snap", &snap)) set_snapping(snap);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(60);
  ImGui::DragFloat("##st", &scene_.snap_translate, 0.05f, 0.05f, 10.0f, "%.2fm");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(50);
  ImGui::DragFloat("##sr", &scene_.snap_rotate, 1.0f, 1.0f, 90.0f, "%.0f deg");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(50);
  ImGui::DragFloat("##ss", &scene_.snap_scale, 0.05f, 0.05f, 2.0f, "%.2fx");
  ImGui::SameLine(0, 18);
  if (theme::SecondaryButton("Undo", ImVec2(56, 0))) undo();
  ImGui::SameLine();
  if (theme::SecondaryButton("Redo", ImVec2(56, 0))) redo();
  ImGui::EndDisabled();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void Editor3DScreen::draw_status_bar() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = theme::metrics::kStatusBarHeight;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 4.0f));
  ImGui::Begin("##Editor3DStatus", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoNav);
  // Play state: the VR rig has no "grounded" (the headset floor is the
  // ground), so VR previews say where the picture goes instead.
  std::string play_state;
  if (vr_previewing()) {
    const bool sim = vr_session_ && vr_session_->simulated();
    play_state = sim ? " | VR preview: desktop simulator"
                     : " | VR preview: headset (" + vr_status_ + ")";
  } else if (playing()) {
    play_state = world_.grounded() ? " | rider grounded" : " | rider airborne";
  }
  ImGui::TextDisabled("%s | %zu entities | %zu sel | %zu draws, %zu tris, %zu lights | %s%s",
                      renderer_backend(), scene_.entities.size(), selection().size(),
                      last_stats_.draws, last_stats_.triangles, last_stats_.lights,
                      scene_.snap ? "snap on" : "snap off", play_state.c_str());
  ImGui::End();
  ImGui::PopStyleVar();
}

world3d::Input3D Editor3DScreen::poll_play_input() const {
  world3d::Input3D in;
  if (ImGui::GetIO().WantTextInput) return in;
  auto down = [](ImGuiKey k) { return ImGui::IsKeyDown(k); };
  in.move_right = (down(ImGuiKey_D) || down(ImGuiKey_RightArrow) ? 1.0f : 0.0f) -
                  (down(ImGuiKey_A) || down(ImGuiKey_LeftArrow) ? 1.0f : 0.0f);
  in.move_forward = (down(ImGuiKey_W) || down(ImGuiKey_UpArrow) ? 1.0f : 0.0f) -
                    (down(ImGuiKey_S) || down(ImGuiKey_DownArrow) ? 1.0f : 0.0f);
  in.turn = (down(ImGuiKey_Q) ? -1.0f : 0.0f) +
            (down(ImGuiKey_Z) ? 1.0f : 0.0f);
  in.jump = down(ImGuiKey_Space);
  in.action = ImGui::IsKeyPressed(ImGuiKey_E, false);
  // Physics gun: hold F to carry the body ahead, G (or left click while
  // carrying) throws it.
  in.grab = down(ImGuiKey_F);
  in.throw_held = ImGui::IsKeyPressed(ImGuiKey_G, false) ||
                  (in.grab && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
  return in;
}

void Editor3DScreen::handle_shortcuts() {
  const ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;
  if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
    if (playing()) play_stop();
    else play_start();
    return;
  }
  if (playing()) {
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) free_camera_in_play_ = !free_camera_in_play_;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) play_stop();
    return;
  }
  if (io.KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
      if (io.KeyShift) redo();
      else undo();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    if (ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selected();
    if (!gizmo_.active()) {
      if (ImGui::IsKeyPressed(ImGuiKey_C, false)) copy_selected();
      if (ImGui::IsKeyPressed(ImGuiKey_X, false)) cut_selected();
      if (ImGui::IsKeyPressed(ImGuiKey_V, false)) paste();
      if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        if (io.KeyShift) ungroup_selected();
        else group_selected();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_L, false)) lock_selected(!io.KeyShift);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      std::string err;
      if (save_scene(&err)) note("Saved scene.json");
    }
    return;
  }
  // Single keys only when the viewport has focus (RMB-fly uses WASD).
  if (!viewport_focused_ || ImGui::IsMouseDown(ImGuiMouseButton_Right)) return;
  if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_.mode = GizmoMode::Translate;
  if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_.mode = GizmoMode::Rotate;
  if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmo_.mode = GizmoMode::Scale;
  if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selection();
  if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
    if (io.KeyShift) set_snapping(!scene_.snap);
    else scene_.show_grid = !scene_.show_grid;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selected();
  if (ImGui::IsKeyPressed(ImGuiKey_H, false)) hide_selected(!io.KeyShift);
  if (ImGui::IsKeyPressed(ImGuiKey_End, false)) drop_to_ground();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (gizmo_.active()) gizmo_cancel();
    else clear_selection();
  }
}

void Editor3DScreen::draw_viewport() {
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float vw = std::max(32.0f, avail.x), vh = std::max(32.0f, avail.y);
  viewport_w_ = vw;
  viewport_h_ = vh;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float scale = ImGui::GetIO().DisplayFramebufferScale.x > 0 ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
  render_frame(static_cast<int>(vw * scale), static_cast<int>(vh * scale));
  const std::uint64_t tex = viewport_texture();
  if (tex != 0) {
    // GL textures are bottom-up.
    ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(vw, vh), ImVec2(0, 1), ImVec2(1, 0));
  } else {
    ImGui::InvisibleButton("##vp_null", ImVec2(vw, vh));
  }
  ImGui::SetCursorScreenPos(origin);
  ImGui::InvisibleButton("##vp_input", ImVec2(vw, vh),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  viewport_hovered_ = ImGui::IsItemHovered();
  viewport_focused_ = ImGui::IsWindowFocused();
  const ImGuiIO& io = ImGui::GetIO();
  const float mx = io.MousePos.x - origin.x, my = io.MousePos.y - origin.y;

  // Asset drops.
  if (!playing() && ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload)) {
      const std::string rel(static_cast<const char*>(p->Data));
      Vec3 at = scene_.editor_camera.target;
      ground_point(mx, my, vw, vh, &at);
      drop_asset(rel, at, pick(mx, my, vw, vh));
    }
    ImGui::EndDragDropTarget();
  }

  // RMB: drag orbits / flies, a click (no drag) opens the context menu.
  if (viewport_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) rmb_dragged_ = false;
  if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) rmb_dragged_ = true;
  if (!playing() && viewport_hovered_ && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      !rmb_dragged_ && !gizmo_.active()) {
    context_id_ = pick(mx, my, vw, vh);
    if (context_id_ && std::find(selection().begin(), selection().end(), context_id_) ==
                           selection().end()) {
      select(context_id_);
    }
    ImGui::OpenPopup("##Viewport3DContext");
  }
  if (ImGui::BeginPopup("##Viewport3DContext")) {
    draw_context_menu(context_id_);
    ImGui::EndPopup();
  }

  // Alt+LMB on a gizmo handle: copy the selection and drag the copies
  // (Alt+LMB anywhere else still orbits).
  bool alt_dup_now = false;
  if (!playing() && io.KeyAlt && viewport_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    alt_dup_now = gizmo_begin_duplicate(mx, my, vw, vh);
  }

  // Camera.
  if (viewport_hovered_ || ImGui::IsItemActive()) {
    if (io.MouseWheel != 0.0f) dolly(std::pow(0.88f, io.MouseWheel));
    const bool orbiting = ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                          (io.KeyAlt && !dup_drag_ && !alt_dup_now &&
                           ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    if (orbiting) orbit(-io.MouseDelta.x * 0.3f, io.MouseDelta.y * 0.3f);
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) pan(io.MouseDelta.x, io.MouseDelta.y, vh);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && (!playing() || free_camera_in_play_)) {
      // Fly: move the orbit target along the view.
      const render3d::Camera3D cam = editor_camera();
      const Vec3 f = cam.forward();
      const Vec3 r = render3d::normalize(render3d::cross(f, {0, 1, 0}));
      const float sp = std::max(2.0f, scene_.editor_camera.distance) * io.DeltaTime *
                       (io.KeyShift ? 3.0f : 1.0f);
      Vec3 d{0, 0, 0};
      if (ImGui::IsKeyDown(ImGuiKey_W)) d += f;
      if (ImGui::IsKeyDown(ImGuiKey_S)) d += f * -1.0f;
      if (ImGui::IsKeyDown(ImGuiKey_D)) d += r;
      if (ImGui::IsKeyDown(ImGuiKey_A)) d += r * -1.0f;
      if (ImGui::IsKeyDown(ImGuiKey_E)) d += Vec3{0, 1, 0};
      if (ImGui::IsKeyDown(ImGuiKey_Q)) d += Vec3{0, -1, 0};
      scene_.editor_camera.target += d * sp;
    }
  }

  // Selection + gizmo (left button; Alt only while an Alt+drag copy runs).
  if (!playing() && (!io.KeyAlt || dup_drag_)) {
    // Ctrl while dragging the move gizmo: stick to the surface under the mouse.
    set_surface_snap(io.KeyCtrl && gizmo_.active());
    if (viewport_hovered_ && !dup_drag_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (!gizmo_begin(mx, my, vw, vh)) {
        const std::uint64_t id = pick(mx, my, vw, vh);
        if (id) select(id, io.KeyCtrl);
        else if (!io.KeyCtrl) clear_selection();
      }
    }
    if (gizmo_.active()) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) gizmo_move(mx, my, vw, vh);
      else gizmo_end();
    }
    if (gizmo_.active() && surface_snap_) {
      ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 10, origin.y + 10),
                                          theme::U32(theme::Accent()), "surface snap");
    }
    draw_gizmo_overlay(origin.x, origin.y, vw, vh);
  }

  // In-game UI (ui/*.ui.json, Lua ui.*) over the Play view.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (playing() && !vr_previewing()) {
    if (debug_tools_on_ && (viewport_focused_ || viewport_hovered_)) {
      if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) debug_tools_.toggle_overlay();
      if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) debug_tools_.toggle_console();
    }
    std::vector<ui::UiEvent> events =
        play_ui_.update(origin, ImVec2(vw, vh), viewport_hovered_, viewport_focused_, io.DeltaTime);
    if (debug_tools_on_) {
      events = debug_tools_.filter(std::move(events));
      debug_tools_.update(io.DeltaTime);
    }
    ui::forward_ui_events(world_, events);
    play_ui_.draw(dl, origin);
  }
  // Toasts.
  float ty = origin.y + 12.0f;
  for (const auto& t : toasts_) {
    const ImVec2 sz = ImGui::CalcTextSize(t.first.c_str());
    const float x = origin.x + (vw - sz.x) * 0.5f;
    dl->AddRectFilled(ImVec2(x - 10, ty - 4), ImVec2(x + sz.x + 10, ty + sz.y + 4),
                      IM_COL32(20, 16, 12, 200), 3.0f);
    dl->AddText(ImVec2(x, ty), theme::U32(theme::Accent()), t.first.c_str());
    ty += sz.y + 12.0f;
  }
  if (playing()) {
    const char* hint = "WASD move, Space jump, E interact, RMB/Q/Z turn camera, Esc stops";
    if (vr_previewing()) {
      hint = (vr_session_ && vr_session_->simulated())
                 ? "VR sim: RMB look, WASD move, Q/E turn, T teleport, LMB trigger, "
                   "G/MMB grip, Shift left hand, M menu, Ctrl crouch, Esc stops"
                 : "VR: playing in the headset (Headset Mirror shows both eyes), "
                   "Esc or Stop ends the preview";
    }
    dl->AddText(ImVec2(origin.x + 10, origin.y + vh - 22), IM_COL32(240, 220, 170, 220), hint);
  }
}

void Editor3DScreen::draw_gizmo_overlay(float x0, float y0, float vw, float vh) {
  const scene3d::Entity3D* e = find(primary());
  if (!e) return;
  const render3d::Camera3D cam = editor_camera();
  const Vec3 o = e->transform.position;
  float ox = 0, oy = 0;
  if (!cam.project(o, vw, vh, &ox, &oy)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float len = Gizmo3D::handle_length(cam, vh, o);
  const ImGuiIO& io = ImGui::GetIO();
  const int hover = gizmo_.active() ? gizmo_.axis()
                                    : gizmo_.hit(cam, vw, vh, o, io.MousePos.x - x0, io.MousePos.y - y0);
  for (int a = 0; a < 3; ++a) {
    const float th = hover == a ? 4.0f : 2.5f;
    if (gizmo_.mode == GizmoMode::Rotate) {
      // Ring around the axis.
      const Vec3 ax = Gizmo3D::axis_dir(a);
      const Vec3 u = std::fabs(ax.y) > 0.9f ? Vec3{1, 0, 0} : render3d::normalize(render3d::cross(ax, {0, 1, 0}));
      const Vec3 v = render3d::cross(ax, u);
      ImVec2 prev;
      bool have = false;
      for (int i = 0; i <= 48; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / 48.0f;
        float sx, sy;
        if (!cam.project(o + (u * std::cos(t) + v * std::sin(t)) * (len * 0.8f), vw, vh, &sx, &sy)) {
          have = false;
          continue;
        }
        const ImVec2 p(x0 + sx, y0 + sy);
        if (have) dl->AddLine(prev, p, axis_color(a), th);
        prev = p;
        have = true;
      }
      continue;
    }
    float ex, ey;
    if (!cam.project(o + Gizmo3D::axis_dir(a) * len, vw, vh, &ex, &ey)) continue;
    dl->AddLine(ImVec2(x0 + ox, y0 + oy), ImVec2(x0 + ex, y0 + ey), axis_color(a), th);
    if (gizmo_.mode == GizmoMode::Scale)
      dl->AddRectFilled(ImVec2(x0 + ex - 5, y0 + ey - 5), ImVec2(x0 + ex + 5, y0 + ey + 5), axis_color(a));
    else
      dl->AddCircleFilled(ImVec2(x0 + ex, y0 + ey), 5.5f, axis_color(a));
  }
  dl->AddCircleFilled(ImVec2(x0 + ox, y0 + oy), 3.5f, IM_COL32(240, 230, 200, 255));
}

void Editor3DScreen::draw_hierarchy() {
  if (theme::SecondaryButton("+ Cube", ImVec2(70, 0))) {
    const Vec3 t = scene_.editor_camera.target;
    create_primitive("cube", {t.x, 0.0f, t.z});
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("+ Light", ImVec2(70, 0))) {
    const Vec3 t = scene_.editor_camera.target;
    create_light(false, {t.x, 2.5f, t.z});
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("+ Empty", ImVec2(70, 0))) {
    const Vec3 t = scene_.editor_camera.target;
    create_empty({t.x, 0.0f, t.z});
  }
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##Hier3DFilter", "Search entities...", hier_filter_,
                           sizeof(hier_filter_));
  ImGui::Separator();
  const bool filtering = hier_filter_[0] != '\0';
  hier_open_context_ = false;
  hier_drop_id_ = 0;
  hier_drop_root_ = false;
  hier_toggle_lock_ = hier_toggle_hide_ = 0;
  std::string needle = hier_filter_;
  for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const scene3d::Entity3D& e : scene_.entities) {
    if (filtering) {
      std::string name = e.name;
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name.find(needle) != std::string::npos) draw_hierarchy_row(e, 0);
    } else if (e.parent == 0) {
      draw_hierarchy_row(e, 0);
    }
  }
  // Empty space below the rows: drop = unparent, RMB = create menu.
  const ImVec2 rest = ImGui::GetContentRegionAvail();
  if (rest.x > 1.0f && rest.y > 1.0f) {
    ImGui::InvisibleButton("##Hier3DEmpty", rest);
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TS_ENTITY3D")) {
        hier_drop_id_ = *static_cast<const std::uint64_t*>(p->Data);
        hier_drop_root_ = true;
      }
      ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      context_id_ = 0;
      hier_open_context_ = true;
    }
  }
  if (!playing()) {
    if (hier_toggle_lock_) {
      if (const scene3d::Entity3D* e = find(hier_toggle_lock_)) set_locked(e->id, !e->locked);
    }
    if (hier_toggle_hide_) {
      if (const scene3d::Entity3D* e = find(hier_toggle_hide_)) set_hidden(e->id, !e->editor_hidden);
    }
    if (hier_drop_id_) {
      if (hier_drop_root_) set_parent(hier_drop_id_, 0);
      else hierarchy_drop(hier_drop_id_, hier_drop_target_, hier_drop_where_);
    }
  }
  if (hier_open_context_) ImGui::OpenPopup("##Hier3DContext");
  if (ImGui::BeginPopup("##Hier3DContext")) {
    draw_context_menu(context_id_);
    ImGui::EndPopup();
  }
}

void Editor3DScreen::draw_hierarchy_row(const scene3d::Entity3D& e, int depth) {
  const bool sel = std::find(selection().begin(), selection().end(), e.id) != selection().end();
  const bool filtering = hier_filter_[0] != '\0';
  const std::vector<std::uint64_t> kids = filtering ? std::vector<std::uint64_t>{} : children_of(e.id);
  ImGui::PushID(static_cast<int>(e.id));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, e.editor_hidden ? theme::TextMuted() : theme::Sand());
  if (ImGui::SmallButton(e.editor_hidden ? "-##eye" : "o##eye")) hier_toggle_hide_ = e.id;
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(e.editor_hidden ? "Show (Shift+H)" : "Hide in the editor (H)");
  ImGui::SameLine(0.0f, 2.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, e.locked ? theme::Warning() : theme::TextMuted());
  if (ImGui::SmallButton(e.locked ? "L##lock" : ".##lock")) hier_toggle_lock_ = e.id;
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(e.locked ? "Unlock (Ctrl+Shift+L)" : "Lock (Ctrl+L)");
  ImGui::PopID();
  ImGui::SameLine(0.0f, 4.0f);

  std::string label = e.name;
  if (e.player) label += "  (rider)";
  else if (e.light) label += "  (light)";
  else if (e.script) label += "  (script)";
  else if (!kids.empty() && !e.mesh) label += "  (group)";
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_DefaultOpen;
  if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  if (sel) flags |= ImGuiTreeNodeFlags_Selected;
  const bool dim = !e.visible || effectively_hidden(e);
  if (dim) ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(e.id)),
                                      flags, "%s", label.c_str());
  if (dim) ImGui::PopStyleColor();
  const ImVec2 mn = ImGui::GetItemRectMin();
  const ImVec2 mxr = ImGui::GetItemRectMax();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
    select(e.id, ImGui::GetIO().KeyCtrl);
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    select(e.id);
    frame_selection();
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    if (!sel) select(e.id);
    context_id_ = e.id;
    hier_open_context_ = true;
  }
  if (!playing() && ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("TS_ENTITY3D", &e.id, sizeof(e.id));
    ImGui::TextUnformatted(e.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload)) {
      drop_asset(static_cast<const char*>(p->Data), e.transform.position, e.id);
    }
    // Top quarter: before, bottom quarter: after, middle: make a child.
    const float h = mxr.y - mn.y;
    const float my = ImGui::GetIO().MousePos.y - mn.y;
    const int where = my < h * 0.25f ? 0 : (my > h * 0.75f ? 1 : 2);
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(
            "TS_ENTITY3D", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      hier_drop_id_ = *static_cast<const std::uint64_t*>(p->Data);
      hier_drop_target_ = e.id;
      hier_drop_where_ = where;
    }
    const ImGuiPayload* live = ImGui::GetDragDropPayload();
    if (live && live->IsDataType("TS_ENTITY3D")) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 col = theme::U32(theme::Accent());
      if (where == 0) dl->AddLine(mn, ImVec2(mxr.x, mn.y), col, 2.0f);
      else if (where == 1) dl->AddLine(ImVec2(mn.x, mxr.y), mxr, col, 2.0f);
      else dl->AddRect(mn, mxr, col, 2.0f, 0, 1.5f);
    }
    ImGui::EndDragDropTarget();
  }
  if (open && !kids.empty()) {
    for (std::uint64_t k : kids) {
      if (const scene3d::Entity3D* c = find(k)) draw_hierarchy_row(*c, depth + 1);
    }
    ImGui::TreePop();
  }
}

void Editor3DScreen::draw_context_menu(std::uint64_t clicked) {
  const scene3d::Entity3D* e = find(clicked);
  const bool has = !selection().empty();
  if (e) {
    ImGui::TextDisabled("%s", e->name.c_str());
    ImGui::Separator();
  }
  if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, has)) duplicate_selected();
  if (ImGui::MenuItem("Delete", "Del", false, has)) delete_selected();
  if (e && ImGui::BeginMenu("Rename")) {
    if (ImGui::IsWindowAppearing()) std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", e->name.c_str());
    if (ImGui::InputText("##ctxrename", rename_buf_, sizeof(rename_buf_),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
      rename(e->id, rename_buf_);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Copy", "Ctrl+C", false, has)) copy_selected();
  if (ImGui::MenuItem("Cut", "Ctrl+X", false, has)) cut_selected();
  if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboard_text().empty())) paste();
  ImGui::Separator();
  if (ImGui::MenuItem("Group", "Ctrl+G", false, has)) group_selected();
  if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, has)) ungroup_selected();
  if (e && e->parent != 0 && ImGui::MenuItem("Unparent")) set_parent(e->id, 0);
  if (e) {
    if (ImGui::MenuItem(e->locked ? "Unlock" : "Lock", "Ctrl+L")) set_locked(e->id, !e->locked);
    if (ImGui::MenuItem(e->editor_hidden ? "Show" : "Hide", "H")) set_hidden(e->id, !e->editor_hidden);
  }
  if (ImGui::MenuItem("Focus", "F", false, has)) frame_selection();
  if (ImGui::MenuItem("Drop to ground", "End", false, has)) drop_to_ground();
  if (selection().size() >= 2 && ImGui::BeginMenu("Align")) {
    static const char* kAx[3] = {"X", "Y", "Z"};
    for (int a = 0; a < 3; ++a) {
      if (ImGui::MenuItem((std::string("Min ") + kAx[a]).c_str())) align_selected(a, -1);
      if (ImGui::MenuItem((std::string("Centre ") + kAx[a]).c_str())) align_selected(a, 0);
      if (ImGui::MenuItem((std::string("Max ") + kAx[a]).c_str())) align_selected(a, 1);
      if (ImGui::MenuItem((std::string("Distribute ") + kAx[a]).c_str(), nullptr, false,
                          selection().size() >= 3)) {
        distribute_selected(a);
      }
      if (a < 2) ImGui::Separator();
    }
    ImGui::EndMenu();
  }
  if (e && ImGui::BeginMenu("Add component")) {
    scene3d::Entity3D c = *e;
    bool add = false;
    if (ImGui::MenuItem("Mesh", nullptr, false, !e->mesh)) { c.mesh = scene3d::MeshRef{}; add = true; }
    if (ImGui::MenuItem("Material", nullptr, false, !e->material)) { c.material = scene3d::Material3D{}; add = true; }
    if (ImGui::MenuItem("Light", nullptr, false, !e->light)) { c.light = scene3d::Light3D{}; add = true; }
    if (ImGui::MenuItem("Collider", nullptr, false, !e->collider)) { c.collider = scene3d::Collider3D{}; add = true; }
    if (ImGui::MenuItem("Grabbable", nullptr, false, !e->grabbable)) { c.grabbable = scene3d::Grabbable3D{}; add = true; }
    if (ImGui::MenuItem("Audio Source", nullptr, false, !e->audio)) { c.audio = scene3d::audio_3d(); add = true; }
    if (ImGui::MenuItem("Particles", nullptr, false, !e->particles)) { c.particles = fx::preset("campfire", fx::Space::ThreeD); add = true; }
    if (add) replace_entity(c, "Add component " + c.name);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::BeginMenu("Create")) {
    const Vec3 at = scene_.editor_camera.target;
    for (const char* p : {"cube", "plane", "sphere", "capsule"})
      if (ImGui::MenuItem(p)) create_primitive(p, {at.x, 0.0f, at.z});
    if (ImGui::MenuItem("Point light")) create_light(false, {at.x, 2.5f, at.z});
    if (ImGui::MenuItem("Empty")) create_empty({at.x, 0.0f, at.z});
    ImGui::EndMenu();
  }
}

void Editor3DScreen::draw_assets() {
  if (theme::SecondaryButton("Rescan", ImVec2(70, 0))) rescan_assets();
  ImGui::SameLine();
  ImGui::TextDisabled("drag into the viewport");
  ImGui::Separator();
  for (const AssetEntry& a : assets_) {
    const char* tag = a.rel.rfind("primitive:", 0) == 0 ? "[prim] " : (a.model ? "[model] " : "[image] ");
    const std::string label = std::string(tag) + a.label;
    ImGui::Selectable(label.c_str(), false);
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(kDragPayload, a.rel.c_str(), a.rel.size() + 1);
      ImGui::TextUnformatted(a.label.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && a.model) {
      const Vec3 t = scene_.editor_camera.target;
      drop_asset(a.rel, {t.x, 0.0f, t.z});
    }
  }
  if (!scripts_.empty()) {
    theme::SectionHeader("Scripts");
    for (const std::string& s : scripts_) ImGui::TextUnformatted(s.c_str());
  }
}

void Editor3DScreen::draw_telegraph() {
  for (const runtime::LogEntry& e : log_) {
    const ImVec4 col = e.level == runtime::LogLevel::Error  ? theme::Danger()
                       : e.level == runtime::LogLevel::Warn ? theme::Warning()
                                                            : theme::TextMuted();
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", runtime::format_log(e).c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
  if (plugins_.loaded("ts.profiler")) {
    std::string echo, out;
    if (draw_console_line(telegraph_console_, &echo, &out)) {
      log_.push_back(runtime::LogEntry{runtime::LogLevel::Info, "console", echo});
      if (!out.empty()) log_.push_back(runtime::LogEntry{runtime::LogLevel::Info, "console", out});
    }
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
