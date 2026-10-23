#include "editor/store/TradingPostPanel.h"

#include "editor/plugins/EditorPlugins.h"
#include "editor/projects/ProjectStore.h"
#include "editor/ui/Theme.h"
#include "editor/ux/Tooltip.h"
#include "plugins/Builtins.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

std::string types_label(std::uint32_t mask) {
  std::string s;
  if (mask & plugin::kType2D) s += "2D";
  if (mask & plugin::kType3D) s += s.empty() ? "3D" : " / 3D";
  if (mask & plugin::kTypeVR) s += s.empty() ? "VR" : " / VR";
  return s.empty() ? "any" : s;
}

ImVec4 source_color(store::Source s) {
  switch (s) {
    case store::Source::BuiltIn: return theme::Sand();
    case store::Source::Installed: return theme::Success();
    case store::Source::Local: return theme::Copper();
    case store::Source::Remote: return theme::Accent();
    case store::Source::ComingSoon: return theme::TextMuted();
  }
  return theme::Text();
}

bool project_has(const ProjectInfo& p, const std::string& id) {
  const std::vector<std::string> list =
      p.plugins_set ? p.plugins : EditorPlugins::default_ids(p.kind);
  return std::find(list.begin(), list.end(), id) != list.end();
}

bool enabled_for(const TradingPostTarget& t, const std::string& id) {
  if (t.plugins && t.plugins->is_open()) return t.plugins->is_enabled(id);
  if (t.project) return project_has(*t.project, id);
  return false;
}

