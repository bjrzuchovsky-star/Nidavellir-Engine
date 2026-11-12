// Animation in the editor: the Animator component (Inspector, undo,
// autosave), edit-mode frame previews and the Stable's set edits. The
// Stable panel itself is drawn in Editor2DScreen_Stable.cpp.

#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Tooltip.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

constexpr const char* kSetSuffix = ".anim.json";

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::char_traits<char>::length(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string trimmed(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

// Combo over clip names; "" stands for `none_label`. True on a pick.
bool clip_combo(const char* label, const AnimSet& set,
                const std::string& current, const char* none_label,
                std::string* out) {
  bool picked = false;
  const char* preview = current.empty() ? none_label : current.c_str();
  if (ImGui::BeginCombo(label, preview)) {
    if (ImGui::Selectable(none_label, current.empty())) {
      *out = "";
      picked = true;
    }
    for (const AnimClip& c : set.clips) {
      if (ImGui::Selectable(c.name.c_str(), c.name == current)) {
        *out = c.name;
        picked = true;
      }
    }
    ImGui::EndCombo();
  }
  return picked;
}

}  // namespace

// --- Animator component ---------------------------------------------------------

bool Editor2DScreen::set_animator(std::uint64_t id,
                                  std::optional<AnimatorData> animator,
                                  const std::string& label) {
  if (is_playing() || !workspace_.find(id)) {
    return false;
  }
  Workspace2D::Snapshot before = prepare_edit();
  Entity2D* e = workspace_.find(id);
  const std::optional<AnimatorData> old = e->animator;
  e->animator = std::move(animator);
  normalize_components(*e);
  if (e->animator == old) {
    return false;
  }
  return commit_discrete(std::move(before), label, "");
}

const AnimLibrary::Entry* Editor2DScreen::anim_entry(const std::string& set_rel) {
  return anim_lib_.get(project_.path, set_rel);
}

std::string Editor2DScreen::anim_set_for_image(const std::string& image_rel) const {
  if (image_rel.empty() || ends_with(image_rel, kSetSuffix)) {
    return {};
  }
  const std::string rel = anim_json::set_path_for_image(image_rel);
  std::error_code ec;
  return fs::exists(fs::path(asset_path(rel)), ec) ? rel : std::string();
}

bool Editor2DScreen::entity_image(const Entity2D& e, double seconds,
                                  EntityImage* out) {
  *out = EntityImage{};
  const bool flip_x = e.sprite && e.sprite->flip_x;
  const bool flip_y = e.sprite && e.sprite->flip_y;
  auto finish = [&](const TextureInfo& t, int x, int y, int w, int h) {
    out->texture = &t;
    out->u0 = static_cast<float>(x) / static_cast<float>(t.width);
    out->v0 = static_cast<float>(y) / static_cast<float>(t.height);
    out->u1 = static_cast<float>(x + w) / static_cast<float>(t.width);
    out->v1 = static_cast<float>(y + h) / static_cast<float>(t.height);
    if (flip_x) std::swap(out->u0, out->u1);
    if (flip_y) std::swap(out->v0, out->v1);
    return true;
  };
  if (e.animator && !e.animator->set.empty()) {
    const AnimLibrary::Entry* entry = anim_entry(e.animator->set);
    if (entry && entry->ok) {
      // Same rule as the runtime: a loaded set owns the picture.
      const AnimSet& set = entry->set;
      const AnimClip* clip = set.find(e.animator->default_clip);
      if (!clip) clip = set.find(set.start_clip());
      const TextureInfo& t = texture(entry->image);
      if (!clip || !t.ok || t.width <= 0 || t.height <= 0) {
        return false;
      }
      const double time = anim_preview_ && e.animator->playing
                              ? seconds * static_cast<double>(e.animator->speed)
                              : 0.0;
      const AnimRect r = set.frame_rect(*clip, sample_clip(*clip, time).frame);
      out->animated = true;
      return finish(t, r.x, r.y, r.w, r.h);
    }
    // No set on disk: the sprite stands in, as it does in the ride.
  }
  if (!e.sprite || e.sprite->path.empty()) {
    return false;
  }
  const SpriteData& sp = *e.sprite;
  const TextureInfo& t = texture(sp.path);
  if (!t.ok || t.width <= 0 || t.height <= 0) {
    return false;
  }
  if (!sp.use_src_rect) {
    return finish(t, 0, 0, t.width, t.height);
  }
  const int sw = sp.src_w > 0 ? sp.src_w : t.width - sp.src_x;
  const int sh = sp.src_h > 0 ? sp.src_h : t.height - sp.src_y;
  return finish(t, sp.src_x, sp.src_y, sw, sh);
}

void Editor2DScreen::draw_inspector_animator(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || e->tilemap || e->camera || e->spawn) {
    return;
  }
  ImGui::SeparatorText("Animator");
  component_header_menu_2d(id, "Animator");
  if (!e->animator) {
    const std::string guess =
        e->sprite ? anim_set_for_image(e->sprite->path) : std::string();
    ImGui::TextDisabled("No animator. The sprite holds still.");
    const std::string label =
        guess.empty() ? std::string("Add Animator")
                      : "Add Animator (" + assets::file_name(guess) + ")";
    if (theme::SecondaryButton(label.c_str(), ImVec2(-1, 0))) {
      AnimatorData an;
      an.set = guess;
      set_animator(id, an, "Add Animator");
    }
    return;
  }
  const AnimatorData an = *e->animator;

  // Set: any .anim.json that sits beside a Supply Wagon image.
  const std::string set_preview =
      an.set.empty() ? std::string("(none)") : assets::file_name(an.set);
  if (ImGui::BeginCombo("Set", set_preview.c_str())) {
    for (const std::string& img : asset_list()) {
      const std::string rel = anim_set_for_image(img);
      if (rel.empty()) continue;
      if (ImGui::Selectable(rel.c_str(), rel == an.set)) {
        AnimatorData next = an;
        next.set = rel;
        next.clip.clear();
        next.default_clip.clear();
        set_animator(id, next, "Animator Set");
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("Sheets with a .anim.json beside them. Make one in the "
                      "Stable.");
  }
  const AnimLibrary::Entry* entry = an.set.empty() ? nullptr : anim_entry(an.set);
  if (an.set.empty()) {
    theme::StatusWarn("No set picked. Draws the plain sprite.");
  } else if (!entry || !entry->ok) {
    const std::string msg =
        (entry ? entry->error : std::string("Missing set")) +
        ". Draws the plain sprite.";
    theme::StatusWarn(msg.c_str());
  } else {
    const AnimSet& set = entry->set;
    std::string pick;
    if (clip_combo("Start clip", set, an.clip, "(default)", &pick)) {
      AnimatorData next = an;
      next.clip = pick;
      set_animator(id, next, "Animator Clip");
    }
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("The clip the ride starts on. Riders then pick "
                        "idle_/walk_ + _down/_up/_left/_right by heading.");
    }
    if (clip_combo("Default clip", set, an.default_clip, "(set default)",
                   &pick)) {
      AnimatorData next = an;
      next.default_clip = pick;
      set_animator(id, next, "Animator Default Clip");
    }
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("Fallback when no idle_/walk_ clip fits; also what "
                        "the viewport previews.");
    }
    ImGui::TextDisabled("%zu clips | %d x %d frames | %s", set.clips.size(),
                        set.grid.frame_w, set.grid.frame_h,
                        assets::file_name(entry->image).c_str());
  }

  Entity2D* cur = workspace_.find(id);
  if (cur && cur->animator) {
    if (ImGui::DragFloat("Speed", &cur->animator->speed, 0.01f, 0.0f,
                         AnimatorData::kMaxSpeed, "%.2fx")) {
      cur->animator->speed =
          std::clamp(cur->animator->speed, 0.0f, AnimatorData::kMaxSpeed);
      mark_dirty();
    }
    track_inspector_item("Animator Speed");
  }
  cur = workspace_.find(id);
  if (cur && cur->animator) {
    bool playing = cur->animator->playing;
    if (ImGui::Checkbox("Playing", &playing)) {
      AnimatorData next = *cur->animator;
      next.playing = playing;
      set_animator(id, next, playing ? "Animator Play" : "Animator Hold");
    }
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("Off holds the current frame.");
    }
    ImGui::SameLine();
    if (theme::SecondaryButton("Open in Stable", ImVec2(120, 0)) &&
        !an.set.empty()) {
      show_stable_ = true;
      stable_open(an.set);
    }
    ImGui::SameLine();
    if (theme::DangerButton("Remove##animator", ImVec2(70, 0))) {
      set_animator(id, std::nullopt, "Remove Animator");
    }
  }
}

