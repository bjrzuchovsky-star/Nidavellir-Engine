#include "editor/ux/ContextMenus.h"

#include "editor/update/UpdateService.h"
#include "editor/ux/Actions.h"
#include "editor/ux/FriendlyError.h"

#include <imgui.h>

#include <cctype>

namespace ts::tombstone::editor::ux {

namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

std::string docs_file_for(const std::string& component) {
  const std::string c = lower(component);
  auto has = [&](const char* k) { return c.find(k) != std::string::npos; };
  if (has("collid") || has("collision") || has("rigid") || has("joint") || has("physics") || has("grab")) return "physics.md";
  if (has("particle") || has("light")) return "particles-and-lighting.md";
  if (has("audio") || has("sound") || has("band")) return "audio.md";
  if (has("anim")) return "3d-vr-animation.md";
  if (has("script")) return "scripting.md";
  if (has("camera") || has("follow")) return "camera.md";
  if (has("mesh") || has("material") || has("model")) return "model-import.md";
  if (has("terrain") || has("foliage")) return "terrain-and-foliage.md";
  if (has("sprite") || has("tilemap") || has("tile")) return "autotiles-and-sprite-slicer.md";
  if (has("prefab")) return "prefabs.md";
  if (has("agent") || has("nav")) return "3d-vr-animation.md";
  if (has("transform") || has("attach")) return "transforms.md";
  if (has("ui") || has("font")) return "ui-and-fonts.md";
  return "editor-guide.md";
}

std::string docs_url(const std::string& file) {
  return "https://github.com/bjrzuchovsky-star/TombStone_Engine/blob/main/docs/" + file;
}

void open_docs(const std::string& component) {
  std::string err;
  const std::string url = docs_url(docs_file_for(component));
  if (!update::open_url(url, &err))
    show_error(explain(err.empty() ? "could not open " + url : err + " (" + url + ")", ErrorKind::Unknown,
                       "Opening the docs for " + component));
}

void ComponentHeaderItems(const char* component, const HeaderOps& ops) {
  ImGui::TextDisabled("%s", component);
  ImGui::Separator();
  if (MenuItem("Reset to defaults", nullptr, false, static_cast<bool>(ops.reset))) ops.reset();
  if (MenuItem("Copy values", nullptr, false, static_cast<bool>(ops.copy))) ops.copy();
  if (MenuItem("Paste values", nullptr, false, ops.can_paste && static_cast<bool>(ops.paste))) ops.paste();
  if (!ops.paste_from.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("From %s", ops.paste_from.c_str());
  ImGui::Separator();
  if (MenuItem("Move up", nullptr, false, static_cast<bool>(ops.move_up))) ops.move_up();
  if (MenuItem("Move down", nullptr, false, static_cast<bool>(ops.move_down))) ops.move_down();
  ImGui::Separator();
  if (MenuItem("Remove component", nullptr, false, static_cast<bool>(ops.remove))) ops.remove();
  ImGui::Separator();
  if (MenuItem("Open docs")) open_docs(component);
}

void ComponentHeaderMenu(const char* component, const HeaderOps& ops) {
  const std::string id = std::string("##hdr_ctx_") + component;
  if (ImGui::BeginPopupContextItem(id.c_str())) {
    ComponentHeaderItems(component, ops);
    ImGui::EndPopup();
  }
}

}  // namespace ts::tombstone::editor::ux
