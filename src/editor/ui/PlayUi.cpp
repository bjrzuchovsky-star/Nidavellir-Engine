#include "editor/ui/PlayUi.h"

#include <cstdint>
#include <filesystem>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

void draw_ui_draws(ImDrawList* draw, const std::vector<ui::UiDraw>& draws, const ImVec2& origin,
                   const UiTextureFn& texture) {
  if (!draw) return;
  ImTextureID atlas = texture ? texture(std::string()) : ImTextureID{};
  for (const ui::UiDraw& d : draws) {
    const ImTextureID tex = d.image.empty() ? atlas : (texture ? texture(d.image) : ImTextureID{});
    if (!tex) continue;
    const ui::UiQuad& q = d.quad;
    draw->AddImage(tex, ImVec2(origin.x + q.x0, origin.y + q.y0),
                   ImVec2(origin.x + q.x1, origin.y + q.y1), ImVec2(q.u0, q.v0),
                   ImVec2(q.u1, q.v1),
                   ImGui::ColorConvertFloat4ToU32(ImVec4(q.rgba[0], q.rgba[1], q.rgba[2], q.rgba[3])));
  }
}

void PlayUi::load(const std::string& project_dir) {
  ui_.clear();
  project_dir_ = project_dir;
  textures_.clear();
  std::string errors;
  ui_.load_dir((std::filesystem::path(project_dir) / "ui").string(), &errors);
  if (!errors.empty()) std::cerr << "[ui] " << errors;
}

std::vector<ui::UiEvent> PlayUi::update(const ImVec2& origin, const ImVec2& size, bool hovered,
                                        bool focused, double dt) {
  ui::UiInput in;
  in.dt = dt;
  const ImGuiIO& io = ImGui::GetIO();
  if (hovered || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    in.mouse_x = io.MousePos.x - origin.x;
    in.mouse_y = io.MousePos.y - origin.y;
    in.mouse_down = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  }
  if (focused && (ui_.modal_open() || ui_.wants_text())) {
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    in.up = pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp);
    in.down = pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_Tab) ||
              pressed(ImGuiKey_GamepadDpadDown);
    in.left = pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft);
    in.right = pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight);
    in.accept = pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) ||
                pressed(ImGuiKey_GamepadFaceDown);
    in.back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
              ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    in.backspace = pressed(ImGuiKey_Backspace);
    if (ui_.wants_text()) {
      for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c >= 32 && c < 127) in.text.push_back(static_cast<char>(c));
      }
    }
  }
  return ui_.update(in, size.x, size.y);
}

ImTextureID PlayUi::texture(const std::string& image) {
  std::string path = image.empty() ? std::string(ui::kAtlasKey) : image;
  if (!image.empty() && image.rfind("mem:", 0) != 0) {
    path = (std::filesystem::path(project_dir_) / image).string();
  }
  return (ImTextureID)(std::uintptr_t)textures_.get(path).handle;
}

void PlayUi::draw(ImDrawList* draw, const ImVec2& origin) {
  std::vector<ui::UiDraw> d;
  ui_.draw(&d);
  draw_ui_draws(draw, d, origin, [this](const std::string& image) { return texture(image); });
}

void UiPreview::draw(bool* open, const std::string& project_dir) {
  if (!*open) return;
  if (dir_ != project_dir) {
    dir_ = project_dir;
    ui_.load(project_dir);
    selected_.clear();
  }
  ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("UI Preview", open)) {
    ImGui::End();
    return;
  }
  const std::vector<std::string> names = ui_.system().names();
  if (selected_.empty() && !names.empty()) selected_ = names.front();
  ImGui::SetNextItemWidth(220);
  if (ImGui::BeginCombo("Document", selected_.c_str())) {
    for (const std::string& n : names) {
      if (ImGui::Selectable(n.c_str(), n == selected_)) selected_ = n;
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Reload")) {
    ui_.load(project_dir);
    last_event_.clear();
  }
  if (names.empty()) {
    ImGui::TextDisabled("No ui/*.ui.json in this project (the pause menu is built in).");
    ImGui::End();
    return;
  }
  for (const std::string& n : names) {
    if (n == selected_) ui_.system().show(n);
    else ui_.system().hide(n);
  }
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.y -= ImGui::GetTextLineHeightWithSpacing();
  ImVec2 size(avail.x, avail.x * 9.0f / 16.0f);
  if (size.y > avail.y) size = ImVec2(avail.y * 16.0f / 9.0f, avail.y);
  if (size.x < 32 || size.y < 18) {
    ImGui::End();
    return;
  }
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##ui_preview", size);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(38, 30, 24, 255));
  for (const ui::UiEvent& e : ui_.update(origin, size, hovered, hovered, ImGui::GetIO().DeltaTime)) {
    static const char* kinds[] = {" clicked", " changed", " submitted", " back"};
    last_event_ = e.document + "/" + e.widget + kinds[static_cast<int>(e.kind)];
  }
  dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
  ui_.draw(dl, origin);
  dl->PopClipRect();
  ImGui::TextDisabled("%s", last_event_.empty() ? "Click widgets to test; events show here."
                                                : last_event_.c_str());
  ImGui::End();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