void chip(const char* text, const ImVec4& col) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + ts.x + 10.0f, p.y + ts.y + 2.0f),
                    theme::U32(col, 0.18f), 4.0f);
  dl->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + ts.x + 10.0f, p.y + ts.y + 2.0f), theme::U32(col, 0.7f),
              4.0f);
  ImGui::SetCursorScreenPos(ImVec2(p.x + 5.0f, p.y + 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::SetCursorScreenPos(ImVec2(p.x + ts.x + 16.0f, p.y));
  ImGui::Dummy(ImVec2(0, ts.y + 2.0f));
}

void draw_sources(TradingPostUi& ui) {
  store::StoreSources& s = ui.post->sources();
  bool dirty = false;
  ImGui::TextDisabled("Plugin cache: %s", ui.post->cache_root().c_str());
  theme::SectionHeader("Local folders", "plugin libraries on this machine");
  for (std::size_t i = 0; i < s.local_folders.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      s.local_folders.erase(s.local_folders.begin() + static_cast<long>(i));
      dirty = true;
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(s.local_folders[i].c_str());
    ImGui::PopID();
  }
  ImGui::SetNextItemWidth(-90.0f);
  ImGui::InputTextWithHint("##newfolder", "D:\\MyPlugins", ui.new_folder, sizeof(ui.new_folder));
  ImGui::SameLine();
  if (theme::SecondaryButton("Add##folder", ImVec2(80, 0)) && ui.new_folder[0]) {
    s.local_folders.push_back(ui.new_folder);
    ui.new_folder[0] = '\0';
    dirty = true;
  }
  theme::SectionHeader("Remote indexes", "index.json + index.json.sig (file:// or http(s)://)");
  for (std::size_t i = 0; i < s.index_urls.size(); ++i) {
    ImGui::PushID(1000 + static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      s.index_urls.erase(s.index_urls.begin() + static_cast<long>(i));
      dirty = true;
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(s.index_urls[i].c_str());
    ImGui::PopID();
  }
  ImGui::SetNextItemWidth(-90.0f);
  ImGui::InputTextWithHint("##newurl", "https://example.com/tombstone/index.json", ui.new_url,
                           sizeof(ui.new_url));
  ImGui::SameLine();
  if (theme::SecondaryButton("Add##url", ImVec2(80, 0)) && ui.new_url[0]) {
    s.index_urls.push_back(ui.new_url);
    ui.new_url[0] = '\0';
    dirty = true;
  }
  theme::SectionHeader("Trusted publishers", "Ed25519 public keys (hex) that may sign an index");
  for (std::size_t i = 0; i < s.trusted_keys.size(); ++i) {
    ImGui::PushID(2000 + static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      s.trusted_keys.erase(s.trusted_keys.begin() + static_cast<long>(i));
      dirty = true;
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(s.trusted_keys[i].c_str());
    ImGui::PopID();
  }
  ImGui::SetNextItemWidth(-90.0f);
  ImGui::InputTextWithHint("##newkey", "64 hex characters", ui.new_key, sizeof(ui.new_key));
  ImGui::SameLine();
  if (theme::SecondaryButton("Add##key", ImVec2(80, 0)) && std::strlen(ui.new_key) == 64) {
    s.trusted_keys.push_back(ui.new_key);
    ui.new_key[0] = '\0';
    dirty = true;
  }
  if (ImGui::Checkbox("Allow unsigned indexes (development only)", &s.allow_unsigned)) dirty = true;
  if (dirty) {
    std::string err;
    if (!ui.post->save_sources(&err)) ui.error = err;
    ui.loaded = false;  // refresh with the new sources
  }
}

}  // namespace

std::string trading_post_entry_tip(const store::StoreEntry& e, bool hitched) {
  std::string t = e.name + "  v" + e.version;
  if (!e.author.empty()) t += "  by " + e.author;
  if (!e.description.empty()) {
    std::string d = e.description;
    if (d.size() > 240) d = d.substr(0, 237) + "...";
    t += "\n" + d;
  }
  t += "\nWorks in: " + types_label(e.types) + " claims.  ";
  switch (e.source) {
    case store::Source::BuiltIn: t += "Built in: ships with TombStone."; break;
    case store::Source::Local: t += "From a local folder: " + e.origin; break;
    case store::Source::ComingSoon: t += "Coming soon: nothing to install yet."; break;
    default: t += "From " + (e.origin.empty() ? std::string("a remote index") : e.origin) + "."; break;
  }
  if (!e.deps.empty()) {
    t += "\nNeeds:";
    for (const store::StoreDep& d : e.deps) t += " " + d.id + (d.optional ? " (optional)" : "");
  }
  t += "\n";
  if (e.source == store::Source::ComingSoon) {
    t += "Check back after an update.";
  } else if (e.update_available) {
    t += "Update available (" + e.installed_version + " installed). Select it, then Update.";
  } else if (hitched) {
    t += "Hitched to this claim. Untick it in the details to switch it off.";
  } else if (e.installed || e.source == store::Source::BuiltIn) {
    t += "Installed. Select it, then tick Hitched to use it in this claim.";
  } else {
    t += "Select it, then Install (downloads are checked against their SHA-256).";
  }
  return t;
}

bool set_plugin_enabled_in_project(ProjectInfo& project, const std::string& id, bool on,
                                   const store::TradingPost& post, std::string* error,
                                   std::vector<std::string>* also) {
  std::vector<std::string> list =
      project.plugins_set ? project.plugins : EditorPlugins::default_ids(project.kind);
  auto has = [&](const std::string& x) { return std::find(list.begin(), list.end(), x) != list.end(); };
  if (on) {
    std::set<std::string> seen;
    std::function<bool(const std::string&)> add = [&](const std::string& x) -> bool {
      if (!seen.insert(x).second) return true;
      const store::StoreEntry* e = post.find(x);
      if (!e) {
        if (error) *error = "No plugin \"" + x + "\"";
        return false;
      }
      if (e->source == store::Source::ComingSoon) {
        if (error) *error = e->name + " is coming soon";
        return false;
      }
      for (const store::StoreDep& d : e->deps) {
        if (!d.optional && !add(d.id)) return false;
      }
      if (!has(x)) {
        list.push_back(x);
        if (also && x != id) also->push_back(x);
      }
      return true;
    };
    if (!add(id)) return false;
  } else {
    std::vector<std::string> drop = {id};
    for (bool grew = true; grew;) {
      grew = false;
      for (const std::string& x : list) {
        if (std::find(drop.begin(), drop.end(), x) != drop.end()) continue;
        const store::StoreEntry* e = post.find(x);
        if (!e) continue;
        for (const store::StoreDep& d : e->deps) {
          if (!d.optional && std::find(drop.begin(), drop.end(), d.id) != drop.end()) {
            drop.push_back(x);
            if (also) also->push_back(x);
            grew = true;
            break;
          }
        }
      }
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const std::string& x) {
                                return std::find(drop.begin(), drop.end(), x) != drop.end();
                              }),
               list.end());
  }
  project.plugins = list;
  project.plugins_set = true;
  return ProjectStore::write_project_json(project, error);
}

