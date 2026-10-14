#include "editor/screens/FrontierScreen.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

FrontierScreen::FrontierScreen(ProjectInfo project)
    : project_(std::move(project)) {}

FrontierScreen::~FrontierScreen() { plugins_.close(); }

void FrontierScreen::on_enter() {
  back_requested_ = false;
  log_.clear();
  auto sink = [this](runtime::LogLevel level, const std::string& text) {
    runtime::LogEntry e;
    e.level = level;
    e.channel = "plugins";
    e.text = text;
    log_.push_back(std::move(e));
    while (log_.size() > 200) log_.pop_front();
  };
  sink(runtime::LogLevel::Info, std::string("Survey camp pitched for ") +
                                    project_.name + " (" +
                                    to_string(project_.kind) + ")");
  plugins_.open(&project_, "frontier", this, sink);

  renderer_ = render3d::make_null_renderer3d();
  std::string err;
  if (renderer_->init(&err)) {
    cube_ = renderer_->create_mesh(render3d::make_cube(1.0f));
    render3d::MaterialDesc mat;
    mat.name = "copper";
    copper_ = renderer_->create_material(mat);
  } else {
    sink(runtime::LogLevel::Warn, "3D renderer wouldn't start: " + err);
  }
  if (project_.kind == ProjectKind::VR) {
    xr::XrConfig cfg;
    cfg.app_name = project_.name;
    xr_ = xr::create_xr_session(cfg);
    std::string why;
    if (!xr_->begin(&why)) {
      sink(runtime::LogLevel::Warn, "Goggles stay in the saddlebag: " + why);
    }
  }
  render_survey_frame(1280, 720);
  std::cout << "[Frontier] survey camp for \"" << project_.name << "\" ("
            << to_string(project_.kind) << ")\n";
}

void FrontierScreen::on_exit() {
  plugins_.close();
  if (xr_) xr_->end();
  xr_.reset();
  if (renderer_) renderer_->shutdown();
  renderer_.reset();
  std::cout << "[Frontier] leaving survey camp\n";
}

const char* FrontierScreen::renderer_backend() const {
  return renderer_ ? renderer_->backend_name() : "none";
}

std::string FrontierScreen::xr_status() const {
  if (project_.kind != ProjectKind::VR) return "not a VR claim";
  if (!xr_) return "no session";
  if (xr_->available()) return std::string(xr_->runtime_name()) + " ready";
  return std::string(xr_->runtime_name()) + ": " + xr_->unavailable_reason();
}

void FrontierScreen::render_survey_frame(int width, int height) {
  if (!renderer_) return;
  render3d::Camera3D cam;
  cam.position = {std::sin(spin_) * 4.0f, 2.0f, std::cos(spin_) * 4.0f};
  renderer_->begin_frame(cam, width, height);
  render3d::Transform t;
  renderer_->draw(cube_, copper_, t);
  last_frame_ = renderer_->end_frame();
}

AppState FrontierScreen::on_update(float delta_seconds) {
  spin_ += delta_seconds * 0.5f;
  if (ImGui::GetCurrentContext() != nullptr) {
    draw_ui();
  }
  render_survey_frame(1280, 720);
  return back_requested_ ? AppState::ProjectManager : AppState::EditorFrontier;
}

