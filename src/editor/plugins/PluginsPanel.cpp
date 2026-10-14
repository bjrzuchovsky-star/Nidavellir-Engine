#include "editor/plugins/PluginsPanel.h"

#include "editor/ui/Theme.h"
#include "plugins/Builtins.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

bool matches(const plugin::PluginInfo& info, const char* filter) {
  if (!filter || !*filter) return true;
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  };
  const std::string f = lower(filter);
  return lower(info.name).find(f) != std::string::npos ||
         lower(info.id).find(f) != std::string::npos ||
         lower(info.description).find(f) != std::string::npos;
}

std::string types_label(std::uint32_t mask) {
  std::string s;
  if (mask & plugin::kType2D) s += "2D";
  if (mask & plugin::kType3D) s += s.empty() ? "3D" : " / 3D";
  if (mask & plugin::kTypeVR) s += s.empty() ? "VR" : " / VR";
  return s.empty() ? "none" : s;
}

void chip(const char* text, const ImVec4& col) {
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

std::string contributions(const plugin::PluginManager& m, const std::string& id) {
  int systems = 0, lua = 0, comps = 0, assets = 0, panels = 0, menus = 0;
  std::string lua_names;
  if (const auto ext = m.world_extensions()) {
    for (const auto& s : ext->systems) systems += s.owner == id;
    for (const auto& c : ext->components) comps += c.owner == id;
    for (const auto& l : ext->lua) {
      if (l.owner != id) continue;
      ++lua;
      if (!lua_names.empty()) lua_names += ", ";
      lua_names += l.name;
    }
  }
  for (const auto& a : m.asset_types()) assets += a.owner == id;
  for (const auto& p : m.editor_panels()) panels += p.owner == id;
  for (const auto& p : m.menu_items()) menus += p.owner == id;
  std::string out;
  auto add = [&](int n, const char* one, const char* many) {
    if (n == 0) return;
    if (!out.empty()) out += ", ";
    out += std::to_string(n) + " " + (n == 1 ? one : many);
  };
  add(systems, "system", "systems");
  add(lua, "Lua table", "Lua tables");
  if (!lua_names.empty()) out += " (" + lua_names + ")";
  add(comps, "component", "components");
  add(assets, "asset type", "asset types");
  add(panels, "panel", "panels");
  add(menus, "menu hook", "menu hooks");
  return out;
}

}  // namespace

