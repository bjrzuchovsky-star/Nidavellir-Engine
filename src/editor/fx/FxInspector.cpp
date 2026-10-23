#include "editor/fx/FxInspector.h"
#include "editor/ux/Tooltip.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace {

struct Tracker {
  FxEdit r = FxEdit::None;
  void item(bool changed) {
    if (changed && r == FxEdit::None) r = FxEdit::Live;
    if (ImGui::IsItemDeactivatedAfterEdit()) r = FxEdit::Commit;
  }
  void discrete(bool changed) {
    if (changed) r = FxEdit::Commit;
  }
};

const char* kShapes[] = {"Point", "Circle / sphere", "Box", "Line"};
const char* kBlends[] = {"Alpha", "Additive"};

// Live preview: a small canvas running its own emitter on the edited desc.
struct Preview {
  fx::Emitter em;
  fx::EmitterDesc desc;
  double last = -1.0;
  bool made = false;
};

void draw_preview(const fx::EmitterDesc& d, fx::Space space) {
  static std::map<ImGuiID, Preview> previews;
  const ImGuiID id = ImGui::GetID("##fxpreview");
  Preview& p = previews[id];
  if (!p.made) {
    p.em = fx::Emitter(d, d.seed ? d.seed : 7u, space);
    p.desc = d;
    p.made = true;
  } else if (!(p.desc == d)) {
    p.em.set_desc(d);
    p.desc = d;
  }
  const double now = ImGui::GetTime();
  float dt = p.last < 0 ? 0.0f : static_cast<float>(now - p.last);
  p.last = now;
  dt = std::min(dt, 0.1f);
  const float W = ImGui::GetContentRegionAvail().x, H = 150.0f;
  const float origin[3] = {0, 0, 0};
  p.em.update(dt, origin);
  if (p.em.finished()) p.em.restart();

  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + W, a.y + H), IM_COL32(14, 12, 20, 255), 4.0f);
  dl->PushClipRect(a, ImVec2(a.x + W, a.y + H), true);
  // 2D: pixels, y down. 3D: metres, y up, 40 px per metre.
  const bool three = space == fx::Space::ThreeD;
  const float k = three ? 40.0f : 1.0f;
  const ImVec2 o(a.x + W * 0.5f, a.y + H * (three ? 0.8f : 0.6f));
  std::vector<fx::Sprite> sprites;
  p.em.sprites(&sprites);
  for (const fx::Sprite& s : sprites) {
    const float x = o.x + s.pos[0] * k;
    const float y = o.y + (three ? -s.pos[1] : s.pos[1]) * k;
    const float r = std::max(1.0f, s.size * k * 0.5f);
    const int al = static_cast<int>(std::clamp(s.rgba[3], 0.0f, 1.0f) * (s.additive ? 140 : 200));
    const ImU32 c = IM_COL32(static_cast<int>(std::clamp(s.rgba[0], 0.0f, 1.0f) * 255),
                             static_cast<int>(std::clamp(s.rgba[1], 0.0f, 1.0f) * 255),
                             static_cast<int>(std::clamp(s.rgba[2], 0.0f, 1.0f) * 255), al);
    dl->AddCircleFilled(ImVec2(x, y), r, c, 12);
  }
  dl->AddCircle(o, 3.0f, IM_COL32(255, 255, 255, 90), 8);
  dl->PopClipRect();
  ImGui::InvisibleButton("##fxpreview", ImVec2(W, H));
  if (ImGui::IsItemClicked()) p.em.restart();
  if (ImGui::IsItemHovered()) ux::SetTooltip("Live preview: click to restart");
  ImGui::TextDisabled("%d alive", p.em.alive());
}

}  // namespace

