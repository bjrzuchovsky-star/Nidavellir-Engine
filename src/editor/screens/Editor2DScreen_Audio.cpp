// Audio in the 2D editor: the Audio Source inspector section and the
// Supply Wagon's sounds shelf (listen / drag / assign).

#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/Actions.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/audio/EditorAudio.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ts {
namespace tombstone {
namespace editor {

bool Editor2DScreen::set_audio_source(std::uint64_t id,
                                      const std::optional<AudioSourceData>& audio,
                                      const char* label) {
  Workspace2D::Snapshot before = prepare_edit();
  Entity2D* target = workspace_.find(id);
  if (!target || target->tilemap) return false;
  target->audio = audio;
  if (target->audio) normalize_audio(*target->audio);
  return commit_discrete(std::move(before), label, "");
}

void Editor2DScreen::draw_inspector_audio(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || e->tilemap) return;
  if (!e->audio) {
    if (theme::SecondaryButton("Add Audio Source", ImVec2(140, 0))) {
      AudioSourceData a;
      a.clip = "builtin:chime";
      set_audio_source(id, a, "Add Audio Source");
    }
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("A sound that rides with this entity (autoplay, loop, "
                        "panned by position).");
    }
    return;
  }
  AudioSourceData copy = *e->audio;
  const char* step = "Edit Audio Source";
  const AudioEdit r = draw_audio_source_fields(
      copy, sounds_, project_.path, false,
      [this](const char* label) { track_inspector_item(label); }, &step);
  switch (r) {
    case AudioEdit::None:
      break;
    case AudioEdit::Live:
      if (Entity2D* t = workspace_.find(id); t && t->audio) {
        t->audio = copy;
        normalize_audio(*t->audio);
        mark_dirty();
      }
      break;
    case AudioEdit::Commit:
      set_audio_source(id, copy, step);
      break;
    case AudioEdit::Remove:
      set_audio_source(id, std::nullopt, step);
      break;
  }
}

void Editor2DScreen::draw_wagon_sounds() {
  if (sounds_.empty()) return;
  char label[64];
  std::snprintf(label, sizeof(label), "Sounds (%zu)###WagonSounds", sounds_.size());
  if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
  EditorAudio& ea = EditorAudio::get();
  Entity2D* sel = workspace_.selected();
  const bool can_assign = sel && !sel->tilemap;
  const float h = std::min(150.0f, ImGui::GetFrameHeightWithSpacing() *
                                       static_cast<float>(sounds_.size()) + 6.0f);
  ImGui::BeginChild("##WagonSounds", ImVec2(0, h), ImGuiChildFlags_None);
  std::string assign;
  for (const std::string& rel : sounds_) {
    ImGui::PushID(rel.c_str());
    bool hearing = ea.previewing(rel);
    if (theme::ToggleButton(hearing ? "Stop" : "Play", &hearing,
                            ImVec2(44, 0))) {
      if (ea.previewing(rel)) {
        ea.stop_preview();
      } else {
        ea.preview(project_.path, rel);
      }
    }
    ImGui::SameLine();
    ImGui::Selectable(assets::file_name(rel).c_str(), false,
                      ImGuiSelectableFlags_AllowDoubleClick);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        can_assign) {
      assign = rel;
    }
    if (ImGui::IsItemHovered()) ux::SetTooltip("%s", rel.c_str());
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("TS_SOUND", rel.c_str(), rel.size() + 1);
      ImGui::Text("Sound: %s", assets::file_name(rel).c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem()) {
      if (ux::MenuItem("Listen")) ea.preview(project_.path, rel);
      if (ux::MenuItem("Assign to selected", nullptr, false, can_assign)) {
        assign = rel;
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (!assign.empty() && sel) {
    AudioSourceData a = sel->audio ? *sel->audio : AudioSourceData{};
    a.clip = assign;
    set_audio_source(sel->id, a, "Audio Clip");
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
