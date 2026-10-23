// The Stable: TombStone's animation-set panel. Cut a sheet into frames,
// round up clips from the thumbnail strip, set their pace and watch them
// run. Every edit lands in the .anim.json beside the sheet (see
// Editor2DScreen_Anim.cpp for the edit ops themselves).

#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Tooltip.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

ImTextureID tex_id(std::uint64_t handle) {
  return (ImTextureID)(std::uintptr_t)handle;
}

const char* mode_label(AnimMode m) {
  switch (m) {
    case AnimMode::Loop:
      return "Loop";
    case AnimMode::Once:
      return "Once";
    case AnimMode::PingPong:
      return "Ping-pong";
  }
  return "Loop";
}

// Thumbnail strips stop here; a 256 x 256 grid is a sheet, not a strip.
constexpr int kMaxThumbs = 512;

// One sheet cell (or a clip frame) scaled into [p0, p1): the image when the
// texture is on the GPU, a sand block with its number when headless.
void draw_cell(ImDrawList* dl, const TextureInfo& t, const AnimRect& r,
               const ImVec2& p0, const ImVec2& p1, int label) {
  dl->AddRectFilled(p0, p1, theme::U32(theme::Charcoal()));
  if (t.ok && t.handle != 0 && t.width > 0 && t.height > 0) {
    const ImVec2 uv0(static_cast<float>(r.x) / static_cast<float>(t.width),
                     static_cast<float>(r.y) / static_cast<float>(t.height));
    const ImVec2 uv1(static_cast<float>(r.x + r.w) / static_cast<float>(t.width),
                     static_cast<float>(r.y + r.h) / static_cast<float>(t.height));
    dl->AddImage(tex_id(t.handle), p0, p1, uv0, uv1);
  } else {
    dl->AddRectFilled(p0, p1, theme::U32(theme::Sand(), 0.35f));
  }
  if (label >= 0) {
    char num[8];
    std::snprintf(num, sizeof(num), "%d", label);
    dl->AddText(ImVec2(p0.x + 2.0f, p0.y + 1.0f),
                theme::U32(theme::TextMuted()), num);
  }
}

}  // namespace