FxEdit draw_particle_fields(fx::EmitterDesc& d, fx::Space space) {
  Tracker t;
  const bool three = space == fx::Space::ThreeD;
  const float dist = three ? 0.05f : 1.0f;
  draw_preview(d, space);
  if (ImGui::BeginCombo("Preset", d.preset.empty() ? "(custom)" : d.preset.c_str())) {
    for (const std::string& n : fx::preset_names()) {
      if (ImGui::Selectable(n.c_str(), n == d.preset)) {
        if (auto p = fx::preset(n, space)) {
          const std::uint32_t seed = d.seed;
          d = *p;
          d.seed = seed;
          t.discrete(true);
        }
      }
    }
    ImGui::EndCombo();
  }
  t.discrete(ImGui::Checkbox("Enabled", &d.enabled));
  ImGui::SameLine();
  t.discrete(ImGui::Checkbox("Continuous", &d.continuous));
  ImGui::SameLine();
  t.discrete(ImGui::Checkbox("Local space", &d.local_space));
  if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen)) {
    t.item(ImGui::DragFloat("Rate /s", &d.rate, 0.5f, 0.0f, 2000.0f, "%.1f"));
    t.item(ImGui::DragInt("Burst", &d.burst, 0.5f, 0, 4096));
    t.item(ImGui::DragFloat("Duration s", &d.duration, 0.05f, 0.0f, 600.0f, "%.2f"));
    t.item(ImGui::DragInt("Max particles", &d.max_particles, 1.0f, 1, 8192));
    int shape = static_cast<int>(d.shape);
    if (ImGui::Combo("Shape", &shape, kShapes, 4)) {
      d.shape = static_cast<fx::EmitShape>(shape);
      t.discrete(true);
    }
    if (d.shape == fx::EmitShape::Circle) t.item(ImGui::DragFloat("Radius", &d.radius, dist, 0.0f, 1e5f));
    if (d.shape == fx::EmitShape::Box || d.shape == fx::EmitShape::Line)
      t.item(ImGui::DragFloat3("Extent", d.extent, dist, 0.0f, 1e5f));
    {
      unsigned seed = d.seed;
      int s = static_cast<int>(seed & 0x7fffffff);
      t.item(ImGui::DragInt("Seed (0 = entity)", &s, 1.0f, 0, 0x7fffffff));
      d.seed = static_cast<std::uint32_t>(s);
    }
  }
  if (ImGui::CollapsingHeader("Motion", ImGuiTreeNodeFlags_DefaultOpen)) {
    t.item(ImGui::DragFloat3("Direction", d.direction, 0.01f, -1.0f, 1.0f));
    t.item(ImGui::DragFloat("Spread deg", &d.spread, 0.5f, 0.0f, 180.0f));
    t.item(ImGui::DragFloat("Speed min", &d.speed_min, dist, 0.0f, 1e5f));
    t.item(ImGui::DragFloat("Speed max", &d.speed_max, dist, 0.0f, 1e5f));
    t.item(ImGui::DragFloat3("Gravity", d.gravity, dist, -1e5f, 1e5f));
    t.item(ImGui::DragFloat("Drag", &d.drag, 0.05f, 0.0f, 20.0f));
    t.item(ImGui::DragFloat("Life min", &d.life_min, 0.01f, 0.01f, 60.0f));
    t.item(ImGui::DragFloat("Life max", &d.life_max, 0.01f, 0.01f, 60.0f));
    t.item(ImGui::DragFloat("Spin deg/s", &d.spin, 1.0f, 0.0f, 3600.0f));
  }
  if (ImGui::CollapsingHeader("Over life", ImGuiTreeNodeFlags_DefaultOpen)) {
    t.item(ImGui::SliderFloat("Mid key", &d.mid_t, 0.05f, 0.95f));
    t.item(ImGui::DragFloat3("Size s/m/e", d.size, dist * 0.2f, 0.0f, 1e4f));
    const ImGuiColorEditFlags cf = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
    t.item(ImGui::ColorEdit4("Color start", d.color_start, cf));
    t.item(ImGui::ColorEdit4("Color mid", d.color_mid, cf));
    t.item(ImGui::ColorEdit4("Color end", d.color_end, cf));
  }
  if (ImGui::CollapsingHeader("Look")) {
    int blend = static_cast<int>(d.blend);
    if (ImGui::Combo("Blend", &blend, kBlends, 2)) {
      d.blend = static_cast<fx::Blend>(blend);
      t.discrete(true);
    }
    char tex[256];
    std::snprintf(tex, sizeof(tex), "%s", d.texture.c_str());
    if (ImGui::InputText("Texture", tex, sizeof(tex))) d.texture = tex;
    if (ImGui::IsItemDeactivatedAfterEdit()) t.r = FxEdit::Commit;
    if (ImGui::IsItemHovered()) ux::SetTooltip("Project image (empty = built-in soft dot)");
    t.item(ImGui::DragInt("Frames x", &d.frames_x, 0.1f, 1, 64));
    t.item(ImGui::DragInt("Frames y", &d.frames_y, 0.1f, 1, 64));
    t.item(ImGui::DragFloat("Frame fps", &d.frame_fps, 0.1f, 0.0f, 120.0f, "%.1f (0 = over life)"));
  }
  if (ImGui::SmallButton("Remove Particles")) return FxEdit::Remove;
  if (t.r != FxEdit::None) fx::normalize(d);
  return t.r;
}