PluginsPanelResult draw_plugins_panel(EditorPlugins& plugins,
                                      PluginsPanelState& state, bool riding) {
  PluginsPanelResult result;
  const plugin::Catalog& catalog = plugins.catalog();
  const plugin::PluginManager& mgr = plugins.manager();

  char caption[64];
  std::snprintf(caption, sizeof(caption), "%zu hitched / %zu in the wagon",
                mgr.load_order().size(), catalog.entries().size());
  theme::SectionHeader("Outfitter's Wagon", caption);
  ImGui::TextDisabled("Outfit this claim: switch plugins on or off.");

  if (plugins.pending()) {
    if (riding) {
      theme::StatusWarn("Changes take hold once this ride's over.");
    } else {
      theme::StatusInfo("Hitching up...");
    }
  }
  for (const std::string& e : plugins.catalog_errors()) {
    theme::StatusError(e.c_str());
  }
  if (const std::size_t errs = mgr.error_count()) {
    char line[96];
    std::snprintf(line, sizeof(line),
                  "%zu plugin problem%s: see the Telegraph.", errs,
                  errs == 1 ? "" : "s");
    theme::StatusError(line);
  }

  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##pluginfilter", "Search the wagon", state.filter,
                           sizeof(state.filter));
  ImGui::Spacing();

  const plugin::ProjectType type = mgr.context().project_type;
  for (const plugin::CatalogEntry& e : catalog.entries()) {
    const plugin::PluginInfo& info = e.info;
    if (!matches(info, state.filter)) continue;
    const bool enabled = plugins.is_enabled(info.id);
    const bool loaded = plugins.loaded(info.id);
    const bool fits = (info.project_types & plugin::type_bit(type)) != 0;

    ImGui::PushID(info.id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::PanelBg());
    ImGui::PushStyleColor(ImGuiCol_Border,
                          loaded ? theme::AccentMuted() : theme::Border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::metrics::kRadiusCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::BeginChild("card", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos();
      dl->AddRectFilled(wp, ImVec2(wp.x + 3.0f, wp.y + ImGui::GetWindowSize().y),
                        theme::U32(loaded ? theme::Accent() : theme::Border()),
                        theme::metrics::kRadiusCard, ImDrawFlags_RoundCornersLeft);
    }

    bool on = enabled;
    ImGui::BeginDisabled(!fits && !enabled);
    if (ImGui::Checkbox("##on", &on)) {
      std::string err;
      if (plugins.set_enabled(info.id, on, &err)) {
        result.changed = true;
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(!fits ? "Doesn't work in %s projects"
                              : (enabled ? "Unhitch (saved to project.json)"
                                         : "Hitch up (saved to project.json)"),
                        plugin::to_string(type));
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, loaded ? theme::Accent() : theme::Text());
    ImGui::TextUnformatted(info.name.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", info.version.str().c_str());
    ImGui::SameLine();
    theme::DimensionBadge(plugin::to_string(info.category));

    // Status line.
    if (loaded && enabled) {
      chip("Hitched", theme::Success());
    } else if (loaded && !enabled) {
      chip(riding ? "Unhitching after this ride" : "Unhitching", theme::Warning());
    } else if (enabled && plugins.pending()) {
      chip(riding ? "Rides out next time" : "Hitching up", theme::Warning());
    } else if (enabled) {
      const std::string why = plugins.skip_reason(info.id);
      const std::string line = "Left behind" + (why.empty() ? std::string() : ": " + why);
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
      ImGui::TextWrapped("%s", line.c_str());
      ImGui::PopStyleColor();
    } else {
      chip(fits ? "In the wagon" : "Wrong territory for this claim", theme::TextMuted());
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
    ImGui::TextWrapped("%s", info.description.c_str());
    ImGui::PopStyleColor();

    if (!info.deps.empty()) {
      ImGui::TextDisabled("Needs:");
      for (const plugin::Dependency& d : info.deps) {
        ImGui::SameLine();
        const plugin::CatalogEntry* dep = catalog.find(d.id);
        const bool ok = dep && dep->info.version.satisfies(d.min);
        std::string label = (dep ? dep->info.name : d.id) + " " + d.min.str() + "+";
        if (d.optional) label += " (optional)";
        chip(label.c_str(), ok ? theme::Sand() : theme::Danger());
      }
    }
    const std::string src =
        e.dynamic ? std::filesystem::path(e.source).filename().string() : e.source;
    ImGui::TextDisabled("%s  |  %s  |  %s", info.id.c_str(), src.c_str(),
                        types_label(info.project_types).c_str());
    if (e.dynamic && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", e.source.c_str());
    }
    if (loaded) {
      const std::string adds = contributions(mgr, info.id);
      if (!adds.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::CopperMuted());
        ImGui::TextWrapped("Adds: %s", adds.c_str());
        ImGui::PopStyleColor();
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 2.0f));
  }

  // Enabled ids nobody has (a removed DLL): shown so they can be dropped.
  for (const std::string& id : plugins.enabled()) {
    if (catalog.find(id)) continue;
    ImGui::PushID(id.c_str());
    std::string line = "Missing from the wagon: " + id;
    theme::StatusWarn(line.c_str());
    ImGui::SameLine();
    if (theme::SecondaryButton("Forget")) {
      if (plugins.set_enabled(id, false)) result.changed = true;
    }
    ImGui::PopID();
  }

  ImGui::Spacing();
  ImGui::BeginDisabled(riding);
  if (theme::SecondaryButton("Rescan Wagon")) {
    result.rescan_requested = true;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    std::string dirs;
    for (const std::string& d : plugins::plugin_search_dirs(mgr.context().project_dir)) {
      dirs += "\n  " + d;
    }
    ImGui::SetTooltip("Reload plugin libraries (.dll / .so) from:%s",
                      dirs.empty() ? "\n  (none)" : dirs.c_str());
  }
  return result;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
