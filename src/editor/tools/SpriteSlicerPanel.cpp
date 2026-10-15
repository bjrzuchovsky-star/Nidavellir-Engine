#include "editor/tools/SpriteSlicerPanel.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {
ImTextureID tex(std::uint64_t h) { return (ImTextureID)(std::uintptr_t)h; }

std::vector<std::string> scan_images(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path root = fs::path(dir) / "assets";
  if (!fs::is_directory(root, ec)) return out;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string ext = it->path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
      out.push_back(fs::relative(it->path(), dir, ec).generic_string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

const char* kModes[] = {"Loop", "Once", "PingPong"};
}  // namespace

SpriteSlicerPanel& SpriteSlicerPanel::get() {
  static SpriteSlicerPanel p;
  return p;
}

void SpriteSlicerPanel::open(const std::string& project_dir, const std::string& image_rel) {
  if (project_dir != project_dir_) {
    project_dir_ = project_dir;
    image_rel_.clear();
    rgba_.clear();
    rects_.clear();
    clips_.clear();
  }
  images_ = scan_images(project_dir_);
  if (!image_rel.empty()) load_image(image_rel);
  open_ = true;
}

bool SpriteSlicerPanel::load_image(const std::string& rel) {
  image_rel_ = rel;
  rgba_.clear();
  w_ = h_ = 0;
  error_.clear();
  status_.clear();
  if (!TextureCache::decode_file((fs::path(project_dir_) / rel).string(), &rgba_, &w_, &h_, &error_)) {
    rects_.clear();
    clips_.clear();
    return false;
  }
  // A sensible first grid: square frames the height of a row when the
  // sheet is a strip, otherwise 32 px.
  if (h_ > 0 && w_ % h_ == 0 && w_ / h_ > 1) grid_.frame_w = grid_.frame_h = h_;
  reslice();
  return true;
}

void SpriteSlicerPanel::reslice() {
  if (rgba_.empty()) return;
  rects_ = detect_mode_ ? slicer::detect_rects(rgba_.data(), w_, h_, detect_)
                        : slicer::grid_rects(rgba_.data(), w_, h_, grid_);
  clips_ = slicer::clips_per_row(rects_);
  selected_clip_ = 0;
}

AnimSet SpriteSlicerPanel::build() const {
  const std::string file = fs::path(image_rel_).filename().string();
  return slicer::build_set(file, rects_, clips_, detect_mode_ ? nullptr : &grid_, w_, h_);
}

std::string SpriteSlicerPanel::anim_path() const {
  return image_rel_.empty() ? std::string() : anim_json::set_path_for_image(image_rel_);
}

bool SpriteSlicerPanel::save(std::string* error) {
  if (rects_.empty() || clips_.empty()) {
    if (error) *error = "nothing sliced";
    return false;
  }
  const std::string rel = anim_path();
  std::string err;
  if (!anim_json::save_file((fs::path(project_dir_) / rel).string(), build(), &err)) {
    if (error) *error = err;
    return false;
  }
  status_ = "Saved " + rel + " (" + std::to_string(clips_.size()) + " clips)";
  return true;
}

void SpriteSlicerPanel::draw_sheet() {
  const TextureInfo& t = textures_.get((fs::path(project_dir_) / image_rel_).string());
  const ImVec2 size(w_ * zoom_, h_ * zoom_);
  ImGui::BeginChild("##sheet", ImVec2(0, std::min(size.y + 16.0f, 360.0f)), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Checkerboard behind transparent pixels.
  const float ck = 8.0f * zoom_;
  for (float y = 0; y < size.y; y += ck) {
    for (float x = 0; x < size.x; x += ck) {
      const bool odd = (static_cast<int>(x / ck) + static_cast<int>(y / ck)) & 1;
      dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(x + ck, size.x), p.y + std::min(y + ck, size.y)),
                        odd ? IM_COL32(58, 52, 48, 255) : IM_COL32(44, 40, 37, 255));
    }
  }
  if (t.ok) dl->AddImage(tex(t.handle), p, ImVec2(p.x + size.x, p.y + size.y));
  // Which clip owns each rect, for colouring.
  std::vector<int> owner(rects_.size(), -1);
  for (std::size_t c = 0; c < clips_.size(); ++c) {
    for (int i = 0; i < clips_[c].count; ++i) {
      const int k = clips_[c].first + i;
      if (k >= 0 && k < static_cast<int>(owner.size())) owner[static_cast<std::size_t>(k)] = static_cast<int>(c);
    }
  }
  for (std::size_t i = 0; i < rects_.size(); ++i) {
    const AnimRect& r = rects_[i];
    const ImVec2 a(p.x + r.x * zoom_, p.y + r.y * zoom_), b(a.x + r.w * zoom_, a.y + r.h * zoom_);
    const bool sel = owner[i] == selected_clip_;
    const ImU32 col = owner[i] < 0 ? theme::U32(theme::TextMuted(), 0.6f)
                                   : sel ? theme::U32(theme::Accent()) : theme::U32(theme::Sand(), 0.8f);
    dl->AddRect(a, b, col, 0, 0, sel ? 2.0f : 1.0f);
    char n[8];
    std::snprintf(n, sizeof(n), "%zu", i);
    dl->AddText(ImVec2(a.x + 2, a.y + 1), col, n);
  }
  ImGui::EndChild();
}