FxEdit draw_light_fields(fx::Light2DData& l) {
  Tracker t;
  int kind = static_cast<int>(l.kind);
  const char* kinds[] = {"Point", "Spot"};
  if (ImGui::Combo("Kind", &kind, kinds, 2)) {
    l.kind = static_cast<fx::Light2DData::Kind>(kind);
    t.discrete(true);
  }
  t.item(ImGui::ColorEdit3("Color", l.color));
  t.item(ImGui::DragFloat("Intensity", &l.intensity, 0.02f, 0.0f, 4.0f));
  t.item(ImGui::DragFloat("Radius px", &l.radius, 1.0f, 1.0f, 4096.0f));
  if (l.kind == fx::Light2DData::Kind::Spot) {
    t.item(ImGui::DragFloat("Angle deg", &l.angle, 1.0f, -360.0f, 360.0f));
    t.item(ImGui::DragFloat("Cone deg", &l.cone, 0.5f, 1.0f, 360.0f));
  }
  t.item(ImGui::SliderFloat("Softness", &l.softness, 0.0f, 1.0f));
  t.item(ImGui::SliderFloat("Flicker", &l.flicker, 0.0f, 1.0f));
  t.discrete(ImGui::Checkbox("Casts tile shadows", &l.shadows));
  if (ImGui::SmallButton("Remove Light")) return FxEdit::Remove;
  if (t.r != FxEdit::None) fx::normalize(l);
  return t.r;
}

FxEdit draw_lighting_fields(fx::Lighting2D& l) {
  Tracker t;
  t.discrete(ImGui::Checkbox("Lighting enabled", &l.enabled));
  if (ImGui::IsItemHovered())
    ux::SetTooltip("Darken the scene by an ambient tint and light it with Light components.");
  t.item(ImGui::ColorEdit3(l.day_night ? "Day ambient" : "Ambient", l.ambient));
  t.discrete(ImGui::Checkbox("Day / night cycle", &l.day_night));
  if (l.day_night) {
    t.item(ImGui::ColorEdit3("Night ambient", l.night_ambient));
    t.item(ImGui::DragFloat("Day length s", &l.day_length, 1.0f, 5.0f, 7200.0f));
    t.item(ImGui::SliderFloat("Start time", &l.start_time, 0.0f, 1.0f, "%.2f (0 midnight, .5 noon)"));
    float now[3];
    l.ambient_at(0.0, now);
    ImGui::ColorButton("##amb_now", ImVec4(now[0], now[1], now[2], 1.0f));
    ImGui::SameLine();
    ImGui::TextDisabled("ambient at start");
  }
  t.item(ImGui::SliderInt("Lightmap px/texel", &l.resolution, 2, 16));
  if (t.r != FxEdit::None) fx::normalize(l);
  return t.r;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