// --- Stable: set edits ------------------------------------------------------------

std::string Editor2DScreen::stable_image() const {
  return anim_json::image_path(stable_path_, stable_set_);
}

bool Editor2DScreen::stable_open(const std::string& rel) {
  if (rel.empty()) {
    return false;
  }
  const bool is_set = ends_with(rel, kSetSuffix);
  const std::string set_rel = is_set ? rel : anim_json::set_path_for_image(rel);
  std::error_code ec;
  AnimSet set;
  bool saved = false;
  if (fs::exists(fs::path(asset_path(set_rel)), ec)) {
    std::string err;
    if (!anim_json::load_file(asset_path(set_rel), &set, &err)) {
      note("Stable: " + err);
      return false;
    }
    saved = true;
  } else if (is_set) {
    note("Stable: no " + assets::file_name(set_rel) + " on disk.");
    return false;
  } else {
    // New set for a bare sheet: one 32 px grid, no clips yet.
    set.image = fs::path(rel).filename().generic_string();
    const TextureInfo& t = texture(rel);
    set.slice(32, 32, t.ok ? t.width : 32, t.ok ? t.height : 32);
  }
  stable_path_ = set_rel;
  stable_set_ = std::move(set);
  stable_saved_ = saved;
  stable_clip_ = stable_set_.clips.empty()
                     ? -1
                     : std::max(0, stable_set_.clip_index(stable_set_.start_clip()));
  stable_anchor_ = -1;
  stable_frame_w_ = stable_set_.grid.frame_w;
  stable_frame_h_ = stable_set_.grid.frame_h;
  stable_play_start_ = now_seconds();
  note(saved ? "Stable: " + assets::file_name(set_rel) + " led in (" +
                   std::to_string(stable_set_.clips.size()) + " clips)"
             : "Stable: new set for " + assets::file_name(rel) +
                   " (saved on the first edit)");
  return true;
}

