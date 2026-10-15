#include "editor/plugins/WeatherPanels.h"

#include "editor/ui/Theme.h"
#include "plugins/weather/WeatherPlugin.h"
#include "weather/Weather.h"
#include "weather/Weather3D.h"

#include <imgui.h>

#include <memory>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

struct WeatherPanelState {
  weather::Config cfg;
  bool loaded = false;
  bool dirty = false;
  std::string status;
  weather::Sim preview;  // edit-mode preview of the mix
  bool previewing = false;
};

bool kind_combo(const char* label, weather::Kind* k) {
  bool changed = false;
  if (ImGui::BeginCombo(label, weather::kind_name(*k))) {
    for (const std::string& n : weather::kind_names()) {
      weather::Kind v;
      if (!weather::kind_from(n, &v)) continue;
      if (ImGui::Selectable(n.c_str(), v == *k)) {
        *k = v;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

void bar(const char* label, float v) {
  ImGui::ProgressBar(v < 0 ? 0 : (v > 1 ? 1 : v), ImVec2(-90, 0), "");
  ImGui::SameLine();
  ImGui::Text("%s %.2f", label, v);
}

void draw_state(const weather::State& s) {
  ImGui::Text("Now: %s -> %s (%.0f%%)", weather::kind_name(s.kind), weather::kind_name(s.target),
              s.blend * 100.0f);
  bar("rain", s.mix.rain);
  bar("snow", s.mix.snow);
  bar("dust", s.mix.dust);
  bar("fog", s.mix.fog);
  bar("storm", s.mix.storm);
  bar("cloud", s.mix.cloud);
  bar("wind", s.wind);
  bar("flash", s.flash);
  ImGui::TextDisabled("strikes %llu, time of day %.2f, daylight %.2f",
                      static_cast<unsigned long long>(s.strikes), s.time_of_day, s.daylight);
}

void live_controls(weather::Sim& sim) {
  for (const std::string& n : weather::kind_names()) {
    weather::Kind k;
    if (!weather::kind_from(n, &k)) continue;
    if (ImGui::SmallButton(n.c_str())) sim.set(k);
    ImGui::SameLine();
  }
  if (ImGui::SmallButton("Strike!")) sim.strike();
  float tod = sim.state().time_of_day;
  if (ImGui::SliderFloat("Time of day", &tod, 0.0f, 1.0f)) sim.set_time_of_day(tod);
  draw_state(sim.state());
}

void draw_weather_panel(WeatherPanelState& st) {
  plugins::WeatherPlugin* p = plugins::WeatherPlugin::active();
  if (!p) {
    ImGui::TextDisabled("The Weather plugin isn't loaded.");
    return;
  }
  if (!st.loaded) {
    st.cfg = p->config();
    st.preview.configure(st.cfg);
    st.loaded = true;
  }

  // Live weather during Play (3D World extension first, else the 2D plugin).
  weather::Sim* live = nullptr;
  if (weather::Weather3D* w3 = weather::Weather3D::latest()) live = &w3->sim();
  else if (p->playing()) live = &p->sim();
  if (live) {
    theme::SectionHeader("Live (Play)");
    live_controls(*live);
  }

  theme::SectionHeader("weather.json");
  weather::Config& c = st.cfg;
  bool ch = false;
  ch |= kind_combo("Start", &c.start);
  ch |= ImGui::SliderFloat("Intensity", &c.intensity, 0.0f, 2.0f);
  ch |= ImGui::DragFloat("Transition (s)", &c.transition, 0.1f, 0.0f, 600.0f);
  ch |= ImGui::Checkbox("Cycle", &c.cycle);
  if (c.cycle) {
    int remove = -1;
    for (int i = 0; i < static_cast<int>(c.steps.size()); ++i) {
      ImGui::PushID(i);
      ImGui::SetNextItemWidth(110);
      ch |= kind_combo("##k", &c.steps[i].kind);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110);
      ch |= ImGui::DragFloat("s##sec", &c.steps[i].seconds, 1.0f, 1.0f, 36000.0f, "%.0f");
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) remove = i;
      ImGui::PopID();
    }
    if (remove >= 0) {
      c.steps.erase(c.steps.begin() + remove);
      ch = true;
    }
    if (ImGui::SmallButton("+ Step")) {
      c.steps.push_back({weather::Kind::Rain, 60.0f});
      ch = true;
    }
  }
  ch |= ImGui::SliderFloat("Wind direction", &c.wind_dir, 0.0f, 360.0f, "%.0f deg");
  ch |= ImGui::SliderFloat("Wind scale", &c.wind_scale, 0.0f, 3.0f);
  ch |= ImGui::SliderFloat("Gust", &c.gust, 0.0f, 1.0f);
  ch |= ImGui::DragFloatRange2("Lightning gap (s)", &c.lightning_min, &c.lightning_max, 0.1f, 0.2f, 600.0f);
  ch |= ImGui::Checkbox("Thunder", &c.thunder);
  ch |= ImGui::Checkbox("Time-of-day link (sky follows the hour)", &c.time_link);
  if (c.time_link) {
    ch |= ImGui::DragFloat("Day length (s)", &c.day_length, 1.0f, 10.0f, 86400.0f, "%.0f");
    ch |= ImGui::SliderFloat("Start time", &c.start_time, 0.0f, 1.0f, "%.2f (0.5 noon)");
  }
  int seed = static_cast<int>(c.seed);
  if (ImGui::InputInt("Seed", &seed)) {
    c.seed = static_cast<std::uint32_t>(seed);
    ch = true;
  }
  if (ch) {
    weather::normalize(c);
    st.dirty = true;
    st.preview.configure(c);
  }
  ImGui::BeginDisabled(!st.dirty);
  if (ImGui::Button("Save")) {
    p->set_config(c);
    std::string err;
    st.status = p->save_config(&err) ? "Saved weather.json." : "Save failed: " + err;
    st.dirty = !err.empty();
  }
  ImGui::SameLine();
  if (ImGui::Button("Revert")) {
    st.loaded = false;
    st.dirty = false;
    st.status.clear();
  }
  ImGui::EndDisabled();
  if (!st.status.empty()) ImGui::TextDisabled("%s", st.status.c_str());

  if (!live) {
    theme::SectionHeader("Preview");
    ImGui::Checkbox("Run preview", &st.previewing);
    if (st.previewing) st.preview.step(ImGui::GetIO().DeltaTime);
    live_controls(st.preview);
  }
}

}  // namespace

void install_weather_panels() {
  plugins::WeatherPlugin::set_editor_hook([](plugins::WeatherPlugin&, plugin::Registrar& reg) {
    auto st = std::make_shared<WeatherPanelState>();
    plugin::EditorPanel p;
    p.id = "weather.panel";
    p.title = "Weather";
    p.menu_label = "Weather";
    p.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(440, 620), ImGuiCond_FirstUseEver); };
    p.draw = [st] { draw_weather_panel(*st); };
    reg.add_editor_panel(std::move(p));
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
