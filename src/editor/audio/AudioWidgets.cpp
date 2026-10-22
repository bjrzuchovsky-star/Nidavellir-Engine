#include "editor/audio/EditorAudio.h"
#include "editor/ux/Tooltip.h"
#include "editor/ui/Theme.h"
#include "scene/SceneData.h"

#include <imgui.h>

#include <algorithm>

namespace ts {
namespace tombstone {
namespace editor {

bool audio_clip_combo(const char* label, const std::string& current,
                      const std::vector<std::string>& sounds, std::string* out) {
  bool picked = false;
  const char* preview = current.empty() ? "(none)" : current.c_str();
  if (ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable("(none)", current.empty()) && !current.empty()) {
      out->clear();
      picked = true;
    }
    ImGui::SeparatorText("Built-in");
    for (const std::string& n : audio::builtin_names()) {
      const std::string ref = "builtin:" + n;
      if (ImGui::Selectable(ref.c_str(), ref == current) && ref != current) {
        *out = ref;
        picked = true;
      }
    }
    ImGui::SeparatorText("Project");
    if (sounds.empty()) ImGui::TextDisabled("No .wav/.ogg/.mp3/.flac in assets/");
    for (const std::string& rel : sounds) {
      if (ImGui::Selectable(rel.c_str(), rel == current) && rel != current) {
        *out = rel;
        picked = true;
      }
    }
    ImGui::EndCombo();
  }
  return picked;
}

AudioEdit draw_audio_source_fields(AudioSourceData& a,
                                   const std::vector<std::string>& sounds,
                                   const std::string& project_dir, bool three_d,
                                   const std::function<void(const char*)>& after_drag,
                                   const char** step) {
  AudioEdit r = AudioEdit::None;
  auto commit = [&](const char* label) {
    r = AudioEdit::Commit;
    *step = label;
  };
  auto live = [&]() {
    if (r == AudioEdit::None) r = AudioEdit::Live;
  };
  ImGui::PushID("audio_source");
  ImGui::TextColored(theme::Accent(), "Audio Source");

  std::string clip;
  if (audio_clip_combo("Clip", a.clip, sounds, &clip)) {
    a.clip = clip;
    commit("Audio Clip");
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TS_SOUND")) {
      a.clip = static_cast<const char*>(p->Data);
      commit("Audio Clip");
    }
    ImGui::EndDragDropTarget();
  }
  EditorAudio& ea = EditorAudio::get();
  ImGui::BeginDisabled(a.clip.empty());
  const bool hearing = ea.previewing(a.clip);
  if (theme::SecondaryButton(hearing ? "Stop##prev" : "Listen##prev", ImVec2(64, 0))) {
    if (hearing) {
      ea.stop_preview();
    } else {
      ea.preview(project_dir, a.clip);
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  audio::Bus parsed = audio::Bus::Sfx;
  audio::parse_bus(a.bus, &parsed);
  int bus = static_cast<int>(parsed);
  const char* names[audio::kBusCount];
  for (int i = 0; i < audio::kBusCount; ++i) {
    names[i] = audio::bus_label(static_cast<audio::Bus>(i));
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  if (ImGui::Combo("Bus", &bus, names, audio::kBusCount)) {
    a.bus = audio::bus_name(static_cast<audio::Bus>(bus));
    commit("Audio Bus");
  }

  float vol = a.volume * 100.0f;
  if (ImGui::SliderFloat("Volume", &vol, 0.0f, AudioSourceData::kMaxVolume * 100.0f,
                         "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
    a.volume = vol / 100.0f;
    live();
  }
  after_drag("Audio Volume");
  if (ImGui::SliderFloat("Pitch", &a.pitch, 0.25f, 4.0f, "%.2fx",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
    live();
  }
  after_drag("Audio Pitch");

  auto check = [&](const char* label, bool* v, const char* step_label) {
    if (ImGui::Checkbox(label, v)) commit(step_label);
  };
  check("Autoplay", &a.autoplay, "Audio Autoplay");
  ImGui::SameLine();
  check("Loop", &a.loop, "Audio Loop");
  ImGui::SameLine();
  check(three_d ? "3D" : "Positional", &a.spatial, "Audio Spatial");
  if (a.spatial) {
    const char* unit = three_d ? "%.1f m" : "%.0f px";
    const float speed = three_d ? 0.1f : 2.0f;
    const float top = three_d ? 1000.0f : 20000.0f;
    float d[2] = {a.min_distance, a.max_distance};
    if (ImGui::DragFloat2("Near / far", d, speed, 0.0f, top, unit)) {
      a.min_distance = d[0];
      a.max_distance = std::max(d[0], d[1]);
      live();
    }
    after_drag("Audio Distance");
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("Full volume inside near; fades to silence at far.");
    }
    check("Doppler", &a.doppler, "Audio Doppler");
  }
  if (!a.autoplay) {
    ImGui::TextDisabled("Quiet until a script calls audio.play(...)");
  }
  if (theme::DangerButton("Remove Audio", ImVec2(120, 0))) {
    r = AudioEdit::Remove;
    *step = "Remove Audio Source";
  }
  ImGui::PopID();
  return r;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
