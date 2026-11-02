// Headless --smoke for the editor's animation tools: the seeded rider,
// Animator undo / redo + autosave, Play with an animated rider (and Stop
// putting everything back), animated Supply Wagon drops, edit-mode frame
// previews and the Stable's set edits.

#include "SmokeAnimation.h"

#include "editor/ProjectInfo.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/workspace/SceneIO.h"
#include "runtime/PlaySession.h"
#include "runtime/World.h"
#include "scene/Animation.h"
#include "scene/SceneData.h"
#include "scene/SceneJson.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace {

using namespace ts::tombstone;
namespace fs = std::filesystem;

int fail(const std::string& what) {
  std::cerr << "[smoke] animation editor FAILED: " << what << '\n';
  return 1;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool near(float a, float b) { return std::fabs(a - b) <= 1.0e-5f; }

runtime::InputFrame ride(float x, float y) {
  runtime::InputFrame f;
  f.slot(0).move_x = x;
  f.slot(0).move_y = y;
  f.slot(0).connected = true;
  return f;
}

std::string doc_text(const editor::Workspace2D& w) {
  return scene_json::write(editor::scene_io::to_doc(w));
}

}  // namespace

int run_animation_editor_smoke() {
  const fs::path dir = fs::path("TombStoneProjects") / "_anim_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  editor::ProjectInfo info;
  info.id = "_anim_smoke";
  info.name = "Animation Smoke";
  info.path = dir.string();
  editor::Editor2DScreen scr(info);
  scr.on_enter();
  editor::Workspace2D& w = scr.workspace();
  const fs::path scene = editor::scene_io::scene_path_for_project(info.path);
  const std::string kSet = "assets/rider.anim.json";

  // A fresh scene rides in with the sample rider sheet and its set.
  const Entity2D* player = find_entity_named(w.entities(), "Player");
  const std::string seeded = read_text(scene);
  if (!fs::exists(dir / "assets" / "rider.png") ||
      !fs::exists(dir / "assets" / "rider.anim.json") || !player ||
      !player->animator || player->animator->set != kSet ||
      !has(seeded, "\"version\": " +
                       std::to_string(scene_json::kSceneVersion)) ||
      !has(seeded, "\"animator\": {\"set\": \"assets/rider.anim.json\"")) {
    return fail("seed scene should carry the rider sheet and an animator");
  }
  const std::uint64_t pid = player->id;
  const AnimLibrary::Entry* entry = scr.anim_entry(kSet);
  if (!entry || !entry->ok || entry->set.clips.size() != 8 ||
      entry->image != "assets/rider.png") {
    return fail("seeded rider.anim.json should load with 8 clips");
  }

  // Edit-mode preview: idle_down at 3 fps; 0.4 s is its second frame.
  editor::EntityImage img;
  if (!scr.entity_image(*w.find(pid), 0.0, &img) || !img.animated ||
      !near(img.u0, 0.0f) || !near(img.v0, 0.0f) ||
      !near(img.u1, 32.0f / 192.0f) || !near(img.v1, 48.0f / 192.0f)) {
    return fail("preview frame 0 should be idle_down's first cell");
  }
  if (!scr.entity_image(*w.find(pid), 0.4, &img) ||
      !near(img.u0, 32.0f / 192.0f)) {
    return fail("preview should step idle_down to frame 1 at 0.4 s");
  }
  scr.set_anim_preview(false);
  if (!scr.entity_image(*w.find(pid), 0.4, &img) || !near(img.u0, 0.0f)) {
    return fail("preview off should rest on frame 0");
  }
  scr.set_anim_preview(true);

  // Animator edits: one undo step each, autosaved.
  const AnimatorData base = *w.find(pid)->animator;
  const std::size_t undo0 = w.undo_count();
  if (scr.set_animator(pid, base) || w.undo_count() != undo0) {
    return fail("an unchanged animator should not add a step");
  }
  AnimatorData quick = base;
  quick.speed = 2.0f;
  quick.default_clip = "walk_down";
  if (!scr.set_animator(pid, quick, "Animator Speed") ||
      w.undo_count() != undo0 + 1 || w.undo_label() != "Animator Speed" ||
      !has(read_text(scene), "\"default_clip\": \"walk_down\", \"speed\": 2")) {
    return fail("animator edit should be one autosaved step");
  }
  if (!scr.undo() || *w.find(pid)->animator != base ||
      !has(read_text(scene), "\"speed\": 1") || !scr.redo() ||
      *w.find(pid)->animator != quick ||
      !has(read_text(scene), "\"speed\": 2") || !scr.undo()) {
    return fail("animator edit should undo / redo with scene.json");
  }
  if (!scr.set_animator(pid, std::nullopt, "Remove Animator") ||
      w.find(pid)->animator || has(read_text(scene), "\"animator\"") ||
      !scr.undo() || !w.find(pid)->animator ||
      *w.find(pid)->animator != base ||
      !has(read_text(scene), "\"animator\"")) {
    return fail("animator remove should undo back in");
  }

  // Play: ride right, the walk cycle turns over; rest, the rider idles
  // facing right. Stop puts the workspace and scene.json back.
  const std::string disk_before = read_text(scene);
  const std::string doc_before = doc_text(w);
  if (!scr.start_play()) {
    return fail("play with an animated rider");
  }
  int changes = 0;
  int last = -1;
  std::string walk;
  for (int i = 0; i < 40; ++i) {
    scr.update_play(1.0 / 60.0, ride(1.0f, 0.0f));
    const runtime::Actor* a = scr.play_session().world().player(0);
    if (!a) break;
    walk = a->anim.clip;
    if (last >= 0 && a->anim.frame != last) ++changes;
    last = a->anim.frame;
  }
  for (int i = 0; i < 10; ++i) {
    scr.update_play(1.0 / 60.0, ride(0.0f, 0.0f));
  }
  const runtime::Actor* rider = scr.play_session().world().player(0);
  if (walk != "walk_right" || changes < 3 || !rider ||
      rider->anim.clip != "idle_right") {
    return fail("play: walk_right should cycle, then idle_right (\"" + walk +
                "\", " + std::to_string(changes) + " changes)");
  }
  if (scr.set_animator(pid, std::nullopt, "Remove Animator")) {
    return fail("animator edits must be locked while playing");
  }
  scr.stop_play();
  if (doc_text(w) != doc_before || read_text(scene) != disk_before) {
    return fail("stop should restore the workspace and leave scene.json");
  }

  // Supply Wagon: the rider sheet rides in animated, one frame big; a bare
  // copy of it rides in as a plain sprite.
  const std::uint64_t drop = scr.create_sprite_at("assets/rider.png", 300, 300);
  const Entity2D* d = w.find(drop);
  if (!d || !d->animator || d->animator->set != kSet || !d->sprite ||
      !d->sprite->use_src_rect || d->sprite->src_w != 32 ||
      d->sprite->src_h != 48 || d->w != 32.0f || d->h != 48.0f ||
      w.undo_label() != "Create Animated Sprite") {
    return fail("animated Supply Wagon drop");
  }
  fs::copy_file(dir / "assets" / "rider.png", dir / "assets" / "mule.png", ec);
  scr.refresh_assets();
  const std::uint64_t mule = scr.create_sprite_at("assets/mule.png", 0, 0);
  if (ec || !w.find(mule) || w.find(mule)->animator ||
      w.find(mule)->w != 192.0f || w.undo_label() != "Create Sprite") {
    return fail("a sheet without a set should drop as a plain sprite");
  }

  // Stable: a new set for the bare sheet, written on the first edit. None
  // of it touches the undo stack or scene.json.
  const std::size_t undo1 = w.undo_count();
  const std::string disk1 = read_text(scene);
  const fs::path mule_set = dir / "assets" / "mule.anim.json";
  if (!scr.stable_open("assets/mule.png") || scr.stable_saved() ||
      scr.stable_path() != "assets/mule.anim.json" ||
      scr.stable_image() != "assets/mule.png" || fs::exists(mule_set) ||
      scr.stable_set().grid.cols != 6 || scr.stable_set().grid.rows != 6) {
    return fail("stable_open on a bare sheet should start an unsaved 32 px set");
  }
  if (!scr.stable_slice(32, 48) || !scr.stable_saved() ||
      !fs::exists(mule_set) || scr.stable_set().grid.rows != 4) {
    return fail("slice should cut 32 x 48 and write the set");
  }
  if (scr.stable_add_clip("trot", 2, 4) != 0 ||
      scr.stable_add_clip("trot", 0, 1) != -1 ||
      scr.stable_add_clip(" idle ", 0, 2) != 1 ||
      scr.stable_set().default_clip != "trot" ||
      !scr.stable_set().find("idle")) {
    return fail("add clips (names unique and trimmed, first is the default)");
  }
  if (!scr.stable_rename_clip(0, "lope") || scr.stable_rename_clip(1, "lope") ||
      scr.stable_set().default_clip != "lope" ||
      !scr.stable_set_timing(0, 12.0f, AnimMode::PingPong) ||
      !scr.stable_set_range(1, 6, 3) || !scr.stable_set_default("idle")) {
    return fail("rename / timing / range / default");
  }
  if (!scr.stable_delete_clip(1) || scr.stable_set().clips.size() != 1 ||
      scr.stable_set().default_clip != "lope") {
    return fail("delete should hand the default to what is left");
  }
  AnimSet on_disk;
  std::string err;
  if (!anim_json::load_file(mule_set.string(), &on_disk, &err) ||
      !(on_disk == scr.stable_set()) || on_disk.clips[0].fps != 12.0f ||
      on_disk.clips[0].mode != AnimMode::PingPong) {
    return fail("the Stable's set should match mule.anim.json " + err);
  }
  const AnimLibrary::Entry* mule_entry = scr.anim_entry("assets/mule.anim.json");
  if (!mule_entry || !mule_entry->ok || mule_entry->set.clips.size() != 1 ||
      w.undo_count() != undo1 || read_text(scene) != disk1) {
    return fail("Stable edits should reach the library and skip undo");
  }
  const std::uint64_t mule2 = scr.create_sprite_at("assets/mule.png", 64, 0);
  if (!w.find(mule2) || !w.find(mule2)->animator ||
      w.find(mule2)->h != 48.0f) {
    return fail("once saddled, the sheet should drop animated");
  }
  if (!scr.stable_open(kSet) || !scr.stable_saved() ||
      scr.stable_set().clips.size() != 8 || scr.stable_clip() != 0) {
    return fail("stable_open on the rider set");
  }
  if (!scr.stable_set_frame_ms(0, 0, 250) || scr.stable_set().clips[0].ms.empty() ||
      scr.stable_set().clips[0].ms[0] != 250) {
    return fail("per-frame hold");
  }
  const AnimSample held = sample_clip(scr.stable_set().clips[0], 0.2);
  const AnimSample next = sample_clip(scr.stable_set().clips[0], 0.3);
  if (held.frame != 0 || next.frame == 0) return fail("a 250 ms hold should still be frame 0 at 0.2 s");
  AnimSet holds;
  if (!anim_json::load_file((dir / kSet).string(), &holds, &err) || holds.clips[0].ms.empty() ||
      holds.clips[0].ms[0] != 250) {
    return fail("frame hold was not written to the set");
  }
  scr.on_exit();
  fs::remove_all(dir, ec);
  std::cout << "[smoke] animation editor OK (seeded rider sheet + set, "
               "animator undo/redo + autosave, Play walk_right cycles then "
               "idles facing right, Stop restores, animated Supply Wagon "
               "drop, preview frames, Stable slice/add/rename/timing/range/"
               "delete saved to .anim.json without undo steps)\n";
  return 0;
}