void SpriteSlicerPanel::draw_clips() {
  if (ImGui::BeginTable("##clips", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Clip");
    ImGui::TableSetupColumn("First");
    ImGui::TableSetupColumn("Frames");
    ImGui::TableSetupColumn("FPS");
    ImGui::TableSetupColumn("Mode");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
    ImGui::TableHeadersRow();
    int remove = -1;
    const int n = static_cast<int>(rects_.size());
    for (std::size_t c = 0; c < clips_.size(); ++c) {
      slicer::ClipPick& k = clips_[c];
      ImGui::PushID(static_cast<int>(c));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::RadioButton("##sel", selected_clip_ == static_cast<int>(c))) selected_clip_ = static_cast<int>(c);
      ImGui::SameLine();
      char name[64];
      std::snprintf(name, sizeof(name), "%s", k.name.c_str());
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputText("##name", name, sizeof(name))) k.name = name;
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      ImGui::DragInt("##first", &k.first, 0.2f, 0, std::max(0, n - 1));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      ImGui::DragInt("##count", &k.count, 0.2f, 1, std::max(1, n - k.first));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      ImGui::DragFloat("##fps", &k.fps, 0.2f, 1.0f, 60.0f, "%.0f");
      ImGui::TableNextColumn();
      int m = static_cast<int>(k.mode);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::Combo("##mode", &m, kModes, 3)) k.mode = static_cast<AnimMode>(m);
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("x")) remove = static_cast<int>(c);
      k.first = std::clamp(k.first, 0, std::max(0, n - 1));
      k.count = std::clamp(k.count, 1, std::max(1, n - k.first));
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove >= 0) clips_.erase(clips_.begin() + remove);
  }
  if (ImGui::Button("+ Clip") && !rects_.empty()) {
    clips_.push_back({"clip" + std::to_string(clips_.size() + 1), 0, static_cast<int>(rects_.size()), 8.0f,
                      AnimMode::Loop});
    selected_clip_ = static_cast<int>(clips_.size()) - 1;
  }
}