bool Editor2DScreen::stable_apply(AnimSet set, const std::string& note_text) {
  if (stable_path_.empty() || project_.path.empty()) {
    return false;
  }
  set.normalize();
  std::string err;
  if (!anim_json::save_file(asset_path(stable_path_), set, &err)) {
    note("Stable: " + err);
    return false;
  }
  stable_set_ = std::move(set);
  stable_saved_ = true;
  anim_lib_.forget(stable_path_);  // re-read even inside one mtime tick
  const int n = static_cast<int>(stable_set_.clips.size());
  if (stable_clip_ >= n) stable_clip_ = n - 1;
  if (!note_text.empty()) {
    note(note_text);
  }
  return true;
}

bool Editor2DScreen::stable_slice(int frame_w, int frame_h) {
  if (stable_path_.empty()) {
    return false;
  }
  AnimSet set = stable_set_;
  const TextureInfo& t = texture(stable_image());
  const int iw = t.ok ? t.width : set.grid.cols * set.grid.frame_w;
  const int ih = t.ok ? t.height : set.grid.rows * set.grid.frame_h;
  set.slice(frame_w, frame_h, iw, ih);
  stable_frame_w_ = set.grid.frame_w;
  stable_frame_h_ = set.grid.frame_h;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "Stable: cut into %d x %d frames (%d x %d)",
                set.grid.frame_w, set.grid.frame_h, set.grid.cols,
                set.grid.rows);
  return stable_apply(std::move(set), buf);
}

int Editor2DScreen::stable_add_clip(const std::string& raw_name, int start,
                                    int count) {
  const std::string name = trimmed(raw_name);
  if (stable_path_.empty() || name.empty() || stable_set_.find(name)) {
    note(name.empty() ? "Stable: a clip needs a name."
                      : "Stable: there's already a clip called " + name + ".");
    return -1;
  }
  AnimSet set = stable_set_;
  AnimClip c;
  c.name = name;
  c.start = start;
  c.count = count;
  set.clips.push_back(c);
  if (set.default_clip.empty()) {
    set.default_clip = name;
  }
  if (!stable_apply(std::move(set), "Stable: clip " + name + " added")) {
    return -1;
  }
  stable_select_clip(static_cast<int>(stable_set_.clips.size()) - 1);
  return stable_clip_;
}

