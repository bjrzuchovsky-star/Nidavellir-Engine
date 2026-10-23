// Particles + 2D lighting in the 2D editor: the Particles and Light
// inspector sections and the scene Lighting panel.

#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Tooltip.h"

#include "editor/fx/FxInspector.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

namespace ts {
namespace tombstone {
namespace editor {

void Editor2DScreen::draw_inspector_fx(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || e->tilemap) return;
  auto apply = [&](const char* label, auto&& fn) {
    Workspace2D::Snapshot before = prepare_edit();
    if (Entity2D* t = workspace_.find(id)) fn(*t);
    commit_discrete(std::move(before), label, "");
  };
  // Particles.
  if (!e->particles) {
    if (theme::SecondaryButton("Add Particles", ImVec2(140, 0))) {
      apply("Add Particles", [](Entity2D& t) { t.particles = fx::preset("campfire", fx::Space::TwoD); });
    }
  } else if (ImGui::CollapsingHeader("Particles", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::PushID("particles");
    fx::EmitterDesc copy = *e->particles;
    switch (draw_particle_fields(copy, fx::Space::TwoD)) {
      case FxEdit::None: break;
      case FxEdit::Live:
        if (Entity2D* t = workspace_.find(id)) { t->particles = copy; mark_dirty(); }
        break;
      case FxEdit::Commit:
        apply("Edit Particles", [&](Entity2D& t) { t.particles = copy; });
        break;
      case FxEdit::Remove:
        apply("Remove Particles", [](Entity2D& t) { t.particles.reset(); });
        break;
    }
    ImGui::PopID();
  }
  // Light.
  e = workspace_.find(id);
  if (!e) return;
  if (!e->light) {
    if (theme::SecondaryButton("Add Light", ImVec2(140, 0))) {
      apply("Add Light", [](Entity2D& t) { t.light = fx::Light2DData{}; });
    }
    if (ImGui::IsItemHovered())
      ux::SetTooltip("A point / spot light (needs scene Lighting on: View > Lighting).");
  } else if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::PushID("light");
    fx::Light2DData copy = *e->light;
    switch (draw_light_fields(copy)) {
      case FxEdit::None: break;
      case FxEdit::Live:
        if (Entity2D* t = workspace_.find(id)) { t->light = copy; mark_dirty(); }
        break;
      case FxEdit::Commit:
        apply("Edit Light", [&](Entity2D& t) { t.light = copy; });
        break;
      case FxEdit::Remove:
        apply("Remove Light", [](Entity2D& t) { t.light.reset(); });
        break;
    }
    ImGui::PopID();
  }
}

void Editor2DScreen::draw_lighting_panel() {
  if (!show_lighting_) return;
  ImGui::SetNextWindowSize(ImVec2(320, 300), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Lighting", &show_lighting_)) {
    fx::Lighting2D l = workspace_.lighting();
    if (draw_lighting_fields(l) != FxEdit::None) {
      workspace_.set_lighting(l);
      mark_dirty();
    }
    ImGui::TextDisabled("Applies in Play; lights are entity components.");
  }
  ImGui::End();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