void FrontierScreen::draw_placeholder_viewport() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  if (size.x < 40.0f || size.y < 40.0f) return;
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  const float horizon = p0.y + size.y * 0.62f;

  // Dusk sky: charcoal overhead fading to copper at the horizon.
  const ImU32 top = theme::U32(theme::Charcoal());
  const ImU32 low = theme::U32(theme::CopperMuted());
  dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, horizon), top, top, low, low);
  // Sun on the horizon.
  const ImVec2 sun(p0.x + size.x * 0.68f, horizon);
  dl->AddCircleFilled(sun, size.y * 0.11f, theme::U32(theme::Accent(), 0.85f), 48);
  // Mesas.
  const ImU32 mesa = theme::U32(theme::PanelBg());
  dl->AddRectFilled(ImVec2(p0.x + size.x * 0.08f, horizon - size.y * 0.10f),
                    ImVec2(p0.x + size.x * 0.27f, horizon), mesa);
  dl->AddRectFilled(ImVec2(p0.x + size.x * 0.05f, horizon - size.y * 0.05f),
                    ImVec2(p0.x + size.x * 0.32f, horizon), mesa);
  dl->AddRectFilled(ImVec2(p0.x + size.x * 0.78f, horizon - size.y * 0.07f),
                    ImVec2(p0.x + size.x * 0.93f, horizon), mesa);
  // Ground with survey lines running to a vanishing point.
  dl->AddRectFilled(ImVec2(p0.x, horizon), p1, theme::U32(theme::HeaderBg()));
  const ImU32 line = theme::U32(theme::AccentMuted(), 0.6f);
  const ImVec2 vp(p0.x + size.x * 0.5f, horizon);
  for (int i = -8; i <= 8; ++i) {
    dl->AddLine(vp, ImVec2(vp.x + i * size.x * 0.14f, p1.y), line, 1.0f);
  }
  for (int i = 1; i <= 6; ++i) {
    const float t = static_cast<float>(i * i) / 36.0f;
    const float y = horizon + (p1.y - horizon) * t;
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), line, 1.0f);
  }
  // Survey stakes.
  for (int i = 0; i < 5; ++i) {
    const float x = p0.x + size.x * (0.18f + 0.16f * i);
    const float y = horizon + (p1.y - horizon) * (0.35f + 0.1f * (i % 2));
    dl->AddLine(ImVec2(x, y), ImVec2(x, y - 22.0f), theme::U32(theme::Sand()), 2.0f);
    dl->AddTriangleFilled(ImVec2(x, y - 22.0f), ImVec2(x + 12.0f, y - 18.0f),
                          ImVec2(x, y - 14.0f), theme::U32(theme::Danger()));
  }
  // Card with the message.
  const float cw = std::min(560.0f, size.x - 40.0f);
  const float ch = 150.0f;
  const ImVec2 c0(p0.x + (size.x - cw) * 0.5f, p0.y + size.y * 0.16f);
  const ImVec2 c1(c0.x + cw, c0.y + ch);
  dl->AddRectFilled(c0, c1, theme::U32(theme::PanelBg(), 0.92f),
                    theme::metrics::kRadiusCard);
  dl->AddRect(c0, c1, theme::U32(theme::AccentMuted()),
              theme::metrics::kRadiusCard);
  dl->AddLine(ImVec2(c0.x + 16, c0.y + 48), ImVec2(c1.x - 16, c0.y + 48),
              theme::U32(theme::Copper()), 2.0f);
  theme::TombstoneMark(dl, ImVec2(c0.x + 34.0f, c0.y + 10.0f), 30.0f,
                       theme::U32(theme::Sand()), theme::U32(theme::Charcoal()));
  theme::DrawSizedText(dl, ImVec2(c0.x + 62.0f, c0.y + 12.0f), 24.0f,
                       theme::U32(theme::Accent()),
                       "This frontier ain't surveyed yet");
  char l1[160];
  std::snprintf(l1, sizeof(l1), "%s claims are staked and saved, but the %s renderer",
                to_string(project_.kind),
                project_.kind == ProjectKind::VR ? "VR" : "3D");
  dl->AddText(ImVec2(c0.x + 16, c0.y + 60), theme::U32(theme::Text()), l1);
  dl->AddText(ImVec2(c0.x + 16, c0.y + 80), theme::U32(theme::Text()),
              "is still out on the trail. Plugins ride with you already.");
  char l2[200];
  std::snprintf(l2, sizeof(l2), "ts_render3d: %s backend, %zu draw / %zu tris  |  ts_xr: %s",
                renderer_backend(), last_frame_.draws, last_frame_.triangles,
                project_.kind == ProjectKind::VR ? (xr_ ? xr_->runtime_name() : "none")
                                                 : "not needed");
  dl->AddText(ImVec2(c0.x + 16, c0.y + 112), theme::U32(theme::CopperMuted()), l2);
  ImGui::Dummy(size);
}

void FrontierScreen::draw_ui() {
  theme::BeginRoot("##FrontierRoot");
  char subtitle[160];
  std::snprintf(subtitle, sizeof(subtitle), "Survey camp  |  %s (%s)",
                project_.name.c_str(), to_string(project_.kind));
  theme::TitleStrip("Unsurveyed Territory", subtitle);

  const float left_w = 360.0f;
  ImGui::BeginChild("##camp_left", ImVec2(left_w, 0), ImGuiChildFlags_None);
  theme::SectionHeader("Claim", to_string(project_.kind));
  ImGui::TextDisabled("%s", project_.path.c_str());
  if (project_.kind == ProjectKind::VR) {
    const std::string xs = "XR: " + xr_status();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
    ImGui::TextWrapped("%s", xs.c_str());
    ImGui::PopStyleColor();
  }
  if (theme::SecondaryButton("< Back to Claims", ImVec2(150, 0))) {
    back_requested_ = true;
  }
  ImGui::Spacing();
  ImGui::BeginChild("##camp_plugins", ImVec2(0, -140), ImGuiChildFlags_None);
  const PluginsPanelResult r = draw_plugins_panel(plugins_, plugins_panel_, false);
  if (r.changed && plugins_.pending()) plugins_.apply();
  if (r.rescan_requested) plugins_.rescan();
  ImGui::EndChild();
  theme::SectionHeader("Telegraph");
  ImGui::BeginChild("##camp_log", ImVec2(0, 0), ImGuiChildFlags_Borders);
  for (const runtime::LogEntry& e : log_) {
    const ImVec4 col = e.level == runtime::LogLevel::Error  ? theme::Danger()
                       : e.level == runtime::LogLevel::Warn ? theme::Warning()
                                                            : theme::TextMuted();
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", runtime::format_log(e).c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndChild();
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##camp_view", ImVec2(0, 0), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar);
  draw_placeholder_viewport();
  ImGui::EndChild();
  theme::EndRoot();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
