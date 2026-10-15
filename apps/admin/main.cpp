#include "core/Engine.h"
#include "editor/audio/EditorAudio.h"
#include "editor/AppFlow.h"
#include "editor/AppState.h"
#include "editor/assets/AssetLibrary.h"
#include "editor/assets/TextureCache.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/settings/Settings.h"
#include "editor/workspace/SceneIO.h"
#include "editor/ui/Brand.h"
#include "editor/ui/Theme.h"
#include "editor/workspace/Workspace2D.h"
#include "gfx/GlTextureUploader.h"

#include "Smoke3D.h"
#include "SmokeTools.h"
#include "SmokeVR.h"
#include "SmokePlugins.h"
#include "SmokeAudio.h"
#include "ExportCli.h"
#include "SmokeGraph.h"
#include "SmokeCSharp.h"
#include "SmokePhysics.h"
#include "editor/scripting/BrandBook.h"
#include "SmokeNative.h"
#include "SmokeFx.h"
#include "SmokeUi.h"
#include "SmokeShipyard.h"
#include "SmokeSurvival.h"
#include "SmokeRuntime.h"
#include "SmokeAI.h"
#include "SmokeStory.h"
#include "SmokeAnim3D.h"
#include "SmokeTemplates.h"
#include "SmokeTradingPost.h"
#include "store/TradingPost.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// GLFW will pull platform OpenGL headers (do not define GLFW_INCLUDE_NONE here).
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ts::tombstone;
using editor::AppFlow;
using editor::AppState;
using editor::Settings;
using editor::SettingsStore;

void apply_theme(const Settings& settings) {
  editor::theme::Apply(settings.theme);
}

// Tiny uncompressed 32-bit TGA writer so --smoke can make test images
// without any extra dependency (stb_image decodes TGA).
bool write_test_tga(const std::filesystem::path& path, int w, int h,
                    std::uint32_t (*pixel)(int x, int y)) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  unsigned char header[18] = {};
  header[2] = 2;  // uncompressed true-colour
  header[12] = static_cast<unsigned char>(w & 0xff);
  header[13] = static_cast<unsigned char>((w >> 8) & 0xff);
  header[14] = static_cast<unsigned char>(h & 0xff);
  header[15] = static_cast<unsigned char>((h >> 8) & 0xff);
  header[16] = 32;
  header[17] = 0x28;  // top-left origin, 8 alpha bits
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::uint32_t rgba = pixel(x, y);
      const unsigned char bgra[4] = {
          static_cast<unsigned char>((rgba >> 8) & 0xff),
          static_cast<unsigned char>((rgba >> 16) & 0xff),
          static_cast<unsigned char>((rgba >> 24) & 0xff),
          static_cast<unsigned char>(rgba & 0xff)};
      out.write(reinterpret_cast<const char*>(bgra), 4);
    }
  }
  return static_cast<bool>(out);
}