void Editor2DScreen::draw_stable() {
  // Nothing open yet: lead in the first set the scene rides with.
  if (stable_path_.empty()) {
    for (const Entity2D& e : workspace_.entities()) {
      if (!e.animator || e.animator->set.empty()) continue;
      const AnimLibrary::Entry* en = anim_entry(e.animator->set);
      if (en && en->ok) {
        stable_open(e.animator->set);
        break;
      }
    }
  }
  const std::string caption =
      stable_path_.empty()
          ? std::string("no set open")
          : assets::file_name(stable_path_) +
                (stable_saved_ ? std::string() : std::string(" (unsaved)"));
  theme::SectionHeader("Stable", caption.c_str());

  // Sheet picker: every Supply Wagon image; [set] marks one already broke.
  const std::string sheet = stable_path_.empty() ? std::string() : stable_image();
  const std::string sheet_label =
      sheet.empty() ? std::string("(pick a sheet)") : assets::file_name(sheet);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##StableSheet", sheet_label.c_str())) {
    for (const std::string& img : asset_list()) {
      const bool has_set = !anim_set_for_image(img).empty();
      const std::string item =
          assets::file_name(img) + (has_set ? "  [set]" : "");
      if (ImGui::Selectable(item.c_str(), img == sheet)) {
        stable_open(img);
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("Sheet to break. [set] = a .anim.json already rides "
                      "beside it; others start a new one.");
  }
  if (stable_path_.empty()) {
    theme::StatusInfo("No sheet in the corral. Pick one above, or Open in "
                      "Stable from an Animator.");
    return;
  }
  if (!stable_saved_) {
    const std::string msg = "New set. The first edit writes " +
                            assets::file_name(stable_path_) + ".";
    theme::StatusWarn(msg.c_str());
  }
  const TextureInfo& tex = texture(sheet);
  if (!tex.ok) {
    theme::StatusError("Sheet image is missing; frames draw as blocks.");
  }

  // --- Grid slice ------------------------------------------------------------
  ImGui::SeparatorText("Slice");
  const AnimGrid& g = stable_set_.grid;
  int fw_fh[2] = {stable_frame_w_, stable_frame_h_};
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
  ImGui::InputInt2("##FrameSize", fw_fh);
  stable_frame_w_ = std::clamp(fw_fh[0], 1, AnimGrid::kMaxFrameSize);
  stable_frame_h_ = std::clamp(fw_fh[1], 1, AnimGrid::kMaxFrameSize);
  ImGui::SameLine();
  if (theme::CopperButton("Slice", ImVec2(62, 0))) {
    stable_slice(stable_frame_w_, stable_frame_h_);
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("Cut the sheet into W x H frames (columns and rows "
                      "fitted to the image).");
  }
  ImGui::TextDisabled("%d x %d px frames | %d cols x %d rows | %s", g.frame_w,
                      g.frame_h, g.cols, g.rows,
                      tex.ok ? (std::to_string(tex.width) + " x " +
                                std::to_string(tex.height) + " sheet").c_str()
                             : "no image");

  // --- Clips -----------------------------------------------------------------
  ImGui::SeparatorText("Clips");
  const int n = static_cast<int>(stable_set_.clips.size());
  if (stable_clip_ >= n) stable_clip_ = n - 1;
  if (ImGui::BeginChild("##StableClips", ImVec2(0, std::min(6, std::max(n, 2)) *
                                                    ImGui::GetTextLineHeightWithSpacing() + 6.0f),
                        ImGuiChildFlags_Borders)) {
    for (int i = 0; i < n; ++i) {
      const AnimClip& c = stable_set_.clips[static_cast<std::size_t>(i)];
      char row[160];
      const int frames = c.frame_count();
      std::snprintf(row, sizeof(row), "%s%s##clip%d", c.name.c_str(),
                    c.name == stable_set_.default_clip ? "  *" : "", i);
      if (ImGui::Selectable(row, i == stable_clip_)) {
        stable_select_clip(i);
      }
      char info[64];
      std::snprintf(info, sizeof(info), "%d fr  %.3g fps  %s", frames,
                    static_cast<double>(c.fps), mode_label(c.mode));
      const float tw = ImGui::CalcTextSize(info).x;
      ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x - tw, 120.0f));
      ImGui::TextDisabled("%s", info);
    }
    if (n == 0) {
      ImGui::TextDisabled("No clips yet. Pick frames below, then + Clip.");
    }
  }
  ImGui::EndChild();

  // New clip from the picked range (or the first cell).
  if (theme::PrimaryButton("+ Clip", ImVec2(70, 0))) {
    int k = n + 1;
    std::string name = "clip_" + std::to_string(k);
    while (stable_set_.find(name)) name = "clip_" + std::to_string(++k);
    const int start = stable_anchor_ >= 0 ? stable_anchor_ : 0;
    stable_add_clip(name, start, 1);
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("New clip starting at the last frame you clicked. "
                      "Name it idle_down / walk_left ... so riders pick it.");
  }
  const bool has_clip = stable_clip_ >= 0 && stable_clip_ < n;
  if (has_clip) {
    const AnimClip c = stable_set_.clips[static_cast<std::size_t>(stable_clip_)];
    ImGui::SameLine();
    const bool is_default = c.name == stable_set_.default_clip;
    ImGui::BeginDisabled(is_default);
    if (theme::SecondaryButton("Make default", ImVec2(100, 0))) {
      stable_set_default(c.name);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme::DangerButton("Turn out", ImVec2(-1, 0))) {
      stable_delete_clip(stable_clip_);
    }
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("Delete this clip from the set.");
    }
    if (stable_clip_ >= 0 &&
        stable_clip_ < static_cast<int>(stable_set_.clips.size())) {
      // Rename (Enter commits).
      if (std::string(stable_name_buf_) != c.name && !ImGui::IsAnyItemActive()) {
        std::snprintf(stable_name_buf_, sizeof(stable_name_buf_), "%s",
                      c.name.c_str());
      }
      if (ImGui::InputText("Name", stable_name_buf_, sizeof(stable_name_buf_),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        stable_rename_clip(stable_clip_, stable_name_buf_);
      }
      if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
        std::snprintf(stable_name_buf_, sizeof(stable_name_buf_), "%s",
                      c.name.c_str());
      }
      // Pace and mode.
      float fps = c.fps;
      if (ImGui::InputFloat("FPS", &fps, 1.0f, 5.0f, "%.1f",
                            ImGuiInputTextFlags_EnterReturnsTrue) &&
          fps != c.fps) {
        stable_set_timing(stable_clip_, std::clamp(fps, 0.1f, 120.0f), c.mode);
      }
      int mode = static_cast<int>(c.mode);
      const char* modes[] = {"Loop", "Once", "Ping-pong"};
      if (ImGui::Combo("Mode", &mode, modes, 3)) {
        stable_set_timing(stable_clip_, c.fps, static_cast<AnimMode>(mode));
      }
      if (!c.ms.empty()) {
        ImGui::TextDisabled("Per-frame ms set in the file override FPS.");
      }
      if (!c.grid) {
        ImGui::TextDisabled("Hand-cut frames (%d rects). Picking cells "
                            "below re-cuts it on the grid.",
                            static_cast<int>(c.rects.size()));
      } else {
        ImGui::TextDisabled("Frames %d..%d  (click first, Shift+click last)",
                            c.start, c.start + c.count - 1);
      }
    }
  }

  // --- Thumbnail strip: click = first frame, Shift+click = last ---------------
  ImGui::SeparatorText("Frames");
  const int cells = std::min(stable_set_.cell_count(), kMaxThumbs);
  const float th = 44.0f;
  const float tw = std::max(12.0f, th * static_cast<float>(g.frame_w) /
                                       static_cast<float>(std::max(1, g.frame_h)));
  const float gap = 3.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per_row = std::max(1, static_cast<int>((avail + gap) / (tw + gap)));
  const AnimClip* sel = has_clip && stable_clip_ < static_cast<int>(stable_set_.clips.size())
                            ? &stable_set_.clips[static_cast<std::size_t>(stable_clip_)]
                            : nullptr;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImGuiIO& io = ImGui::GetIO();
  for (int i = 0; i < cells; ++i) {
    if (i % per_row != 0) ImGui::SameLine(0.0f, gap);
    ImGui::PushID(i);
    ImGui::InvisibleButton("##cell", ImVec2(tw, th));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    draw_cell(dl, tex, stable_set_.cell_rect(i), p0, p1, i);
    const bool in_clip = sel && sel->grid && i >= sel->start &&
                         i < sel->start + sel->count;
    if (in_clip) {
      dl->AddRect(p0, p1, theme::U32(theme::Accent()), 0.0f, 0, 2.0f);
    } else if (i == stable_anchor_) {
      dl->AddRect(p0, p1, theme::U32(theme::Copper()), 0.0f, 0, 2.0f);
    } else if (ImGui::IsItemHovered()) {
      dl->AddRect(p0, p1, theme::U32(theme::AccentMuted()));
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (io.KeyShift && stable_anchor_ >= 0 && sel) {
        const int lo = std::min(stable_anchor_, i);
        const int hi = std::max(stable_anchor_, i);
        const int keep = stable_anchor_;
        stable_set_range(stable_clip_, lo, hi - lo + 1);
        stable_anchor_ = keep;
      } else {
        stable_anchor_ = i;
        if (sel) {
          stable_set_range(stable_clip_, i, 1);
          stable_anchor_ = i;
        }
      }
      stable_play_start_ = now_seconds();
    }
    ImGui::PopID();
  }
  if (stable_set_.cell_count() > kMaxThumbs) {
    ImGui::TextDisabled("Showing the first %d of %d frames.", kMaxThumbs,
                        stable_set_.cell_count());
  }

  // --- Preview -----------------------------------------------------------------
  ImGui::SeparatorText("Preview");
  sel = stable_clip_ >= 0 && stable_clip_ < static_cast<int>(stable_set_.clips.size())
            ? &stable_set_.clips[static_cast<std::size_t>(stable_clip_)]
            : nullptr;
  if (!sel) {
    ImGui::TextDisabled("Pick a clip to see it run.");
    return;
  }
  const double t = stable_playing_ ? now_seconds() - stable_play_start_ : 0.0;
  const AnimSample s = sample_clip(*sel, t);
  const AnimRect r = stable_set_.frame_rect(*sel, s.frame);
  const float ph = 96.0f;
  const float pw = ph * static_cast<float>(std::max(1, r.w)) /
                   static_cast<float>(std::max(1, r.h));
  ImGui::InvisibleButton("##StablePreview", ImVec2(pw, ph));
  draw_cell(dl, tex, r, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), -1);
  dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
              theme::U32(theme::AccentMuted()));
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Text("%s", sel->name.c_str());
  ImGui::TextDisabled("frame %d / %d%s", s.frame + 1, sel->frame_count(),
                      s.finished ? "  (done)" : "");
  ImGui::TextDisabled("%.3g fps  %s", static_cast<double>(sel->fps),
                      mode_label(sel->mode));
  if (theme::SecondaryButton(stable_playing_ ? "Hold" : "Ride", ImVec2(60, 0))) {
    stable_playing_ = !stable_playing_;
    stable_play_start_ = now_seconds();
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Restart", ImVec2(70, 0))) {
    stable_play_start_ = now_seconds();
  }
  ImGui::EndGroup();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
