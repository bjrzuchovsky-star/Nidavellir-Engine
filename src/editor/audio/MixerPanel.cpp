// "Saloon Band": the mixer. One player per bus on the stage -- a fader, a
// level lamp, and a hat to tip (mute). Music deck + voices below.

#include "editor/audio/EditorAudio.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

const char* bus_player(audio::Bus b) {
  switch (b) {
    case audio::Bus::Master: return "Bandleader";
    case audio::Bus::Music: return "Piano";
    case audio::Bus::Sfx: return "Six-guns";
    case audio::Bus::Ui: return "Bell";
    case audio::Bus::Voice: return "Singer";
    default: break;
  }
  return "";
}

void level_lamp(float peak, float height) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = 10.0f;
  const ImVec2 p1(p0.x + w, p0.y + height);
  dl->AddRectFilled(p0, p1, theme::U32(theme::Charcoal()), 2.0f);
  const float f = std::clamp(peak, 0.0f, 1.0f);
  if (f > 0.0f) {
    const ImVec4 col = f > 0.95f ? theme::Danger()
                       : f > 0.7f ? theme::Warning()
                                  : theme::Accent();
    dl->AddRectFilled(ImVec2(p0.x, p1.y - height * f), p1, theme::U32(col), 2.0f);
  }
  dl->AddRect(p0, p1, theme::U32(theme::Border()), 2.0f);
  ImGui::Dummy(ImVec2(w, height));
}

}  // namespace

void draw_mixer_panel() {
  EditorAudio& ea = EditorAudio::get();
  audio::Engine& e = ea.engine();

  const std::string backend = e.backend_name();
  theme::SectionHeader("Saloon Band", e.has_device() ? "live" : "quiet house");
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  if (e.has_device()) {
    ImGui::TextWrapped(
        "Dusk settles on the saloon. Five players on the stage: slide a fader "
        "to set who plays loud, tip a hat to hush one.");
  } else {
    ImGui::TextWrapped(
        "No sound card answered the call (%s). The band still plays to an "
        "empty room, so the lamps keep moving.",
        backend.c_str());
  }
  ImGui::PopStyleColor();
  ImGui::Spacing();

  const float fader_h = 150.0f;
  const float col_w = 86.0f;
  for (int i = 0; i < audio::kBusCount; ++i) {
    const audio::Bus b = static_cast<audio::Bus>(i);
    ImGui::PushID(i);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    ImGui::TextUnformatted(bus_player(b));
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
    ImGui::TextUnformatted(audio::bus_label(b));
    ImGui::PopStyleColor();

    float vol = e.bus_volume(b) * 100.0f;
    if (ImGui::VSliderFloat("##vol", ImVec2(34.0f, fader_h), &vol, 0.0f, 200.0f,
                            "%.0f%%")) {
      ea.set_bus_volume(b, vol / 100.0f);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      ea.set_bus_volume(b, 1.0f);
    }
    ImGui::SameLine();
    level_lamp(e.bus_peak(b), fader_h);

    bool muted = e.bus_muted(b);
    if (theme::ToggleButton(muted ? "Hat off" : "Hat on", &muted)) {
      ea.set_bus_muted(b, muted);
    }
    ImGui::EndGroup();
    ImGui::PopID();
    if (i + 1 < audio::kBusCount) {
      ImGui::SameLine(0.0f, std::max(8.0f, col_w - ImGui::GetItemRectSize().x));
    }
  }

  ImGui::Spacing();
  theme::Divider();
  theme::SectionHeader("Piano roll", "music");
  const std::string now = e.current_music();
  if (now.empty()) {
    ImGui::TextDisabled("Piano's quiet.");
  } else {
    ImGui::Text("Playing: %s", now.c_str());
    if (!e.playlist().empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("(track %d of %d)", e.playlist_index() + 1,
                          static_cast<int>(e.playlist().size()));
    }
    const float a = e.music_deck_gain(0);
    const float b = e.music_deck_gain(1);
    ImGui::TextDisabled("Decks: %.0f%% / %.0f%%", a * 100.0f, b * 100.0f);
  }
  if (theme::SecondaryButton("Strike up a tune")) {
    e.play_music("builtin:tune", 1.0f, true);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(now.empty());
  if (theme::SecondaryButton("Hush the piano")) e.stop_music(1.0f);
  if (!e.playlist().empty()) {
    ImGui::SameLine();
    if (theme::SecondaryButton("Next song")) e.next_track(1.0f);
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  theme::SectionHeader("On the floor", "voices");
  const auto voices = e.voices();
  ImGui::Text("%d voice%s sounding (of %d)", static_cast<int>(voices.size()),
              voices.size() == 1 ? "" : "s", static_cast<int>(audio::Engine::kMaxVoices));
  int shown = 0;
  for (const audio::VoiceInfo& v : voices) {
    if (++shown > 6) {
      ImGui::TextDisabled("...");
      break;
    }
    ImGui::BulletText("%s  [%s]", v.ref.empty() ? "(clip)" : v.ref.c_str(),
                      audio::bus_name(v.bus));
  }
  if (theme::SecondaryButton("Fire a test shot")) {
    audio::PlayParams p;
    p.pitch_variation = 0.08f;
    e.play("builtin:gunshot", p);
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Ring the bell")) ea.click();
  ImGui::SameLine();
  if (theme::SecondaryButton("Everyone, hush")) e.stop_all();

  ImGui::Spacing();
  if (theme::SecondaryButton("Reset the band")) ea.reset_mixer();
  ImGui::SameLine();
  ImGui::TextDisabled("Saved to %s", ea.settings_path().c_str());
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