void trading_post_refresh(TradingPostUi& ui, const std::string& project_dir) {
  if (!ui.post) {
    ui.post = std::make_unique<store::TradingPost>();
    std::string err;
    if (!ui.post->load_sources(&err)) ui.error = err;
  }
  plugin::Catalog known;
  std::vector<std::string> errs;
  plugins::fill_catalog(known, project_dir, &errs);
  register_editor_builtin_plugins(known, &errs);
  ui.post->refresh(&known);
  ui.loaded = true;
}

TradingPostResult draw_trading_post_window(TradingPostUi& ui, const TradingPostTarget& target) {
  TradingPostResult result;
  if (!ui.open) return result;
  if (!ui.loaded) trading_post_refresh(ui, !target.project_dir.empty() ? target.project_dir : target.project ? target.project->path : std::string());
  store::TradingPost& post = *ui.post;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                 vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(1080.0f, vp->WorkSize.x - 40.0f),
                                  std::min(680.0f, vp->WorkSize.y - 40.0f)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowFocus();
  if (!ImGui::Begin("Trading Post###TradingPost", &ui.open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return result;
  }

  char caption[160];
  std::snprintf(caption, sizeof(caption), "%zu goods on the shelf%s%s", post.entries().size(),
                target.project ? "  |  outfitting " : "",
                target.project ? target.project->name.c_str() : "");
  theme::SectionHeader("Trading Post", caption);
  ImGui::SetNextItemWidth(320.0f);
  ImGui::InputTextWithHint("##tpsearch", "Search plugins (name, tag, author)", ui.search,
                           sizeof(ui.search));
  ImGui::SameLine();
  if (theme::SecondaryButton("Refresh", ImVec2(90, 0))) {
    trading_post_refresh(ui, !target.project_dir.empty() ? target.project_dir : target.project ? target.project->path : std::string());
    ui.status = "Shelves restocked.";
  }
  if (ImGui::IsItemHovered()) ux::SetTooltip("Reload the catalogue from built-ins, your folders and index URLs.");
  ImGui::SameLine();
  theme::ToggleButton("Sources...", &ui.show_sources);
  if (ImGui::IsItemHovered()) ux::SetTooltip("Where plugins come from: local folders, index URLs and signing keys.");
  if (!ui.status.empty()) {
    ImGui::SameLine();
    theme::StatusSuccess(ui.status.c_str());
  }
  if (!ui.error.empty()) theme::StatusError(ui.error.c_str());

  if (ui.show_sources) {
    ImGui::BeginChild("tpsources", ImVec2(0, 250), ImGuiChildFlags_Borders);
    draw_sources(ui);
    ImGui::EndChild();
  }

  const float notes_h = post.notes().empty() ? 0.0f : 58.0f;
  // Categories.
  ImGui::BeginChild("tpcats", ImVec2(170, -notes_h), ImGuiChildFlags_Borders);
  std::vector<std::string> cats = {""};
  cats.push_back("Installed");
  cats.push_back("Updates");
  for (const std::string& c : post.categories()) cats.push_back(c);
  cats.push_back("Coming Soon");
  for (const std::string& c : cats) {
    const std::size_t n = post.search("", c).size();
    if (n == 0 && c == "Updates") continue;
    char label[96];
    std::snprintf(label, sizeof(label), "%s  (%zu)", c.empty() ? "All goods" : c.c_str(), n);
    if (ImGui::Selectable(label, ui.category == c)) ui.category = c;
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // List.
  const std::vector<const store::StoreEntry*> shown = post.search(ui.search, ui.category);
  if (ui.selected.empty() && !shown.empty()) ui.selected = shown.front()->id;
  ImGui::BeginChild("tplist", ImVec2(380, -notes_h), ImGuiChildFlags_Borders);
  if (shown.empty()) ImGui::TextDisabled("Nothing on this shelf.");
  for (const store::StoreEntry* e : shown) {
    ImGui::PushID(e->id.c_str());
    const bool sel = ui.selected == e->id;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##row", sel, 0, ImVec2(0, 44))) ui.selected = e->id;
    if (ImGui::IsItemHovered()) ux::SetTooltip("%s", trading_post_entry_tip(*e, enabled_for(target, e->id)).c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(p0.x + 6, p0.y + 3), theme::U32(sel ? theme::Accent() : theme::Text()),
                e->name.c_str());
    const std::string v = "v" + e->version;
    const float vw = ImGui::CalcTextSize(v.c_str()).x;
    dl->AddText(ImVec2(p0.x + ImGui::GetContentRegionAvail().x - vw - 4, p0.y + 3),
                theme::U32(theme::TextMuted()), v.c_str());
    std::string line = std::string(store::to_string(e->source)) + "  " + e->category;
    if (e->update_available) line += "  |  update";
    if (enabled_for(target, e->id)) line += "  |  hitched here";
    dl->AddText(ImVec2(p0.x + 6, p0.y + 23), theme::U32(source_color(e->source)), line.c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // Details.
  ImGui::BeginChild("tpdetail", ImVec2(0, -notes_h), ImGuiChildFlags_Borders);
  const store::StoreEntry* e = post.find(ui.selected);
  if (!e) {
    ImGui::TextDisabled("Pick something off the shelf.");
  } else {
    theme::DrawSizedText(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), 24.0f,
                         theme::U32(theme::Accent()), e->name.c_str());
    ImGui::Dummy(ImVec2(0, 28));
    ImGui::TextDisabled("%s  |  v%s%s%s", e->id.c_str(), e->version.c_str(),
                        e->author.empty() ? "" : "  |  by ", e->author.c_str());
    chip(store::to_string(e->source), source_color(e->source));
    ImGui::SameLine();
    chip(e->category.c_str(), theme::Copper());
    ImGui::SameLine();
    chip(types_label(e->types).c_str(), theme::Sand());
    if (e->source == store::Source::BuiltIn && e->optional) {
      ImGui::SameLine();
      chip("optional: ships only when enabled", theme::Success());
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(e->description.empty() ? "(no description)" : e->description.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (e->deps.empty()) {
      ImGui::TextDisabled("Needs: nothing else");
    } else {
      ImGui::TextUnformatted("Needs:");
      for (const store::StoreDep& d : e->deps) {
        const store::StoreEntry* de = post.find(d.id);
        const bool have = de && de->installed;
        ImGui::BulletText("%s%s%s%s  %s", d.id.c_str(), d.min.empty() ? "" : " >= ",
                          d.min.c_str(), d.optional ? " (optional)" : "",
                          have ? "[in the wagon]" : de ? "[installs with it]" : "[not offered]");
      }
    }
    if (!e->tags.empty()) {
      std::string tags;
      for (const std::string& t : e->tags) tags += (tags.empty() ? "" : ", ") + t;
      ImGui::TextDisabled("Tags: %s", tags.c_str());
    }
    ImGui::TextDisabled("From: %s", e->origin.c_str());
    if (!e->installed_dir.empty()) ImGui::TextDisabled("Installed in: %s", e->installed_dir.c_str());
    if (!e->package_sha256.empty()) {
      ImGui::TextDisabled("SHA-256: %.16s... (checked on download)", e->package_sha256.c_str());
    }
    ImGui::Spacing();
    theme::Divider();

    // Actions.
    if (e->source == store::Source::ComingSoon) {
      theme::StatusInfo("Coming soon: on the wagon trail, nothing to install yet.");
    } else {
      if ((e->installable() && !e->installed) || e->update_available) {
        const char* label = e->update_available ? "Update" : "Install";
        const bool clicked = theme::PrimaryButton(label, ImVec2(120, 0));
        if (ImGui::IsItemHovered())
          ux::SetTooltip("%s", e->update_available ? "Download and install the newer version." : "Download and install into the plugin cache. Then hitch it to a claim.");
        if (clicked) {
          std::string err;
          const std::string id = e->id;
          if (post.install(id, &err)) {
            ui.status = std::string(label) + "ed " + post.find(id)->name;
            ui.error.clear();
            result.installed_changed = true;
          } else {
            ui.error = err;
          }
          e = post.find(id);
        }
        ImGui::SameLine();
      }
      if (e && e->installed && !e->installed_dir.empty()) {
        const bool clicked = theme::DangerButton("Uninstall", ImVec2(110, 0));
        if (ImGui::IsItemHovered()) ux::SetTooltip("Remove it from the plugin cache. Claims that use it will warn until you reinstall.");
        if (clicked) {
          std::string err;
          const std::string id = e->id;
          if (post.uninstall(id, &err)) {
            ui.status = "Uninstalled " + id;
            ui.error.clear();
            result.installed_changed = true;
          } else {
            ui.error = err;
          }
          e = post.find(id);
        }
        ImGui::SameLine();
      }
      if (e && (target.project || target.plugins)) {
        const bool on = enabled_for(target, e->id);
        const bool usable = e->installed || e->source == store::Source::BuiltIn;
        const bool in_wagon = !target.plugins || target.plugins->catalog().find(e->id) != nullptr;
        ImGui::BeginDisabled(!usable || (!on && !in_wagon));
        bool want = on;
        if (ImGui::Checkbox(target.project ? "Hitched to this claim" : "Enabled", &want)) {
          std::string err;
          bool ok = false;
          if (target.plugins && target.plugins->is_open()) {
            ok = target.plugins->set_enabled(e->id, want, &err);
          } else if (target.project) {
            std::vector<std::string> also;
            ok = set_plugin_enabled_in_project(*target.project, e->id, want, post, &err, &also);
          }
          if (ok) {
            ui.status = std::string(want ? "Hitched " : "Unhitched ") + e->name;
            ui.error.clear();
            result.enabled_changed = true;
          } else {
            ui.error = err;
          }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ux::SetTooltip("%s", !usable ? "Install it first, then tick to use it in this claim."
                                       : "Tick to use this plugin in the claim (its needs come along too); untick to switch it off.");
        if (!usable) {
          ImGui::SameLine();
          ImGui::TextDisabled("install it first");
        } else if (!on && !in_wagon) {
          ImGui::SameLine();
          ImGui::TextDisabled("rescanning the wagon...");
        }
      }
    }
  }
  ImGui::EndChild();

  if (!post.notes().empty()) {
    ImGui::BeginChild("tpnotes", ImVec2(0, 0), ImGuiChildFlags_None);
    for (const std::string& n : post.notes()) {
      if (n.find("refused") != std::string::npos || n.find("failed") != std::string::npos) {
        theme::StatusError(n.c_str());
      } else {
        theme::StatusInfo(n.c_str());
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();
  return result;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