bool Editor2DScreen::stable_rename_clip(int index, const std::string& raw_name) {
  const std::string name = trimmed(raw_name);
  if (index < 0 || index >= static_cast<int>(stable_set_.clips.size()) ||
      name.empty()) {
    return false;
  }
  AnimSet set = stable_set_;
  AnimClip& c = set.clips[static_cast<std::size_t>(index)];
  if (c.name == name) {
    return false;
  }
  if (set.find(name)) {
    note("Stable: there's already a clip called " + name + ".");
    return false;
  }
  const std::string old = c.name;
  c.name = name;
  if (set.default_clip == old) {
    set.default_clip = name;
  }
  return stable_apply(std::move(set), "Stable: " + old + " is now " + name);
}

bool Editor2DScreen::stable_delete_clip(int index) {
  if (index < 0 || index >= static_cast<int>(stable_set_.clips.size())) {
    return false;
  }
  AnimSet set = stable_set_;
  const std::string name = set.clips[static_cast<std::size_t>(index)].name;
  set.clips.erase(set.clips.begin() + index);
  if (set.default_clip == name) {
    set.default_clip = set.clips.empty() ? std::string() : set.clips.front().name;
  }
  return stable_apply(std::move(set), "Stable: clip " + name + " turned out");
}

bool Editor2DScreen::stable_set_range(int index, int start, int count) {
  if (index < 0 || index >= static_cast<int>(stable_set_.clips.size())) {
    return false;
  }
  AnimSet set = stable_set_;
  AnimClip& c = set.clips[static_cast<std::size_t>(index)];
  c.grid = true;
  c.rects.clear();
  c.start = start;
  c.count = count;
  return stable_apply(std::move(set), "");
}

bool Editor2DScreen::stable_set_timing(int index, float fps, AnimMode mode) {
  if (index < 0 || index >= static_cast<int>(stable_set_.clips.size())) {
    return false;
  }
  AnimSet set = stable_set_;
  AnimClip& c = set.clips[static_cast<std::size_t>(index)];
  c.fps = fps;
  c.mode = mode;
  return stable_apply(std::move(set), "");
}

bool Editor2DScreen::stable_set_frame_ms(int index, int frame, int ms) {
  if (index < 0 || index >= static_cast<int>(stable_set_.clips.size())) return false;
  AnimSet set = stable_set_;
  AnimClip& c = set.clips[static_cast<std::size_t>(index)];
  const int n = c.frame_count();
  if (frame < 0 || frame >= n) return false;
  const int fallback = std::clamp(static_cast<int>(std::lround(1000.0 / std::max(c.fps, AnimClip::kMinFps))), 1,
                                  AnimClip::kMaxFrameMs);
  if (static_cast<int>(c.ms.size()) < n) c.ms.resize(static_cast<std::size_t>(n), fallback);
  c.ms[static_cast<std::size_t>(frame)] = std::clamp(ms, 1, AnimClip::kMaxFrameMs);
  return stable_apply(std::move(set), "");
}

bool Editor2DScreen::stable_set_default(const std::string& clip) {
  if (stable_path_.empty() || (!clip.empty() && !stable_set_.find(clip))) {
    return false;
  }
  AnimSet set = stable_set_;
  set.default_clip = clip;
  return stable_apply(std::move(set), "Stable: default clip " +
                                          (clip.empty() ? "cleared" : clip));
}

void Editor2DScreen::stable_select_clip(int index) {
  const int n = static_cast<int>(stable_set_.clips.size());
  stable_clip_ = n == 0 ? -1 : std::clamp(index, 0, n - 1);
  stable_anchor_ = -1;
  stable_play_start_ = now_seconds();
  if (stable_clip_ >= 0) {
    const std::string& name = stable_set_.clips[static_cast<std::size_t>(stable_clip_)].name;
    std::snprintf(stable_name_buf_, sizeof(stable_name_buf_), "%s", name.c_str());
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