void SpriteSlicerPanel::draw() {
  if (!open_) return;
  if (pinned_) {
    ImGui::SetNextWindowPos(ImVec2(pin_[0], pin_[1]), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pin_[2], pin_[3]), ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowSize(ImVec2(760, 680), ImGuiCond_FirstUseEver);
  }
  textures_.poll_changes(ImGui::GetTime());
  if (!ImGui::Begin("Sprite Slicer###ts_slicer", &open_)) {
    ImGui::End();
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted("SPRITE SLICER");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("sheet -> frames -> clips -> .anim.json");

  ImGui::SetNextItemWidth(320);
  if (ImGui::BeginCombo("Sheet", image_rel_.empty() ? "(pick an image under assets/)" : image_rel_.c_str())) {
    for (const std::string& img : images_) {
      if (ImGui::Selectable(img.c_str(), img == image_rel_)) load_image(img);
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) images_ = scan_images(project_dir_);
  if (!error_.empty()) ImGui::TextColored(theme::Danger(), "%s", error_.c_str());
  if (rgba_.empty()) {
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("%d x %d px", w_, h_);
  ImGui::SameLine();
  if (ImGui::RadioButton("Grid", !detect_mode_)) set_auto_detect(false);
  ImGui::SameLine();
  if (ImGui::RadioButton("Auto-detect", detect_mode_)) set_auto_detect(true);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  ImGui::SliderFloat("Zoom", &zoom_, 1.0f, 8.0f, "%.0fx");
  bool changed = false;
  if (!detect_mode_) {
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::DragInt2("Frame", &grid_.frame_w, 0.25f, 1, 4096);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    changed |= ImGui::DragInt2("Offset", &grid_.offset_x, 0.25f, 0, 4096);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    changed |= ImGui::DragInt2("Spacing", &grid_.spacing_x, 0.25f, 0, 512);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Skip empty", &grid_.skip_empty);
  } else {
    ImGui::SetNextItemWidth(120);
    changed |= ImGui::SliderInt("Alpha >", &detect_.alpha_threshold, 0, 254);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    changed |= ImGui::SliderInt("Min px", &detect_.min_size, 1, 64);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    changed |= ImGui::SliderInt("Merge gap", &detect_.merge_gap, 0, 16);
  }
  if (changed) reslice();
  ImGui::TextColored(theme::TextMuted(), "%zu frames in %zu rows", rects_.size(),
                     rects_.empty() ? std::size_t{0} : static_cast<std::size_t>(slicer::row_of(rects_).back() + 1));

  draw_sheet();
  ImGui::SeparatorText("Clips");
  ImGui::BeginGroup();
  draw_clips();
  ImGui::EndGroup();

  // Preview of the selected clip.
  if (selected_clip_ >= 0 && selected_clip_ < static_cast<int>(clips_.size())) {
    const slicer::ClipPick& k = clips_[static_cast<std::size_t>(selected_clip_)];
    AnimClip clip;
    clip.count = k.count;
    clip.fps = k.fps;
    clip.mode = k.mode;
    const AnimSample s = sample_clip(clip, ImGui::GetTime());
    const int idx = k.first + s.frame;
    const TextureInfo& t = textures_.get((fs::path(project_dir_) / image_rel_).string());
    if (t.ok && idx >= 0 && idx < static_cast<int>(rects_.size())) {
      const AnimRect& r = rects_[static_cast<std::size_t>(idx)];
      const float sc = std::min(96.0f / std::max(r.w, r.h), 4.0f);
      ImGui::Text("Preview: %s  frame %d/%d", k.name.c_str(), s.frame + 1, k.count);
      ImGui::Image(tex(t.handle), ImVec2(r.w * sc, r.h * sc),
                   ImVec2(static_cast<float>(r.x) / w_, static_cast<float>(r.y) / h_),
                   ImVec2(static_cast<float>(r.x + r.w) / w_, static_cast<float>(r.y + r.h) / h_));
    }
  }

  ImGui::Separator();
  ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::AccentHover());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextOnAccent());
  if (ImGui::Button("Save animations")) {
    std::string err;
    if (!save(&err)) status_ = "Save failed: " + err;
  }
  ImGui::PopStyleColor(3);
  ImGui::SameLine();
  ImGui::TextDisabled("-> %s", anim_path().c_str());
  if (!status_.empty()) ImGui::TextColored(theme::TextMuted(), "%s", status_.c_str());
  ImGui::End();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