int run_console_smoke() {
  namespace fs = std::filesystem;

  std::cout << "TombStone Admin (--smoke console path)\n";

  const fs::path smoke_root = "./TombStoneProjects";
  const fs::path smoke_config = "./TombStoneConfig";
  std::error_code ec;
  fs::remove_all(smoke_root, ec);
  fs::remove_all(smoke_config, ec);

  {
    Settings seed = SettingsStore::make_defaults();
    seed.projects_root = SettingsStore::default_projects_root();
    seed.username = "dev";
    seed.auto_login_dev = false;
    seed.theme = "dark";
    if (!SettingsStore::save(seed)) {
      std::cerr << "Failed to write seed settings\n";
      return 1;
    }
  }

  Engine engine;
  if (!engine.init()) {
    std::cerr << "Engine init failed\n";
    return 1;
  }

  // The sound card opens with the window (TS_AUDIO=null keeps it quiet).
  editor::EditorAudio::get().start(true);
  AppFlow flow;
  flow.start();

  constexpr float kDt = 1.0f / 60.0f;

  // Loading splash is longer with ImGui; budget enough ticks.
  for (int i = 0; i < 200 && flow.current_state() == AppState::Loading; ++i) {
    engine.tick(kDt);
    flow.tick(kDt);
  }

  if (flow.current_state() != AppState::Login) {
    std::cerr << "Expected Login after Loading\n";
    engine.shutdown();
    return 1;
  }

  if (!flow.open_settings_from_login()) {
    std::cerr << "Failed to open Settings from Login\n";
    engine.shutdown();
    return 1;
  }
  if (!flow.cancel_settings() || flow.current_state() != AppState::Login) {
    std::cerr << "Expected return to Login after cancel Settings\n";
    engine.shutdown();
    return 1;
  }

  if (!flow.submit_dev_login()) {
    std::cerr << "DEV_LOGIN failed\n";
    engine.shutdown();
    return 1;
  }

  if (flow.current_state() != AppState::ProjectManager) {
    std::cerr << "Expected ProjectManager after login\n";
    engine.shutdown();
    return 1;
  }

  // Samples are templates now: a fresh root starts empty, and the Break
  // Ground picker creates claims from templates.
  if (!flow.projects().empty()) {
    std::cerr << "Expected an empty projects root (no seeded samples)\n";
    engine.shutdown();
    return 1;
  }
  if (flow.templates().templates().size() < 7 ||
      !flow.create_from_template("Frontier Smoke", "frontier-survival") ||
      !flow.create_from_template("Saloon Smoke", "haunted-saloon-vr") ||
      flow.projects().size() != 2) {
    std::cerr << "Template create failed: " << flow.last_error() << '\n';
    engine.shutdown();
    return 1;
  }

  if (!flow.create_new_project_2d("Smoke Test 2D")) {
    std::cerr << "Failed to create 2D project: " << flow.last_error() << '\n';
    engine.shutdown();
    return 1;
  }

  // Collision / validation polish checks.
  if (flow.create_new_project_2d("Smoke Test 2D")) {
    std::cerr << "Expected collision failure for duplicate project name\n";
    engine.shutdown();
    return 1;
  }
  if (flow.create_new_project_2d("bad/name")) {
    std::cerr << "Expected validation failure for unsafe project name\n";
    engine.shutdown();
    return 1;
  }

  if (!flow.open_settings_from_projects()) {
    std::cerr << "Failed to open Settings from ProjectManager\n";
    engine.shutdown();
    return 1;
  }
  if (!flow.settings_set_theme("light")) {
    std::cerr << "Failed to set theme draft\n";
    engine.shutdown();
    return 1;
  }
  if (!flow.apply_settings_draft() ||
      flow.current_state() != AppState::ProjectManager) {
    std::cerr << "Expected return to ProjectManager after apply Settings\n";
    engine.shutdown();
    return 1;
  }
  if (flow.settings().theme != "light") {
    std::cerr << "Expected theme=light after Settings apply\n";
    engine.shutdown();
    return 1;
  }

  bool opened = false;
  for (std::size_t i = 0; i < flow.projects().size(); ++i) {
    // Claims sort by name (Frontier Smoke first); this walk-through is 2D.
    if (flow.projects()[i].kind != ts::tombstone::editor::ProjectKind::TwoD) continue;
    if (flow.select_project(i)) {
      opened = true;
      break;
    }
  }
  if (!opened) {
    std::cerr << "Failed to open a 2D project\n";
    engine.shutdown();
    return 1;
  }

  if (flow.current_state() != AppState::Editor2D) {
    std::cerr << "Expected Editor2D after selecting 2D project\n";
    engine.shutdown();
    return 1;
  }

  engine.tick(kDt);
  flow.tick(kDt);

  // scene.json should exist after opening Editor2D (load or seed+write).
  const std::string opened_path =
      flow.active_project() ? flow.active_project()->path : std::string{};
  if (opened_path.empty()) {
    std::cerr << "Expected active project path in Editor2D\n";
    engine.shutdown();
    return 1;
  }
  const fs::path scene_file =
      fs::path(opened_path) / "scene.json";
  if (!fs::exists(scene_file)) {
    std::cerr << "Expected scene.json after opening Editor2D: " << scene_file
              << '\n';
    engine.shutdown();
    return 1;
  }

  editor::Workspace2D* ws = flow.editor_workspace();
  if (!ws) {
    std::cerr << "Expected editor workspace pointer\n";
    engine.shutdown();
    return 1;
  }
  const std::size_t before_count = ws->entities().size();
  const std::uint64_t smoke_id = ws->create_entity("SmokePersist");
  if (smoke_id == 0) {
    std::cerr << "Failed to create SmokePersist entity\n";
    engine.shutdown();
    return 1;
  }
  if (editor::Entity2D* e = ws->find(smoke_id)) {
    e->x = 321.25f;
    e->y = 42.5f;
    e->layer = 7;
  }
  if (!flow.editor_save_scene()) {
    std::cerr << "editor_save_scene failed\n";
    engine.shutdown();
    return 1;
  }
  if (ws->entities().size() != before_count + 1) {
    std::cerr << "Expected entity count to grow after create\n";
    engine.shutdown();
    return 1;
  }

  flow.request_back_to_projects();
  if (flow.current_state() != AppState::ProjectManager) {
    std::cerr << "Expected ProjectManager after back from Editor2D\n";
    engine.shutdown();
    return 1;
  }

  // Re-open same project and verify hierarchy restored from scene.json.
  bool reopened = false;
  for (std::size_t i = 0; i < flow.projects().size(); ++i) {
    if (flow.projects()[i].path == opened_path) {
      if (flow.select_project(i)) {
        reopened = true;
      }
      break;
    }
  }
  if (!reopened || flow.current_state() != AppState::Editor2D) {
    std::cerr << "Failed to reopen project for scene.json roundtrip\n";
    engine.shutdown();
    return 1;
  }
  editor::Workspace2D* ws2 = flow.editor_workspace();
  if (!ws2) {
    std::cerr << "Expected workspace after reopen\n";
    engine.shutdown();
    return 1;
  }
  const editor::Entity2D* persisted = nullptr;
  for (const editor::Entity2D& e : ws2->entities()) {
    if (e.name == "SmokePersist") {
      persisted = &e;
      break;
    }
  }
  if (!persisted) {
    std::cerr << "SmokePersist entity missing after reload from scene.json\n";
    engine.shutdown();
    return 1;
  }
  if (persisted->x < 321.0f || persisted->x > 321.5f || persisted->layer != 7) {
    std::cerr << "SmokePersist fields not restored (x=" << persisted->x
              << " layer=" << persisted->layer << ")\n";
    engine.shutdown();
    return 1;
  }
  if (ws2->entities().size() != before_count + 1) {
    std::cerr << "Entity count mismatch after scene.json reload\n";
    engine.shutdown();
    return 1;
  }
  if (ws2->can_undo() || ws2->can_redo()) {
    std::cerr << "Undo history should be empty after opening a project\n";
    engine.shutdown();
    return 1;
  }

  // Editor tools through the screen: Ctrl+D / Del paths must autosave.
  {
    ws2->select(std::nullopt);
    for (const editor::Entity2D& e : ws2->entities()) {
      if (e.name == "SmokePersist") {
        ws2->select(e.id);
        break;
      }
    }
    if (flow.editor_duplicate_selected() != 1) {
      std::cerr << "editor_duplicate_selected expected 1 copy\n";
      engine.shutdown();
      return 1;
    }
    editor::Workspace2D on_disk;
    std::string err;
    if (!editor::scene_io::load(on_disk, scene_file.string(), &err) ||
        on_disk.entities().size() != before_count + 2) {
      std::cerr << "Duplicate was not autosaved to scene.json: " << err << '\n';
      engine.shutdown();
      return 1;
    }
    bool copy_found = false;
    for (const editor::Entity2D& e : on_disk.entities()) {
      if (e.name == "SmokePersist 2" && e.layer == 7) {
        copy_found = true;
      }
    }
    if (!copy_found) {
      std::cerr << "Expected \"SmokePersist 2\" copy in scene.json\n";
      engine.shutdown();
      return 1;
    }
    if (flow.editor_delete_selected() != 1) {
      std::cerr << "editor_delete_selected expected 1 removal\n";
      engine.shutdown();
      return 1;
    }
    editor::Workspace2D after_delete;
    if (!editor::scene_io::load(after_delete, scene_file.string(), &err) ||
        after_delete.entities().size() != before_count + 1) {
      std::cerr << "Delete was not autosaved to scene.json\n";
      engine.shutdown();
      return 1;
    }
    std::cout << "[smoke] duplicate/delete autosave OK\n";
  }

  // Undo / redo through the screen: create, drag-move, duplicate, nudge,
  // Inspector edit, delete -> undo all, redo all, new edit clears redo, and
  // scene.json follows every step.
  {
    auto fail = [&](const std::string& what) {
      std::cerr << "Undo/redo smoke failed: " << what << '\n';
      engine.shutdown();
      return 1;
    };
    // Reopen so history starts empty (history is per open project).
    flow.request_back_to_projects();
    bool again = false;
    for (std::size_t i = 0; i < flow.projects().size(); ++i) {
      if (flow.projects()[i].path == opened_path) {
        again = flow.select_project(i);
        break;
      }
    }
    editor::Editor2DScreen* scr = flow.editor_screen();
    if (!again || !scr) return fail("reopen project");
    editor::Workspace2D& w = scr->workspace();
    if (w.can_undo() || w.can_redo()) return fail("history not cleared on open");
    if (flow.editor_undo()) return fail("undo on empty history");

    auto disk_matches = [&](const editor::Workspace2D& ref) {
      editor::Workspace2D d;
      std::string err;
      if (!editor::scene_io::load(d, scene_file.string(), &err) ||
          d.entities() != ref.entities()) {
        return false;
      }
      // An empty selection reloads as "first entity", so only compare a
      // real one.
      return ref.selection_count() == 0 ||
             (d.selection() == ref.selection() &&
              d.selected_id() == ref.selected_id());
    };
    auto same_state = [](const editor::Workspace2D& a,
                         const editor::Workspace2D::Snapshot& s) {
      return a.entities() == s.entities && a.selection() == s.selection &&
             a.selected_id() == s.selected_id;
    };

    w.set_snap_enabled(false);
    const editor::Workspace2D::Snapshot initial = w.snapshot();
    std::vector<editor::Workspace2D::Snapshot> states{initial};
    std::vector<std::string> labels;

    // 1) Create.
    const std::uint64_t crate = scr->create_entity("UndoCrate");
    if (crate == 0) return fail("create");
    states.push_back(w.snapshot());
    labels.push_back(w.undo_label());

    // 2) Group drag-move: many update frames, one step.
    std::uint64_t other = 0;
    for (const editor::Entity2D& e : w.entities()) {
      if (e.id != crate) {
        other = e.id;
        break;
      }
    }
    w.select(other);
    w.add_to_selection(crate);
    // Selection-only changes are not steps; undo restores the selection as
    // it was when the next edit started.
    states.back() = w.snapshot();
    const float crate_x0 = w.find(crate)->x;
    scr->begin_drag_move();
    for (int f = 1; f <= 12; ++f) {
      w.update_move(5.0f * static_cast<float>(f), 2.0f * static_cast<float>(f));
    }
    if (!scr->end_drag_move()) return fail("drag move reported no change");
    if (w.find(crate)->x != crate_x0 + 60.0f) return fail("drag position");
    if (w.undo_count() != 2 || w.undo_label() != "Move 2") {
      return fail("drag should be ONE step labelled \"Move 2\" (got \"" +
                  w.undo_label() + "\")");
    }
    if (!disk_matches(w)) return fail("move not autosaved");
    states.push_back(w.snapshot());
    labels.push_back(w.undo_label());

    // A cancelled drag (Esc) leaves no step.
    scr->begin_drag_move();
    w.update_move(40.0f, 40.0f);
    scr->cancel_drag_move();
    if (w.undo_count() != 2 || !same_state(w, states.back())) {
      return fail("cancelled drag should not add a step");
    }

    // 3) Duplicate.
    if (scr->duplicate_selected() != 2) return fail("duplicate");
    if (w.undo_label() != "Duplicate 2") return fail("duplicate label");
    states.push_back(w.snapshot());
    labels.push_back(w.undo_label());

    // 4) Held-arrow nudge: 5 repeats coalesce into one step.
    for (int i = 0; i < 5; ++i) {
      scr->nudge_selected(1, 0, false);
    }
    if (w.undo_count() != 3 || !w.edit_open()) {
      return fail("nudges should coalesce while held");
    }
    scr->flush_pending_edit();  // keys released + window elapsed
    if (w.undo_count() != 4 || w.undo_label() != "Nudge 2") {
      return fail("nudge should be ONE step");
    }
    states.push_back(w.snapshot());
    labels.push_back(w.undo_label());

    // 5) Inspector field: several value changes while active = one step.
    w.select(crate);
    states.back() = w.snapshot();
    scr->begin_inspector_edit("Edit X");
    for (int i = 0; i < 6; ++i) {
      w.find(crate)->x += 3.5f;
    }
    w.find(crate)->color[1] = 0.125f;  // same activation, still one step
    if (!scr->end_inspector_edit()) return fail("inspector edit not pushed");
    if (w.undo_count() != 5 || w.undo_label() != "Edit X") {
      return fail("inspector edit should be ONE step");
    }
    if (!disk_matches(w)) return fail("inspector edit not autosaved");
    // Activate + deactivate without change: no step.
    scr->begin_inspector_edit("Edit Y");
    scr->end_inspector_edit();
    if (w.undo_count() != 5) return fail("no-op inspector edit added a step");
    states.push_back(w.snapshot());
    labels.push_back(w.undo_label());

    // 6) Delete (multi).
    w.select(crate);
    w.add_to_selection(other);
    if (w.can_redo() || w.undo_count() != 5) return fail("selection made a step");
    states.back() = w.snapshot();
    if (scr->delete_selected() != 2) return fail("delete");
    states.push_back(w.snapshot());
    labels.push_back(w.undo_label());
    if (w.undo_count() != 6 || w.can_redo()) return fail("6 steps expected");

    // Undo everything, checking each intermediate state + scene.json.
    for (std::size_t k = states.size() - 1; k > 0; --k) {
      if (w.undo_label() != labels[k - 1]) return fail("undo label order");
      if (!flow.editor_undo()) return fail("undo returned false");
      if (!same_state(w, states[k - 1])) {
        return fail("state after undo of \"" + labels[k - 1] + "\"");
      }
      if (!disk_matches(w)) {
        return fail("scene.json after undo of \"" + labels[k - 1] + "\"");
      }
    }
    if (w.can_undo() || w.redo_count() != 6) return fail("undo depth");
    if (!same_state(w, initial)) return fail("undo-all != initial");
    if (flow.editor_undo()) return fail("undo past start");

    // Redo everything.
    for (std::size_t k = 1; k < states.size(); ++k) {
      if (w.redo_label() != labels[k - 1]) return fail("redo label order");
      if (!flow.editor_redo()) return fail("redo returned false");
      if (!same_state(w, states[k]) || !disk_matches(w)) {
        return fail("state after redo of \"" + labels[k - 1] + "\"");
      }
    }
    if (w.can_redo() || w.undo_count() != 6) return fail("redo depth");
    if (flow.editor_redo()) return fail("redo past end");

    // Undo two, then a new edit clears the redo stack.
    flow.editor_undo();
    flow.editor_undo();
    if (w.redo_count() != 2) return fail("redo stack after 2 undos");
    if (scr->create_entity("FreshClaim") == 0) return fail("create after undo");
    if (w.can_redo() || flow.editor_redo()) return fail("new edit must clear redo");
    if (w.undo_count() != 5 || w.undo_label() != "Create Entity") {
      return fail("history after branch");
    }
    if (!disk_matches(w)) return fail("scene.json after branch");

    // Undo back to the start one more time and confirm disk state.
    while (flow.editor_undo()) {
    }
    if (!same_state(w, initial) || !disk_matches(w)) {
      return fail("undo-all after branch");
    }

    // History cap (headless workspace).
    editor::Workspace2D cap;
    for (int i = 0; i < 260; ++i) {
      editor::Workspace2D::Snapshot before = cap.snapshot();
      cap.create_entity("Cap");
      cap.commit_step("Create Entity", std::move(before));
    }
    if (cap.undo_count() != editor::Workspace2D::kMaxHistory) {
      return fail("history cap");
    }
    std::cout << "[smoke] undo/redo OK (6 steps: create, move, duplicate, "
                 "nudge, inspector, delete; undo-all, redo-all, redo cleared "
                 "by new edit, scene.json in sync, cap "
              << editor::Workspace2D::kMaxHistory << ")\n";
  }

  // TileMap painting + sprites/assets through the screen (headless, no GL):
  // paint stroke = one step, brush size, erase, bucket fill, rect fill,
  // cancelled stroke, resize keeps tiles, tile size, eyedropper, undo-all /
  // redo-all, scene.json v2 roundtrip, v1 upgrade, sprite assign / flip /
  // source rect roundtrip, missing + corrupt image fallback, import, tileset.
  {
    auto fail = [&](const std::string& what) {
      std::cerr << "TileMap/sprite smoke failed: " << what << '\n';
      engine.shutdown();
      return 1;
    };
    if (!fs::is_directory(smoke_root / "smoke-test-2d" / "assets")) {
      return fail("new project should get an assets/ folder");
    }
    flow.request_back_to_projects();
    bool again = false;
    for (std::size_t i = 0; i < flow.projects().size(); ++i) {
      if (flow.projects()[i].path == opened_path) {
        again = flow.select_project(i);
        break;
      }
    }
    editor::Editor2DScreen* scr = flow.editor_screen();
    if (!again || !scr) return fail("reopen project");
    if (!fs::is_directory(fs::path(opened_path) / "assets")) {
      return fail("assets/ folder not created on open");
    }
    editor::Workspace2D& w = scr->workspace();
    w.set_snap_enabled(false);
    auto disk_matches = [&]() {
      editor::Workspace2D d;
      std::string err;
      return editor::scene_io::load(d, scene_file.string(), &err) &&
             d.entities() == w.entities();
    };
    const editor::Workspace2D::Snapshot initial = w.snapshot();

    // Seeded defaults carry a real, pre-painted TileMap.
    {
      editor::Workspace2D seed;
      std::uint64_t seeded = 0;
      for (const editor::Entity2D& e : seed.entities()) {
        if (e.name == "TileMap" && e.tilemap) seeded = e.id;
      }
      if (seeded == 0 || seed.tile_at(seeded, 0, 0) != 4 ||
          seed.tile_at(seeded, 7, 1) != 1 || seed.find(seeded)->w != 256.0f) {
        return fail("seeded TileMap should be an 8x2 painted grid");
      }
    }

    const std::uint64_t tm = scr->create_tilemap();  // 16 x 8, 32 px
    if (tm == 0 || w.undo_count() != 1 || scr->paint_target() != tm) {
      return fail("create_tilemap");
    }
    const editor::Entity2D* tme = w.find(tm);
    if (!tme->tilemap || tme->w != 512.0f || tme->h != 256.0f) {
      return fail("tilemap extent");
    }
    scr->set_tool(editor::TileTool::Paint);
    if (scr->tool() != editor::TileTool::Paint) return fail("set_tool");

    // 1) One stroke over many cells (with a gap the line fills) = one step.
    scr->set_brush_tile(5);
    if (!scr->begin_paint_stroke(tm, false)) return fail("begin stroke");
    scr->stroke_to_cell(0, 0);
    scr->stroke_to_cell(5, 0);  // jump: Bresenham fills 1..4
    scr->stroke_to_cell(5, 0);  // same cell again: no-op
    scr->stroke_to_cell(5, 3);
    scr->stroke_to_cell(40, 3);  // off the grid: clipped
    if (!scr->end_paint_stroke()) return fail("end stroke pushed nothing");
    if (w.undo_count() != 2 || w.undo_label() != "Paint 19 tiles") {
      return fail("stroke should be ONE step \"Paint 19 tiles\" (got \"" +
                  w.undo_label() + "\")");
    }
    for (int c = 0; c <= 5; ++c) {
      if (w.tile_at(tm, c, 0) != 5) return fail("stroke row 0");
    }
    if (w.tile_at(tm, 5, 2) != 5 || w.tile_at(tm, 15, 3) != 5 ||
        w.tile_at(tm, 6, 1) != 0) {
      return fail("stroke cells");
    }
    if (!disk_matches()) return fail("stroke not autosaved");

    // 2) Brush size 2.
    scr->set_brush_size(2);
    scr->begin_paint_stroke(tm, false);
    scr->stroke_to_cell(10, 5);
    scr->end_paint_stroke();
    scr->set_brush_size(1);
    if (w.tile_at(tm, 10, 5) != 5 || w.tile_at(tm, 11, 6) != 5 ||
        w.tile_at(tm, 12, 5) != 0 || w.undo_label() != "Paint 4 tiles") {
      return fail("brush size 2");
    }

    // Undo / redo the 2x2 dab.
    if (!flow.editor_undo() || w.tile_at(tm, 10, 5) != 0 ||
        w.tile_at(tm, 0, 0) != 5 || !disk_matches()) {
      return fail("undo stroke");
    }
    if (!flow.editor_redo() || w.tile_at(tm, 11, 6) != 5 || !disk_matches()) {
      return fail("redo stroke");
    }

    // 3) Erase stroke.
    scr->begin_paint_stroke(tm, true);
    scr->stroke_to_cell(2, 0);
    scr->end_paint_stroke();
    if (w.tile_at(tm, 2, 0) != 0 || w.undo_label() != "Erase 1 tile") {
      return fail("erase");
    }

    // A stroke that paints nothing new pushes no step; Esc restores.
    const std::size_t steps_before = w.undo_count();
    scr->begin_paint_stroke(tm, false);
    scr->stroke_to_cell(0, 0);  // already 5
    scr->end_paint_stroke();
    if (w.undo_count() != steps_before) return fail("no-op stroke added a step");
    const editor::Workspace2D::Snapshot pre_cancel = w.snapshot();
    scr->begin_paint_stroke(tm, false);
    scr->stroke_to_cell(8, 7);
    scr->stroke_to_cell(12, 7);
    scr->cancel_paint_stroke();
    if (w.undo_count() != steps_before || w.entities() != pre_cancel.entities) {
      return fail("cancelled stroke should restore tiles and add no step");
    }

    // 4) Bucket fill of the empty region. The stroke's row-3 line walls off
    // cols 6..15 x rows 0..2 (30 cells), which must stay empty.
    const std::size_t empty_cells =
        128 - w.find(tm)->tilemap->count_nonempty();
    scr->set_brush_tile(3);
    const std::size_t filled = scr->fill_at(tm, 8, 7);
    if (filled != empty_cells - 30 ||
        w.find(tm)->tilemap->count_nonempty() != 98 ||
        w.tile_at(tm, 2, 0) != 3 || w.tile_at(tm, 0, 0) != 5 ||
        w.tile_at(tm, 8, 1) != 0) {
      return fail("flood fill (" + std::to_string(filled) + " vs " +
                  std::to_string(empty_cells) + ")");
    }
    if (w.undo_label() != "Fill " + std::to_string(filled) + " tiles" ||
        !disk_matches()) {
      return fail("fill step / autosave");
    }
    if (scr->fill_at(tm, 8, 7) != 0) return fail("fill same tile should no-op");
    if (!flow.editor_undo() || w.tile_at(tm, 8, 7) != 0) return fail("undo fill");
    if (!flow.editor_redo() || w.tile_at(tm, 8, 7) != 3) return fail("redo fill");

    // 5) Rect fill + rect erase.
    scr->set_brush_tile(7);
    if (scr->fill_rect_cells(tm, 3, 7, 0, 6, false) != 8 ||
        w.tile_at(tm, 0, 6) != 7 || w.tile_at(tm, 3, 7) != 7) {
      return fail("rect fill");
    }
    if (scr->fill_rect_cells(tm, 14, 7, 15, 7, true) != 2 ||
        w.tile_at(tm, 15, 7) != 0 || w.tile_at(tm, 13, 7) != 3) {
      return fail("rect erase");
    }

    // 6) Eyedropper.
    if (!scr->eyedrop(tm, 0, 6) || scr->brush_tile() != 7) return fail("eyedrop");
    if (scr->eyedrop(tm, 15, 0)) return fail("eyedrop on empty should fail");

    // 7) Resize keeps the overlap; undo restores the cut tiles.
    const editor::Workspace2D::Snapshot pre_resize = w.snapshot();
    if (!scr->resize_tilemap(tm, 4, 3)) return fail("resize");
    tme = w.find(tm);
    if (tme->tilemap->cols != 4 || tme->tilemap->rows != 3 || tme->w != 128.0f ||
        tme->h != 96.0f || w.tile_at(tm, 0, 0) != 5 || w.tile_at(tm, 2, 0) != 3 ||
        w.undo_label() != "Resize TileMap" || !disk_matches()) {
      return fail("resize keeps tiles");
    }
    if (!flow.editor_undo() || w.entities() != pre_resize.entities) {
      return fail("undo resize");
    }
    flow.editor_redo();
    if (!scr->resize_tilemap(tm, 6, 5) || w.tile_at(tm, 5, 4) != 0 ||
        w.tile_at(tm, 3, 2) != 3 || w.tile_at(tm, 0, 0) != 5) {
      return fail("grow keeps tiles, new cells empty");
    }
    if (scr->resize_tilemap(tm, 6, 5)) return fail("same-size resize is a step");
    if (!scr->set_tile_size(tm, 16) || w.find(tm)->w != 96.0f ||
        w.find(tm)->h != 80.0f) {
      return fail("tile size");
    }

    // 8) scene.json v2 roundtrip of the tile data (RLE).
    {
      editor::Workspace2D d;
      std::string err;
      if (!editor::scene_io::load(d, scene_file.string(), &err)) {
        return fail("load v2: " + err);
      }
      const editor::Entity2D* de = d.find(tm);
      if (!de || !de->tilemap || *de->tilemap != *w.find(tm)->tilemap ||
          de->w != 96.0f) {
        return fail("tilemap save/load roundtrip");
      }
      std::ifstream in(scene_file);
      std::stringstream ss;
      ss << in.rdbuf();
      const std::string current_version =
          "\"version\": " +
          std::to_string(editor::scene_io::kSceneVersion);
      if (ss.str().find(current_version) == std::string::npos ||
          ss.str().find("\"encoding\": \"rle\"") == std::string::npos) {
        return fail("scene.json should be the current version with RLE tiles");
      }
    }
    {
      std::vector<int> tiles = {0, 0, 0, 4, 4, 1, 0, 65535, 65535, 2};
      const std::string rle = editor::tile_codec::encode_rle(tiles);
      std::vector<int> back;
      if (rle != "3*0,2*4,1,0,2*65535,2" ||
          !editor::tile_codec::decode_rle(rle, tiles.size(), &back) ||
          back != tiles) {
        return fail("rle codec (" + rle + ")");
      }
      if (editor::tile_codec::decode_rle("3*x", 3, &back) ||
          back != std::vector<int>({0, 0, 0})) {
        return fail("rle should reject junk");
      }
    }

    // 9) Undo everything back to the open state, then redo it all.
    const editor::Workspace2D::Snapshot painted = w.snapshot();
    const std::size_t depth = w.undo_count();
    while (flow.editor_undo()) {
    }
    if (w.entities() != initial.entities || !disk_matches()) {
      return fail("undo-all should restore the opened scene");
    }
    while (flow.editor_redo()) {
    }
    if (w.undo_count() != depth || w.entities() != painted.entities ||
        !disk_matches()) {
      return fail("redo-all should restore the painted scene");
    }

    // 10) Older files: v1 TileMap rect upgrades to an empty grid; files with
    // no "version" key behave the same.
    {
      const fs::path v1_dir = smoke_root / "_scene_v1";
      fs::create_directories(v1_dir);
      const std::string v1_path = (v1_dir / "scene.json").string();
      {
        std::ofstream v1(v1_path, std::ios::trunc);
        v1 << "{\n  \"version\": 1,\n  \"pan_x\": 0,\n  \"pan_y\": 0,\n"
              "  \"zoom\": 1,\n  \"show_grid\": true,\n  \"selected_id\": 2,\n"
              "  \"entities\": [\n"
              "    {\"id\": 1, \"name\": \"Player\", \"x\": 1, \"y\": 2, "
              "\"w\": 32, \"h\": 48, \"r\": 1, \"g\": 0, \"b\": 0, \"a\": 1, "
              "\"layer\": 5},\n"
              "    {\"id\": 2, \"name\": \"TileMap\", \"x\": 0, \"y\": 160, "
              "\"w\": 256, \"h\": 64, \"r\": 0.5, \"g\": 0.5, \"b\": 0.5, "
              "\"a\": 1, \"layer\": 0}\n  ]\n}\n";
      }
      editor::Workspace2D v1w;
      std::string err;
      if (!editor::scene_io::load(v1w, v1_path, &err)) {
        return fail("v1 scene.json should still load: " + err);
      }
      const editor::Entity2D* up = v1w.find(2);
      const editor::Entity2D* pl = v1w.find(1);
      if (!up || !up->tilemap || up->tilemap->cols != 8 ||
          up->tilemap->rows != 2 || up->tilemap->count_nonempty() != 0 ||
          !pl || pl->tilemap || pl->sprite || pl->x != 1.0f) {
        return fail("v1 upgrade");
      }
    }

    // 11) Sprites + assets.
    const fs::path assets_dir = fs::path(opened_path) / "assets";
    if (!write_test_tga(assets_dir / "smoke_rider.tga", 16, 24,
                        [](int x, int y) -> std::uint32_t {
                          return ((x + y) & 1) ? 0xC9B48CFFu : 0xB86238FFu;
                        }) ||
        !write_test_tga(assets_dir / "smoke_tiles.tga", 64, 32,
                        [](int x, int) -> std::uint32_t {
                          return x < 32 ? 0x6B8C45FFu : 0x735033FFu;
                        })) {
      return fail("write test images");
    }
    {
      std::ofstream junk(assets_dir / "broken.png", std::ios::binary);
      junk << "not really a png";
    }
    scr->refresh_assets();
    const auto& list = scr->asset_list();
    auto listed = [&](const std::string& rel) {
      return std::find(list.begin(), list.end(), rel) != list.end();
    };
    if (!listed("assets/smoke_rider.tga") || !listed("assets/smoke_tiles.tga") ||
        !listed("assets/broken.png")) {
      return fail("asset listing");
    }
    const editor::TextureInfo& rider = scr->texture("assets/smoke_rider.tga");
    if (!rider.ok || rider.width != 16 || rider.height != 24 ||
        rider.handle != 0) {
      return fail("headless decode of the rider image");
    }
    if (scr->texture("assets/broken.png").ok) return fail("corrupt image decoded");

    const std::size_t pre_sprite_steps = w.undo_count();
    const std::uint64_t spr =
        scr->create_sprite_at("assets/smoke_rider.tga", 100.0f, 100.0f);
    const editor::Entity2D* se = w.find(spr);
    if (spr == 0 || !se || !se->sprite || se->w != 16.0f || se->h != 24.0f ||
        se->x != 92.0f || se->y != 88.0f || se->name != "smoke_rider" ||
        se->color[0] != 1.0f ||
        scr->sprite_state(*se) != editor::SpriteState::Ready ||
        w.undo_count() != pre_sprite_steps + 1 ||
        w.undo_label() != "Create Sprite") {
      return fail("create sprite at drop point");
    }
    // Assign to a plain entity, then flip + source rect as one step.
    const std::uint64_t crate = scr->create_entity("SpriteCrate");
    if (!scr->assign_sprite(crate, "assets/smoke_rider.tga") ||
        w.undo_label() != "Assign Sprite") {
      return fail("assign sprite");
    }
    {
      editor::Workspace2D::Snapshot before = w.snapshot();
      editor::Entity2D* ce = w.find(crate);
      ce->sprite->flip_x = true;
      ce->sprite->use_src_rect = true;
      ce->sprite->src_x = 2;
      ce->sprite->src_y = 4;
      ce->sprite->src_w = 8;
      ce->sprite->src_h = 12;
      if (!w.commit_step("Flip Sprite", std::move(before))) {
        return fail("sprite field edit should be a step");
      }
      flow.editor_save_scene();
    }
    // Missing file: keeps the path, reports Missing, never crashes.
    if (!scr->assign_sprite(spr, "assets/nope_not_here.png")) {
      return fail("assign missing sprite");
    }
    if (scr->sprite_state(*w.find(spr)) != editor::SpriteState::Missing ||
        scr->texture("assets/nope_not_here.png").error != "missing file") {
      return fail("missing sprite fallback");
    }
    const std::uint64_t bad = scr->create_sprite_at("assets/broken.png", 0, 0);
    if (bad == 0 || w.find(bad)->w != 64.0f ||
        scr->sprite_state(*w.find(bad)) != editor::SpriteState::Missing) {
      return fail("corrupt sprite falls back to a 64px rect");
    }
    {
      editor::Workspace2D d;
      std::string err;
      if (!editor::scene_io::load(d, scene_file.string(), &err)) {
        return fail("load sprites: " + err);
      }
      const editor::Entity2D* dc = d.find(crate);
      const editor::Entity2D* ds = d.find(spr);
      if (!dc || !dc->sprite || *dc->sprite != *w.find(crate)->sprite ||
          !dc->sprite->flip_x || dc->sprite->src_h != 12 || !ds ||
          !ds->sprite || ds->sprite->path != "assets/nope_not_here.png" ||
          d.entities() != w.entities()) {
        return fail("sprite save/load roundtrip");
      }
    }
    // Undo the missing assignment -> back to the rider; undo the assign.
    flow.editor_undo();  // corrupt sprite entity
    flow.editor_undo();  // missing-path assignment
    if (w.find(spr)->sprite->path != "assets/smoke_rider.tga") {
      return fail("undo sprite reassignment");
    }
    flow.editor_undo();  // flip / src
    flow.editor_undo();  // assign
    if (w.find(crate)->sprite) return fail("undo assign should drop the sprite");
    flow.editor_redo();
    if (!w.find(crate)->sprite) return fail("redo assign");
    if (!scr->assign_sprite(crate, "") || w.find(crate)->sprite) {
      return fail("remove sprite");
    }

    // Tileset slicing: 64x32 image at 16 px = 4 x 2 tiles; 32 px = 2 x 1.
    if (!scr->set_tileset(tm, "assets/smoke_tiles.tga") ||
        !scr->tileset_ready(*w.find(tm)) || scr->palette_count(*w.find(tm)) != 8) {
      return fail("tileset slicing at 16 px");
    }
    scr->set_tile_size(tm, 32);
    if (scr->palette_count(*w.find(tm)) != 2) return fail("tileset at 32 px");
    scr->set_tileset(tm, "assets/missing_tiles.png");
    if (scr->tileset_ready(*w.find(tm)) ||
        scr->palette_count(*w.find(tm)) != editor::kBuiltinTileCount) {
      return fail("missing tileset falls back to the built-in palette");
    }

    // Import copies into assets/ and never overwrites.
    const fs::path outside = smoke_root / "outside_art.tga";
    write_test_tga(outside, 4, 4, [](int, int) -> std::uint32_t {
      return 0xDEA034FFu;
    });
    std::string rel1;
    std::string rel2;
    if (!scr->import_asset(outside.string(), &rel1) ||
        !scr->import_asset(outside.string(), &rel2) ||
        rel1 != "assets/outside_art.tga" || rel2 != "assets/outside_art_2.tga" ||
        !fs::exists(assets_dir / "outside_art_2.tga")) {
      return fail("import (" + rel1 + ", " + rel2 + ")");
    }
    if (scr->import_asset((smoke_root / "nope.png").string())) {
      return fail("import of a missing file should fail");
    }
    if (!disk_matches()) return fail("scene.json out of sync after sprites");
    std::cout << "[smoke] tilemap OK (stroke = 1 step, brush 2x2, erase, "
                 "cancelled stroke, fill "
              << filled << " tiles, rect, eyedrop, resize keeps tiles, tile "
                           "size, undo-all/redo-all, v2 RLE roundtrip, v1 "
                           "upgrade)\n";
    std::cout << "[smoke] sprites OK (drop-create, assign/flip/src rect "
                 "roundtrip, undo/redo, missing + corrupt fallback, tileset "
                 "slicing, import)\n";
  }

  // Editor tools, headless: snap drag, nudge, multi-select, marquee,
  // duplicate, delete, and scene.json persistence of grid/snap/selection.
  {
    auto fail = [&](const char* what) {
      std::cerr << "Editor tools smoke failed: " << what << '\n';
      engine.shutdown();
      return 1;
    };
    editor::Workspace2D t;
    t.reset_defaults();  // Camera2D (0,0) / Player (64,64) / TileMap (0,160)
    std::uint64_t cam = 0;
    std::uint64_t player = 0;
    for (const editor::Entity2D& e : t.entities()) {
      if (e.name == "Camera2D") cam = e.id;
      if (e.name == "Player") player = e.id;
    }
    if (cam == 0 || player == 0) return fail("seed entities");

    t.set_grid_size(16.0f);
    t.set_snap_enabled(true);
    // Grid-only checks: object snap (edges/centres) is covered in SmokeTools.
    t.set_object_snap(false);
    if (t.snap_value(37.0f) != 32.0f || t.snap_extent(5.0f) != 16.0f) {
      return fail("snap_value / snap_extent");
    }

    // Drag-move with snap: +13,-5 from (64,64) lands on (80,64).
    t.select(player);
    t.begin_move();
    t.update_move(13.0f, -5.0f);
    if (!t.end_move()) return fail("drag move reported no change");
    const editor::Entity2D* p = t.find(player);
    if (p->x != 80.0f || p->y != 64.0f) return fail("snapped drag position");

    // Nudge: 1 cell right, 4 cells down (Shift).
    t.nudge_selection(1, 0, false);
    t.nudge_selection(0, 1, true);
    p = t.find(player);
    if (p->x != 96.0f || p->y != 128.0f) return fail("snapped nudge");
    // Off-grid entity lands on the next grid line first.
    t.find(player)->x = 100.0f;
    t.nudge_selection(1, 0, false);
    if (t.find(player)->x != 112.0f) return fail("off-grid nudge");
    // Snap off: 1 px / 10 px steps.
    t.set_snap_enabled(false);
    t.nudge_selection(-1, 0, false);
    t.nudge_selection(-1, 0, true);
    if (t.find(player)->x != 101.0f) return fail("free nudge");
    t.set_snap_enabled(true);
    t.find(player)->x = 96.0f;

    // Multi-select (Ctrl+click semantics) + group drag keeps formation.
    t.toggle_selection(cam);
    if (t.selection_count() != 2 || t.selected_id() != cam) {
      return fail("toggle_selection");
    }
    t.begin_move();
    t.update_move(32.0f, 0.0f);
    t.end_move();
    if (t.find(cam)->x != 32.0f || t.find(player)->x != 128.0f) {
      return fail("group move");
    }
    t.toggle_selection(cam);
    if (t.selection_count() != 1 || t.selected_id() != player) {
      return fail("toggle off");
    }
    t.toggle_selection(cam);

    // Duplicate both; copies are offset one cell and become the selection.
    const std::size_t n0 = t.entities().size();
    const std::vector<std::uint64_t> copies = t.duplicate_selection();
    if (copies.size() != 2 || t.entities().size() != n0 + 2 ||
        t.selection() != copies) {
      return fail("duplicate_selection");
    }
    const editor::Entity2D* pc = t.find(t.selected_id().value_or(0));
    if (!pc || pc->name != "Camera2D 2" || pc->x != 48.0f || pc->y != 16.0f) {
      return fail("duplicate naming / offset");
    }
    t.select(copies.front());
    t.duplicate_selection();
    if (t.selected() == nullptr || t.selected()->name != "Player 3") {
      return fail("duplicate of a copy should count up (Player 3)");
    }

    // Marquee: box around the camera only.
    if (t.select_in_rect(30.0f, -4.0f, 40.0f, 8.0f, false) != 1 ||
        t.selected_id() != cam || t.selection_count() != 1) {
      return fail("select_in_rect");
    }
    t.select_in_rect(-1000.0f, -1000.0f, 1000.0f, 1000.0f, false);
    if (t.selection_count() != t.entities().size()) {
      return fail("select_in_rect all");
    }

    // Persist grid/snap/selection through scene.json.
    t.set_selection(copies, copies.back());
    const fs::path tools_dir = smoke_root / "_editor_tools";
    fs::create_directories(tools_dir);
    const std::string tools_path = (tools_dir / "scene.json").string();
    std::string err;
    if (!editor::scene_io::save(t, tools_path, &err)) return fail("save");
    editor::Workspace2D back;
    if (!editor::scene_io::load(back, tools_path, &err)) return fail("load");
    if (back.grid_size() != 16.0f || !back.snap_enabled() ||
        back.entities().size() != t.entities().size() ||
        back.selection() != copies || back.selected_id() != copies.back()) {
      return fail("grid/snap/selection roundtrip");
    }

    // Delete selection.
    const std::size_t before_del = back.entities().size();
    if (back.delete_selection() != 2 ||
        back.entities().size() != before_del - 2 ||
        back.selection_count() != 0) {
      return fail("delete_selection");
    }
    std::cout << "[smoke] editor tools OK (snap drag, nudge, multi-select, "
                 "marquee, duplicate, delete, grid/snap persistence)\n";
  }

  // Direct SceneIO roundtrip without going through GUI again.
  {
    editor::Workspace2D direct;
    direct.reset_defaults();
    const std::uint64_t id = direct.create_entity("DirectRoundTrip");
    if (editor::Entity2D* e = direct.find(id)) {
      e->w = 99.0f;
      e->color[0] = 0.11f;
    }
    const fs::path direct_dir = smoke_root / "_direct_scene_io";
    fs::create_directories(direct_dir);
    const std::string direct_path = (direct_dir / "scene.json").string();
    std::string err;
    if (!editor::scene_io::save(direct, direct_path, &err)) {
      std::cerr << "Direct scene_io::save failed: " << err << '\n';
      engine.shutdown();
      return 1;
    }
    editor::Workspace2D loaded;
    if (!editor::scene_io::load(loaded, direct_path, &err)) {
      std::cerr << "Direct scene_io::load failed: " << err << '\n';
      engine.shutdown();
      return 1;
    }
    bool found = false;
    for (const editor::Entity2D& e : loaded.entities()) {
      if (e.name == "DirectRoundTrip" && e.w > 98.5f && e.color[0] < 0.12f) {
        found = true;
        break;
      }
    }
    if (!found || loaded.entities().size() != direct.entities().size()) {
      std::cerr << "Direct SceneIO roundtrip mismatch\n";
      engine.shutdown();
      return 1;
    }
  }

  flow.request_back_to_projects();
  if (flow.current_state() != AppState::ProjectManager) {
    std::cerr << "Expected ProjectManager after second back from Editor2D\n";
    engine.shutdown();
    return 1;
  }

  if (!flow.logout() || flow.current_state() != AppState::Login) {
    std::cerr << "Expected Login after Logout\n";
    engine.shutdown();
    return 1;
  }

  flow.request_quit();
  engine.shutdown();

  // Runtime + Play mode (headless; see SmokeRuntime.cpp).
  if (run_templates_smoke() != 0) {
    return 1;
  }
  if (run_trading_post_smoke() != 0) {
    return 1;
  }
  if (run_runtime_smoke() != 0) {
    return 1;
  }
  // Plugins + project types (see SmokePlugins.cpp).
  if (run_plugin_smoke() != 0) {
    return 1;
  }
  // 3D: scene v8, glTF, play runtime, GL offscreen render, 3D editor
  // (see Smoke3D.cpp / Smoke3DEditor.cpp).
  // AI toolkit + the ts.ai plugin (see SmokeAI.cpp / SmokeAIWorld.cpp).
  if (run_ai_smoke() != 0) {
    std::cerr << "TombStone Admin smoke FAILED (AI)\n";
    return 1;
  }
  // Story plugins: quests, dialogue, saves (see SmokeStory.cpp).
  if (run_story_smoke() != 0) {
    std::cerr << "TombStone Admin smoke FAILED (story)\n";
    return 1;
  }
  if (run_anim3d_smoke() != 0) {
    std::cerr << "TombStone Admin smoke FAILED (3D animation)\n";
    return 1;
  }
  if (run_3d_smoke() != 0) {
    return 1;
  }
  // VR: rig / grabbables, saloon sample driven through World3D (SmokeVR.cpp).
  if (run_vr_smoke() != 0) {
    return 1;
  }
  // Physics: Jolt 3D core, World3D bodies / VR grabs, Box2D (SmokePhysics.cpp).
  if (run_physics_smoke() != 0) {
    return 1;
  }
  // Frontier Survival plugin (see SmokeSurvival.cpp).
  if (run_survival_smoke() != 0) {
    return 1;
  }
  if (run_tools_smoke() != 0) {
    return 1;
  }
  // Airship Works plugin (see SmokeShipyard.cpp).
  if (run_shipyard_smoke() != 0) {
    return 1;
  }
  // Sound: decode, mixing levels, spatial math (see SmokeAudio.cpp).
  if (run_audio_smoke() != 0) {
    return 1;
  }
  // In-game UI: fonts, layout, navigation, round trip (see SmokeUi.cpp).
  if (run_ui_smoke() != 0) {
    return 1;
  }
  // Particles + 2D lighting (see SmokeFx.cpp).
  if (run_fx_smoke() != 0) {
    return 1;
  }
  // Visual scripting graphs (see SmokeGraph.cpp).
  if (run_graph_smoke() != 0) {
    return 1;
  }
  // C++ gameplay scripts (see SmokeNative.cpp; skipped without a compiler).
  if (run_native_smoke() != 0) {
    return 1;
  }
  // C# scripts (see SmokeCSharp.cpp; graceful-disable check without the SDK).
  if (run_csharp_smoke() != 0) {
    return 1;
  }

  std::cout << "TombStone Admin smoke complete\n";
  return flow.current_state() == AppState::Quit ? 0 : 1;
}

// ts_admin --brand-book <project> [scripts/x.graph.json]: the Brand Book on
// its own (edit a claim's visual scripts without opening the editor).
int run_brand_book_app(const std::string& project, const std::string& graph) {
  if (!glfwInit()) {
    std::cerr << "glfwInit failed\n";
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
  GLFWwindow* window = glfwCreateWindow(1440, 860, "TombStone Brand Book", nullptr, nullptr);
  if (!window) {
    std::cerr << "glfwCreateWindow failed\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");
  Settings defaults;
  apply_theme(defaults);
  const std::string name = std::filesystem::path(project).filename().string();
  editor::BrandBook& book = editor::BrandBook::get();
  if (graph.empty()) book.open(project, name);
  else book.open_graph(project, name, graph);
  book.set_fill_viewport(true);
  while (!glfwWindowShouldClose(window) && book.is_open()) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    book.draw();
    ImGui::Render();
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    const ImVec4 clear = editor::theme::Charcoal();
    glClearColor(clear.x, clear.y, clear.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

int run_imgui_app() {
  if (!glfwInit()) {
    std::cerr << "glfwInit failed\n";
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

  GLFWwindow* window =
      glfwCreateWindow(1280, 720, editor::brand::kWindowTitle, nullptr, nullptr);
  if (!window) {
    std::cerr << "glfwCreateWindow failed\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // Multi-viewport is optional; enable when the backend supports it.
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");
  // Sprites / tilesets upload through this while the GL context lives.
  static gfx::GlTextureUploader gl_uploader;
  gfx::set_texture_uploader(&gl_uploader);

  Engine engine;
  if (!engine.init()) {
    std::cerr << "Engine init failed\n";
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  AppFlow flow;
  flow.start();
  apply_theme(flow.settings());
  std::string last_theme = flow.settings().theme;
  std::string last_title = editor::brand::kWindowTitle;

  while (!glfwWindowShouldClose(window) && flow.is_running()) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : (1.0f / 60.0f);
    engine.tick(dt);
    flow.tick(dt);
    editor::EditorAudio::get().tick(dt);

    if (flow.settings().theme != last_theme) {
      apply_theme(flow.settings());
      last_theme = flow.settings().theme;
    }

    // Window title follows the open project ("TombStone Admin | My Claim").
    std::string title = editor::brand::kWindowTitle;
    if (flow.current_state() == AppState::Editor2D && flow.active_project()) {
      title = std::string(editor::brand::kWindowTitlePrefix) +
              flow.active_project()->name;
    }
    if (title != last_title) {
      glfwSetWindowTitle(window, title.c_str());
      last_title = title;
    }

    ImGui::Render();
    int display_w = 0;
    int display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    const ImVec4 clear = editor::theme::Charcoal();
    glClearColor(clear.x, clear.y, clear.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      GLFWwindow* backup_current_context = glfwGetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      glfwMakeContextCurrent(backup_current_context);
    }
    glfwSwapBuffers(window);
  }

  editor::EditorAudio::get().shutdown();
  engine.shutdown();
  // Live caches drop (not delete) their handles once the context is gone.
  gfx::set_texture_uploader(nullptr);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool smoke = false;
  // The Trading Post's plugin cache: installed plugins are found by this
  // process and by the ts_game / ts_client it launches.
  if (!std::getenv("TS_PLUGIN_CACHE")) {
    const std::string cache = ts::tombstone::store::TradingPost::default_cache_root();
#if defined(_WIN32)
    _putenv_s("TS_PLUGIN_CACHE", cache.c_str());
#else
    setenv("TS_PLUGIN_CACHE", cache.c_str(), 1);
#endif
  }
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--export") == 0) {
      // Headless Ship It: ts_admin --export <project> --out <dir> [...]
      return ts::tombstone::run_export_cli(argc, argv);
    }
    if (std::strcmp(argv[i], "--bake-templates") == 0 && i + 1 < argc) {
      return run_bake_templates(argv[i + 1]);
    }
    if (std::strcmp(argv[i], "--smoke-store") == 0) {
      return run_trading_post_smoke();
    }
    if (std::strcmp(argv[i], "--store-sign") == 0) {
      // ts_admin --store-sign <index.json> <seed hex>: writes index.json.sig
      return run_store_sign(argc, argv);
    }
    if (std::strcmp(argv[i], "--smoke-templates") == 0) {
      return run_templates_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-export") == 0) {
      return ts::tombstone::run_export_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-ai") == 0) {
      return run_ai_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-story") == 0) {
      return run_story_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-anim3d") == 0) {
      return run_anim3d_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-3d") == 0) {
      // Just the 3D checks (quick loop while working on the 3D side).
      return run_3d_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-vr") == 0) {
      // Just the VR checks.
      return run_vr_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-tools") == 0) {
      // Just the editor tools checks (2D + 3D).
      return run_tools_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-shipyard") == 0) {
      // Just the Airship Works checks.
      return run_shipyard_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-audio") == 0) {
      // Just the sound checks (null backend).
      return run_audio_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-ui") == 0) {
      return run_ui_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-cpp") == 0) {
      return run_native_smoke();
    }
    if (std::strcmp(argv[i], "--brand-book") == 0 && i + 1 < argc) {
      return run_brand_book_app(argv[i + 1], i + 2 < argc ? argv[i + 2] : "");
    }
    if (std::strcmp(argv[i], "--smoke-physics") == 0) {
      return run_physics_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-cs") == 0) {
      return run_csharp_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-graph") == 0) {
      return run_graph_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-fx") == 0) {
      return run_fx_smoke();
    }
    if (std::strcmp(argv[i], "--smoke-survival") == 0) {
      // Just the Frontier Survival checks.
      return run_survival_smoke();
    }
    if (std::strcmp(argv[i], "--smoke") == 0) {
      smoke = true;
    }
  }

  if (smoke) {
    return run_console_smoke();
  }
  return run_imgui_app();
}
