#include "editor/assets/AssetBrowser.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/Actions.h"
#include "editor/scripting/ScriptComponents.h"

#include "editor/assets/AssetImport.h"
#include "editor/ui/Theme.h"
#include "render3d/ModelImport.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ts {
namespace tombstone {
namespace editor {

using assetdb::AssetDb;
using assetdb::Kind;
using json::Value;

namespace {

std::string dir_of(const std::string& rel) {
  const auto s = rel.find_last_of('/');
  return s == std::string::npos ? std::string() : rel.substr(0, s);
}
std::string name_of(const std::string& rel) {
  const auto s = rel.find_last_of('/');
  return s == std::string::npos ? rel : rel.substr(s + 1);
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
ImU32 kind_color(Kind k, float a = 1.0f) {
  ImVec4 c;
  switch (k) {
    case Kind::Texture: c = theme::Accent(); break;
    case Kind::Model: c = theme::Copper(); break;
    case Kind::Audio: c = ImVec4(0.45f, 0.70f, 0.55f, 1); break;
    case Kind::Font: c = theme::Sand(); break;
    default: c = theme::TextMuted(); break;
  }
  c.w *= a;
  return ImGui::GetColorU32(c);
}
const char* kind_glyph(Kind k) {
  switch (k) {
    case Kind::Model: return "3D";
    case Kind::Audio: return "~)";
    case Kind::Font: return "Aa";
    default: return "?";
  }
}
const char* const kPayloads[] = {kAssetAnyPayload, "TS_ASSET_IMAGE", "TS_ASSET3D"};

}  // namespace

void AssetBrowser::open(const std::string& project_dir, AssetBrowserHost host) {
  host_ = std::move(host);
  db_.set_importer(assetdb::import_asset);
  db_.open(project_dir);  // moves legacy sidecar .meta files into .tombstone/meta
  import_settings_ = importsort::load_settings(project_dir);
  thumbs_.clear();
  selected_.clear();
  folder_.clear();
  const AssetDb::MigrationReport& mr = db_.last_migration();
  if (mr.any() && host_.note) {
    host_.note("Asset metadata moved into .tombstone/meta: " + std::to_string(mr.moved) + " .meta file(s) (GUIDs kept; backup in " +
               mr.backup_dir + ", log in .tombstone/meta.log)");
  }
  rescan();
}

void AssetBrowser::set_import_settings(const importsort::Settings& s) {
  import_settings_ = s;
  std::string err;
  if (!importsort::save_settings(db_.project_dir(), s, &err) && host_.note) host_.note("Import settings: " + err);
}

importsort::Result AssetBrowser::import_files(const std::vector<std::string>& all_sources, const std::string& dest_folder) {
  importsort::Result r;
  // Source addons (.gma) open the addon picker instead of being copied in.
  std::vector<std::string> sources;
  for (const std::string& src : all_sources) {
    std::string ext = src.size() >= 4 ? src.substr(src.size() - 4) : std::string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".gma") import_gma(src);
    else sources.push_back(src);
  }
  if (sources.empty()) return r;
  importsort::import_files(db_.project_dir(), sources, dest_folder, import_settings_, &r);
  rescan();
  if (host_.note) {
    std::string where = dest_folder.empty() ? (import_settings_.sort_imports ? "sorted into type folders" : "into assets/")
                                            : "into " + dest_folder;
    host_.note("Imported " + std::to_string(r.mains.size()) + " asset(s), " + std::to_string(r.files.size()) +
               " file(s) " + where);
    for (const std::string& n : r.notes) host_.note("Import: " + n);
    for (const std::string& e : r.errors) host_.note("Import failed: " + e);
  }
  if (!r.mains.empty()) {
    selected_ = r.mains.front();
    const auto slash = selected_.find_last_of('/');
    if (!folder_.empty() && slash != std::string::npos) folder_ = selected_.substr(0, slash);
  }
  return r;
}

void AssetBrowser::take_os_drop(const std::string& folder) {
  const std::vector<std::string> paths = importsort::take_os_drop();
  if (!paths.empty()) import_files(paths, folder);
}

void AssetBrowser::draw_import_popup() {
  if (ImGui::BeginPopup("##importmenu")) {
    if (ux::MenuItem("Import file...")) {
      import_pick_folder_ = false;
      import_picker_.open_file({}, {}, "Pick a file to import");
    }
    if (ux::MenuItem("Import folder (model pack)...")) {
      import_pick_folder_ = true;
      import_picker_.open({});
    }
    if (ux::MenuItem("Import Source addon (.gma)...")) gma_dialog_.open_picker(db_.project_dir());
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("Source engine formats (.mdl/.vtf/.vmt/.gma): pick the models / materials / sounds\n"
                        "to extract into assets/Models/<Addon>/. Dropping a .gma here does the same.");
    }
    ImGui::Separator();
    ImGui::TextDisabled("Destination: %s", folder_.empty() ? (import_settings_.sort_imports ? "sorted by type" : "assets/")
                                                           : folder_.c_str());
    ImGui::TextDisabled("(or drop files from Explorer onto this panel / a folder)");
    ImGui::EndPopup();
  }
  if (import_picker_.draw(import_pick_folder_ ? "Import folder##assets" : "Import file##assets")) {
    const std::string picked = import_picker_.take_result();
    if (!picked.empty()) import_files({picked}, folder_);
  }
}

AssetDb::ScanReport AssetBrowser::rescan() {
  // Moves outside the editor rewrite files on disk: save the open scene
  // first only when needed (we learn that from the scan itself, which does
  // not touch references).
  AssetDb::ScanReport rep = db_.scan();
  orphans_ = rep.orphan_metas;
  if (!rep.relinked.empty() && host_.note) {
    for (const auto& r : rep.relinked) {
      host_.note("Relinked " + r.from + " -> " + r.to + " (GUID kept, matched by " +
                 (r.how == "hash" ? "content" : "file name") + ")");
    }
  }
  if (!rep.adopted.empty() && host_.note) {
    host_.note("Moved " + std::to_string(rep.adopted.size()) + " sidecar .meta file(s) into .tombstone/meta");
  }
  if (!rep.moved.empty()) {
    if (host_.before_disk_change) host_.before_disk_change();
    const std::size_t n = db_.fix_references(rep);
    if (host_.note) {
      host_.note("Assets moved outside the editor: " + std::to_string(rep.moved.size()) +
                 " file(s), " + std::to_string(n) + " reference(s) updated");
    }
    if (host_.after_refs_changed) host_.after_refs_changed();
  }
  std::vector<std::string> changed = rep.changed;
  for (const auto& [from, to] : rep.moved) changed.push_back(to);
  for (const std::string& rel : changed) thumbs_.invalidate(db_.abs(rel));
  if (!rep.changed.empty() && host_.note) {
    host_.note("Reimported " + std::to_string(rep.changed.size()) + " changed asset(s)");
  }
  if (!changed.empty() && host_.on_assets_changed) host_.on_assets_changed(changed);
  return rep;
}

void AssetBrowser::poll(double now, double interval) {
  if (now - last_poll_ < interval) return;
  last_poll_ = now;
  rescan();
}

bool AssetBrowser::move(const std::string& from, const std::string& to, std::string* error) {
  if (host_.before_disk_change) host_.before_disk_change();
  std::size_t n = 0;
  std::string err;
  const bool ok = db_.move(from, to, &err, &n);
  if (ok) {
    if (selected_ == from) selected_ = to;
    thumbs_.invalidate(db_.abs(from));
    if (host_.note) host_.note("Moved " + from + " -> " + to + " (" + std::to_string(n) + " reference(s) updated)");
    if (host_.after_refs_changed) host_.after_refs_changed();
  } else {
    if (host_.note) host_.note("Move failed: " + err);
    if (error) *error = err;
  }
  return ok;
}

bool AssetBrowser::remove(const std::string& rel, bool force, std::vector<assetdb::Ref>* refs,
                          std::string* error) {
  std::string err;
  const bool ok = db_.remove(rel, force, refs, &err);
  if (ok) {
    thumbs_.invalidate(db_.abs(rel));
    if (selected_ == rel) selected_.clear();
    if (host_.note) host_.note("Deleted " + rel);
  } else if (error) {
    *error = err;
  }
  return ok;
}

void AssetBrowser::accept_move_into(const std::string& folder) {
  if (!ImGui::BeginDragDropTarget()) return;
  for (const char* type : kPayloads) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) {
      std::string rel(static_cast<const char*>(p->Data));
      if (db_.by_path(rel) && dir_of(rel) != folder) {
        move(rel, (folder.empty() ? "" : folder + "/") + name_of(rel));
      }
    }
  }
  ImGui::EndDragDropTarget();
}

void AssetBrowser::accept_vault_drop() {
  if (!host_.on_vault_drop || !ImGui::BeginDragDropTarget()) return;
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kVaultItemPayload)) {
    host_.on_vault_drop(std::string(static_cast<const char*>(p->Data)));
  }
  ImGui::EndDragDropTarget();
}

void AssetBrowser::reimport(const std::string& rel) {
  db_.reimport(rel);
  thumbs_.invalidate(db_.abs(rel));
  if (host_.on_assets_changed) host_.on_assets_changed({rel});
}

void AssetBrowser::draw() {
  thumbs_.poll_changes(ImGui::GetTime());
  // Toolbar.
  ImGui::SetNextItemWidth(180);
  ImGui::InputTextWithHint("##assetsearch", "Search assets...", search_, sizeof(search_));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100);
  const char* filters[] = {"All", "Textures", "Models", "Audio", "Fonts"};
  ImGui::Combo("##assetfilter", &filter_, filters, 5);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  ImGui::SliderFloat("##tilesize", &tile_size_, 40.0f, 140.0f, "%.0f px");
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) rescan();
  ImGui::SameLine();
  if (ImGui::Button("New folder")) ImGui::OpenPopup("##newfolder");
  if (ImGui::BeginPopup("##newfolder")) {
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##nf", "folder name", new_folder_, sizeof(new_folder_));
    if (ImGui::Button("Create") && new_folder_[0]) {
      const std::string base = folder_.empty() ? "assets" : folder_;
      db_.make_folder(base + "/" + new_folder_);
      new_folder_[0] = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Import...")) ImGui::OpenPopup("##importmenu");
  draw_import_popup();
  ImGui::SameLine();
  if (ImGui::Button("Options")) ImGui::OpenPopup("##importopts");
  if (ImGui::BeginPopup("##importopts")) {
    importsort::Settings st = import_settings_;
    bool changed = ImGui::Checkbox("Sort imports into type folders", &st.sort_imports);
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("assets/Models/<Name>/, Textures/, Materials/, Audio/, Fonts/, Scenes/, Animations/, Other/,\n"
                        "prefabs/ and scripts/. Dropping on a folder puts files there instead.");
    }
    ImGui::BeginDisabled(!st.sort_imports);
    changed |= ImGui::Checkbox("Keep source folder structure for model packs", &st.keep_model_packs);
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("On: a model's textures stay with it in assets/Models/<Name>/.\n"
                        "Off: they go to assets/Textures/<Name>/ (glTF / OBJ paths rewritten).");
    }
    ImGui::EndDisabled();
    if (changed) set_import_settings(st);
    ImGui::Separator();
    ImGui::TextDisabled("Metadata (.meta) lives in .tombstone/meta");
    ImGui::EndPopup();
  }
  if (gma_dialog_.draw()) {
    const gmaimport::Result& r = gma_dialog_.result();
    rescan();
    if (!r.models.empty()) selected_ = r.models.front();
    if (host_.note) {
      host_.note("Imported " + std::to_string(r.written.size()) + " file(s) from the addon into " + r.dest +
                 (r.skipped.empty() ? "" : " (" + std::to_string(r.skipped.size()) + " skipped)"));
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu assets", db_.entries().size());
  if (!orphans_.empty()) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
    if (ImGui::SmallButton((std::to_string(orphans_.size()) + " orphaned .meta").c_str())) orphans_popup_ = true;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ux::SetTooltip("Metadata whose file is gone (deleted or moved outside the editor and not found)");
  }

  const float details_h = selected_.empty() ? 0.0f : 150.0f;
  const float body_h = ImGui::GetContentRegionAvail().y - details_h;
  if (ImGui::BeginChild("##folders", ImVec2(170, body_h), ImGuiChildFlags_Borders)) draw_folders();
  ImGui::EndChild();
  ImGui::SameLine();
  if (ImGui::BeginChild("##grid", ImVec2(0, body_h), ImGuiChildFlags_Borders)) draw_grid();
  ImGui::EndChild();
  grid_hovered_ = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  accept_vault_drop();  // a Vault tile dropped on the grid: pull it into the claim
  if (!selected_.empty()) {
    if (ImGui::BeginChild("##details", ImVec2(0, 0), ImGuiChildFlags_Borders)) draw_details();
    ImGui::EndChild();
  }

  // Files dropped from the OS: onto a folder row = that folder; onto the
  // grid = the open folder (or sorted when showing all assets).
  if (importsort::os_drop_pending() && !db_.project_dir().empty()) {
    if (drop_seen_ < 0) drop_seen_ = ImGui::GetTime();
    if (!drop_folder_.empty()) take_os_drop(drop_folder_ == "\x01all" ? std::string() : drop_folder_);
    else if (grid_hovered_) take_os_drop(folder_);
    else if (ImGui::GetTime() - drop_seen_ > 0.25) take_os_drop({});  // dropped elsewhere: sorted
    if (!importsort::os_drop_pending()) drop_seen_ = -1.0;
  }
  // Orphaned metadata: confirm before anything is removed.
  if (orphans_popup_) {
    ImGui::OpenPopup("Orphaned asset metadata");
    orphans_popup_ = false;
  }
  if (ImGui::BeginPopupModal("Orphaned asset metadata", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("These .meta files belong to assets that are no longer in the claim:");
    const std::size_t show = std::min<std::size_t>(orphans_.size(), 12);
    for (std::size_t i = 0; i < show; ++i) ImGui::BulletText("%s", orphans_[i].c_str());
    if (orphans_.size() > show) ImGui::TextDisabled("... and %zu more", orphans_.size() - show);
    ImGui::TextDisabled("If a file was moved outside the editor, put it back or rescan first:\n"
                        "a file with the same content or name gets its GUID back.");
    if (ImGui::Button("Move to .tombstone/meta_orphans")) {
      const int n = db_.clean_orphans(orphans_, true);
      if (host_.note) host_.note("Moved " + std::to_string(n) + " orphaned .meta file(s) to .tombstone/meta_orphans");
      rescan();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Danger());
    if (ImGui::Button("Delete them")) {
      const int n = db_.clean_orphans(orphans_, false);
      if (host_.note) host_.note("Deleted " + std::to_string(n) + " orphaned .meta file(s)");
      rescan();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Keep")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // Delete confirmation (asset in use).
  if (!delete_pending_.empty()) ImGui::OpenPopup("Delete asset in use?");
  if (ImGui::BeginPopupModal("Delete asset in use?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(theme::Warning(), "%s is still referenced:", delete_pending_.c_str());
    for (const assetdb::Ref& r : delete_refs_) ImGui::BulletText("%s (%d)", r.file.c_str(), r.count);
    ImGui::TextDisabled("Those references will point at a missing file.");
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Danger());
    if (ImGui::Button("Delete anyway")) {
      remove(delete_pending_, true);
      delete_pending_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Keep it")) {
      delete_pending_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void AssetBrowser::draw_folders() {
  drop_folder_.clear();
  if (ImGui::Selectable("All assets", folder_.empty())) folder_.clear();
  if (ImGui::IsItemHovered()) drop_folder_ = "\x01all";
  accept_move_into("assets");
  accept_vault_drop();
  for (const std::string& f : db_.folders()) {
    const int depth = static_cast<int>(std::count(f.begin(), f.end(), '/'));
    ImGui::Indent(10.0f * depth + 4.0f);
    std::size_t n = 0;
    for (const auto& e : db_.entries()) n += dir_of(e.rel) == f;
    const std::string label = name_of(f) + (n ? "  (" + std::to_string(n) + ")" : "") + "##" + f;
    ImGui::PushStyleColor(ImGuiCol_Text, folder_ == f ? theme::Accent() : theme::Text());
    if (ImGui::Selectable(label.c_str(), folder_ == f)) folder_ = f;
    if (ImGui::IsItemHovered()) drop_folder_ = f;
    ImGui::PopStyleColor();
    accept_move_into(f);
    ImGui::Unindent(10.0f * depth + 4.0f);
  }
}

bool AssetBrowser::tile(const AssetDb::Entry& e, float size) {
  ImGui::PushID(e.rel.c_str());
  ImGui::BeginGroup();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(size, size + 18));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool sel = selected_ == e.rel;
  dl->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size + 18),
                    ImGui::GetColorU32(sel ? theme::AccentMuted() : (hovered ? theme::HeaderBg() : theme::PanelBg())),
                    4.0f);
  const ImVec2 t0(p0.x + 4, p0.y + 4), t1(p0.x + size - 4, p0.y + size - 4);
  bool drew = false;
  if (e.meta.kind == Kind::Texture && e.import_ok) {
    const gfx::TextureInfo& ti = thumbs_.get(db_.abs(e.rel));
    if (ti.ok && ti.handle && ti.width > 0 && ti.height > 0) {
      const float s = std::min((t1.x - t0.x) / ti.width, (t1.y - t0.y) / ti.height);
      const float w = ti.width * s, h = ti.height * s;
      const ImVec2 c((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f);
      dl->AddRectFilled(t0, t1, ImGui::GetColorU32(theme::Charcoal()), 3.0f);
      dl->AddImage((ImTextureID)(std::uintptr_t)ti.handle, ImVec2(c.x - w / 2, c.y - h / 2),
                   ImVec2(c.x + w / 2, c.y + h / 2));
      drew = true;
    }
  }
  if (!drew) {
    dl->AddRectFilled(t0, t1, kind_color(e.meta.kind, 0.22f), 6.0f);
    dl->AddRect(t0, t1, kind_color(e.meta.kind, 0.8f), 6.0f, 0, 1.5f);
    const char* g = e.meta.kind == Kind::Texture ? "IMG" : kind_glyph(e.meta.kind);
    const float px = size * 0.32f;
    const ImVec2 ts = theme::MeasureText(px, g);
    theme::DrawSizedText(dl, ImVec2((t0.x + t1.x - ts.x) * 0.5f, (t0.y + t1.y - ts.y) * 0.5f), px,
                         kind_color(e.meta.kind), g);
  }
  if (!e.import_ok) dl->AddText(ImVec2(t0.x + 2, t0.y), ImGui::GetColorU32(theme::Danger()), "!");
  // Vault origin: a marker for pulled / sent assets, a copper "UPD" when a newer version waits.
  if (const int vb = host_.vault_badge ? host_.vault_badge(e.rel) : 0) {
    const char* mark = vb == 2 ? "UPD" : "V";
    const ImVec2 ms = ImGui::CalcTextSize(mark);
    const ImVec4 col = vb == 2 ? theme::Copper() : (vb == 3 ? theme::Accent() : theme::Success());
    dl->AddRectFilled(ImVec2(t1.x - ms.x - 7, t0.y + 1), ImVec2(t1.x - 1, t0.y + ms.y + 3),
                      ImGui::GetColorU32(col), 3.0f);
    dl->AddText(ImVec2(t1.x - ms.x - 4, t0.y + 2), ImGui::GetColorU32(theme::TextOnAccent()), mark);
  }
  dl->AddRectFilled(ImVec2(t0.x, t1.y - 3), ImVec2(t1.x, t1.y), kind_color(e.meta.kind));
  std::string name = name_of(e.rel);
  const float max_w = size - 4;
  while (name.size() > 3 && ImGui::CalcTextSize(name.c_str()).x > max_w) {
    name = name.substr(0, name.size() - 4) + "..";
  }
  dl->AddText(ImVec2(p0.x + 2, p0.y + size), ImGui::GetColorU32(sel ? theme::Accent() : theme::Text()),
              name.c_str());
  ImGui::EndGroup();
  // Drag: into the scene (host payload) or onto a folder.
  if (ImGui::BeginDragDropSource()) {
    const std::string type = host_.payload_type ? host_.payload_type(e) : kAssetAnyPayload;
    ImGui::SetDragDropPayload(type.c_str(), e.rel.c_str(), e.rel.size() + 1);
    ImGui::Text("%s", e.rel.c_str());
    ImGui::EndDragDropSource();
  }
  if (hovered) {
    ux::SetTooltip("%s\n%s  %s\nGUID %s", e.rel.c_str(), assetdb::kind_name(e.meta.kind),
                      assetdb::info_summary(e.meta.info, e.meta.kind).c_str(), e.meta.guid.c_str());
  }
  if (ImGui::BeginPopupContextItem("##ctx")) {
    selected_ = e.rel;
    if (ux::MenuItem("Find references")) {
      refs_for_ = e.rel;
      refs_shown_ = db_.find_references(e.rel);
    }
    if (ux::MenuItem("Reimport")) reimport(e.rel);
    if (host_.send_to_vault && ux::MenuItem("Send to Vault...")) host_.send_to_vault(e.rel);
    if (ux::MenuItem("Delete")) {
      std::vector<assetdb::Ref> refs;
      if (!remove(e.rel, false, &refs)) {
        delete_pending_ = e.rel;
        delete_refs_ = refs;
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return clicked;
}

void AssetBrowser::draw_grid() {
  const std::string q = lower(search_);
  const float size = tile_size_;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per_row = std::max(1, static_cast<int>(avail / (size + 8)));
  int col = 0;
  std::size_t shown = 0;
  for (const AssetDb::Entry& e : db_.entries()) {
    if (filter_ == 1 && e.meta.kind != Kind::Texture) continue;
    if (filter_ == 2 && e.meta.kind != Kind::Model) continue;
    if (filter_ == 3 && e.meta.kind != Kind::Audio) continue;
    if (filter_ == 4 && e.meta.kind != Kind::Font) continue;
    if (!q.empty()) {
      if (lower(e.rel).find(q) == std::string::npos && e.meta.guid.rfind(q, 0) != 0) continue;
      if (!folder_.empty() && e.rel.rfind(folder_ + "/", 0) != 0) continue;
    } else if (!folder_.empty() && dir_of(e.rel) != folder_) {
      continue;
    }
    if (col > 0) ImGui::SameLine(0, 8);
    if (tile(e, size)) {
      selected_ = e.rel;
      std::snprintf(rename_, sizeof(rename_), "%s", e.rel.c_str());
    }
    col = (col + 1) % per_row;
    ++shown;
  }
  if (host_.scripts && (filter_ == 0)) {
    for (const std::string& rel : host_.scripts()) {
      if (!q.empty()) {
        if (lower(rel).find(q) == std::string::npos) continue;
        if (!folder_.empty() && rel.rfind(folder_ + "/", 0) != 0) continue;
      } else if (!folder_.empty() && dir_of(rel) != folder_) {
        continue;
      }
      if (col > 0) ImGui::SameLine(0, 8);
      if (script_tile(rel, size)) selected_ = rel;
      col = (col + 1) % per_row;
      ++shown;
    }
  }
  if (shown == 0) {
    theme::StatusInfo(db_.entries().empty()
                          ? "No assets yet. Drop .png / .jpg / .gltf / .glb / .wav / .ogg / .mp3 / "
                            ".ttf files into the project's assets/ folder."
                          : "Nothing matches.");
  }
}

bool AssetBrowser::script_tile(const std::string& rel, float size) {
  ImGui::PushID(rel.c_str());
  ImGui::BeginGroup();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(size, size + 18));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool sel = selected_ == rel;
  dl->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size + 18),
                    ImGui::GetColorU32(sel ? theme::AccentMuted() : (hovered ? theme::HeaderBg() : theme::PanelBg())),
                    4.0f);
  const ImVec2 t0(p0.x + 4, p0.y + 4), t1(p0.x + size - 4, p0.y + size - 4);
  const ScriptLang lang = lang_of(rel);
  const ImVec4 c = lang_colour(lang);
  dl->AddRectFilled(t0, t1, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.22f)), 6.0f);
  dl->AddRect(t0, t1, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.8f)), 6.0f, 0, 1.5f);
  const char* g = lang_tag(lang);
  const float px = size * 0.26f;
  const ImVec2 ts = theme::MeasureText(px, g);
  theme::DrawSizedText(dl, ImVec2((t0.x + t1.x - ts.x) * 0.5f, (t0.y + t1.y - ts.y) * 0.5f), px,
                       ImGui::GetColorU32(c), g);
  dl->AddRectFilled(ImVec2(t0.x, t1.y - 3), ImVec2(t1.x, t1.y), ImGui::GetColorU32(c));
  std::string name = name_of(rel);
  const float max_w = size - 4;
  while (name.size() > 3 && ImGui::CalcTextSize(name.c_str()).x > max_w) {
    name = name.substr(0, name.size() - 4) + "..";
  }
  dl->AddText(ImVec2(p0.x + 2, p0.y + size), ImGui::GetColorU32(sel ? theme::Accent() : theme::Text()),
              name.c_str());
  ImGui::EndGroup();
  script_drag_source(rel);
  if (hovered) {
    ux::SetTooltip("%s\n%s script - drag onto an entity to attach, double-click to open", rel.c_str(),
                      lang_label(lang));
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && host_.open_script) host_.open_script(rel, 0);
  }
  if (ImGui::BeginPopupContextItem("##ctx")) {
    selected_ = rel;
    if (host_.open_script) {
      if (ux::MenuItem(lang == ScriptLang::Graph ? "Open in Brand Book" : "Open")) host_.open_script(rel, 0);
      if (ux::MenuItem("Open in VS Code")) host_.open_script(rel, 1);
      if (ux::MenuItem("Open in Visual Studio")) host_.open_script(rel, 2);
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return clicked;
}

void AssetBrowser::draw_settings(const AssetDb::Entry& e) {
  Value s = e.meta.settings;
  bool changed = false;
  auto flag = [&](const char* key, const char* label) {
    bool v = s.flag(key, false);
    if (ImGui::Checkbox(label, &v)) {
      s.set(key, Value::boolean(v));
      changed = true;
    }
  };
  auto number = [&](const char* key, const char* label, float lo, float hi, const char* fmt) {
    float v = s.flt(key, 0);
    ImGui::SetNextItemWidth(140);
    if (ImGui::DragFloat(label, &v, (hi - lo) / 400.0f, lo, hi, fmt)) {
      s.set(key, Value::number(v));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
  };
  switch (e.meta.kind) {
    case Kind::Texture: {
      int f = s.str("filter") == "linear" ? 1 : 0;
      const char* fl[] = {"nearest", "linear"};
      ImGui::SetNextItemWidth(140);
      if (ImGui::Combo("Filter", &f, fl, 2)) {
        s.set("filter", Value::string(fl[f]));
        changed = true;
      }
      flag("mipmaps", "Mipmaps");
      ImGui::SameLine();
      flag("srgb", "sRGB");
      number("max_size", "Max size", 16, 16384, "%.0f");
      number("pixels_per_unit", "Pixels / unit", 1, 1024, "%.0f");
      break;
    }
    case Kind::Model: {
      // One import panel for every model format (glTF, Source .mdl, FBX,
      // OBJ, DAE, ... .blend): the same settings, applied by
      // render3d::load_model wherever the model loads.
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(theme::Warning(), "%s", render3d::kModelRightsNote);
      ImGui::PopTextWrapPos();
      number("unit_scale", "Unit scale", 0.0001f, 100, "%.4f");
      if (ImGui::IsItemHovered()) ux::SetTooltip("File units to metres (Source: 0.0254, centimetres: 0.01)");
      int up = s.str("up_axis") == "y" ? 1 : s.str("up_axis") == "z" ? 2 : 0;
      const char* ups[] = {"Auto (file)", "Y up", "Z up (convert)"};
      ImGui::SetNextItemWidth(140);
      if (ImGui::Combo("Up axis", &up, ups, 3)) {
        s.set("up_axis", Value::string(up == 1 ? "y" : up == 2 ? "z" : "auto"));
        changed = true;
      }
      number("scale", "Placement scale", 0.001f, 1000, "%.3f");
      auto path_field = [&](const char* key, const char* label, char* buf, std::size_t n,
                            std::string* owner, const char* hint) {
        const std::string want = e.rel + "|" + key;
        if (*owner != want) {
          *owner = want;
          std::snprintf(buf, n, "%s", s.str(key).c_str());
        }
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint(label, hint, buf, n);
        if (ImGui::IsItemDeactivatedAfterEdit() && s.str(key) != buf) {
          s.set(key, Value::string(buf));
          changed = true;
        }
      };
      path_field("material_path", "Material search path", material_path_, sizeof(material_path_),
                 &material_path_for_, "e.g. C:/.../GarrysMod/garrysmod; more with ;");
      const int lods = std::max(1, static_cast<int>(e.meta.info.num("lods", 1)));
      if (lods > 1 || s.num("lod", 0) > 0) {
        int lod = static_cast<int>(s.num("lod", 0));
        ImGui::SetNextItemWidth(140);
        if (ImGui::SliderInt("LOD", &lod, 0, std::max(1, lods - 1))) s.set("lod", Value::number(lod));
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
      }
      const int skins = static_cast<int>(e.meta.info.num("skins", 0));
      if (skins > 1) {
        int sk = static_cast<int>(s.num("skin", 0));
        ImGui::SetNextItemWidth(140);
        if (ImGui::SliderInt("Skin", &sk, 0, skins - 1)) s.set("skin", Value::number(sk));
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
      }
      const Value& groups = e.meta.info["body_groups"];
      if (groups.is_array()) {
        for (const Value& g : groups.items()) {
          const Value& opts = g["options"];
          if (!opts.is_array() || opts.items().size() < 2) continue;
          const std::string name = g.str("name");
          Value bg = s["body_groups"].is_object() ? s["body_groups"] : Value::object();
          int cur = static_cast<int>(bg.num(name, 0));
          const std::string preview = cur >= 0 && cur < static_cast<int>(opts.items().size())
                                          ? opts.items()[cur].as_string() : std::string("?");
          ImGui::SetNextItemWidth(140);
          if (ImGui::BeginCombo(("Body: " + name).c_str(), preview.c_str())) {
            for (int i = 0; i < static_cast<int>(opts.items().size()); ++i) {
              if (ImGui::Selectable(opts.items()[i].as_string().c_str(), i == cur)) {
                bg.set(name, Value::number(i));
                s.set("body_groups", bg);
                changed = true;
              }
            }
            ImGui::EndCombo();
          }
        }
      }
      {
        // Collider a placed model gets (render3d/ColliderFit.h). "None"
        // also skips the file's own collision (.phy / UCX_ meshes).
        static const char* keys[] = {"auto", "box", "capsule", "convex", "mesh", "none"};
        const bool hulls = e.meta.info.num("hulls", 0) > 0;
        const char* labels = hulls ? "Auto (model hulls)\0Tight box\0Capsule\0Convex hull\0Mesh\0None\0"
                                   : (e.meta.info.flag("skinned", false)
                                          ? "Auto (capsule)\0Tight box\0Capsule\0Convex hull\0Mesh\0None\0"
                                          : "Auto (tight box)\0Tight box\0Capsule\0Convex hull\0Mesh\0None\0");
        int ci = 0;
        const std::string cur = s.flag("generate_collider", true) ? s.str("collider", "auto") : "none";
        for (int k = 0; k < 6; ++k) {
          if (cur == keys[k]) ci = k;
        }
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo("Collider", &ci, labels)) {
          s.set("collider", Value::string(keys[ci]));
          s.set("generate_collider", Value::boolean(ci != 5));
          changed = true;
        }
      }
      ImGui::SameLine();
      flag("import_animations", "Animations");
      if (e.meta.info.num("hitboxes", 0) > 0) {
        ImGui::TextDisabled("%d hitboxes (set \"%s\"): View > Hitboxes shows them",
                            static_cast<int>(e.meta.info.num("hitboxes", 0)), e.meta.info.str("hitbox_set").c_str());
      }
      if (s.has("blender_path")) {
        path_field("blender_path", "Blender", blender_path_, sizeof(blender_path_), &blender_path_for_,
                   "auto (registry / PATH / Program Files)");
      }
      if (e.meta.info.has("warning")) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(theme::TextMuted(), "%s", e.meta.info.str("warning").c_str());
        ImGui::PopTextWrapPos();
      }
      break;
    }
    case Kind::Audio: {
      int b = 0;
      const char* buses[] = {"sfx", "music", "voice", "ambience", "ui"};
      for (int i = 0; i < 5; ++i) {
        if (s.str("bus") == buses[i]) b = i;
      }
      ImGui::SetNextItemWidth(140);
      if (ImGui::Combo("Bus", &b, buses, 5)) {
        s.set("bus", Value::string(buses[b]));
        changed = true;
      }
      number("volume", "Volume", 0, 2, "%.2f");
      flag("loop", "Loop");
      ImGui::SameLine();
      flag("stream", "Stream");
      break;
    }
    case Kind::Font:
      number("size", "Size", 6, 256, "%.0f px");
      number("oversample", "Oversample", 1, 4, "%.0f");
      break;
    default: break;
  }
  if (changed) {
    std::string err;
    if (!db_.set_settings(e.rel, s, &err) && host_.note) host_.note(err);
  }
}

void AssetBrowser::draw_details() {
  const AssetDb::Entry* e = db_.by_path(selected_);
  if (!e) {
    selected_.clear();
    return;
  }
  const AssetDb::Entry entry = *e;  // stable copy (actions may reshuffle)
  theme::SectionHeader(name_of(entry.rel).c_str(), assetdb::kind_name(entry.meta.kind));
  if (ImGui::BeginTable("##det", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("GUID %s", entry.meta.guid.c_str());
    if (host_.vault_origin) {
      const std::string origin = host_.vault_origin(entry.rel);
      if (!origin.empty()) ImGui::TextColored(origin.find("update") != std::string::npos ? theme::Copper() : theme::Sand(), "%s", origin.c_str());
    }
    if (entry.import_ok) {
      ImGui::Text("%s", assetdb::info_summary(entry.meta.info, entry.meta.kind).c_str());
    } else {
      theme::StatusError(entry.import_error.c_str());
    }
    ImGui::SetNextItemWidth(-90);
    ImGui::InputText("##rename", rename_, sizeof(rename_));
    ImGui::SameLine();
    if (ImGui::Button("Rename") && entry.rel != rename_) move(entry.rel, rename_);
    if (ImGui::Button("Find references")) {
      refs_for_ = entry.rel;
      refs_shown_ = db_.find_references(entry.rel);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reimport")) reimport(entry.rel);
    if (host_.send_to_vault) {
      ImGui::SameLine();
      if (ImGui::Button("Send to Vault")) host_.send_to_vault(entry.rel);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, theme::CopperMuted());
    if (ImGui::Button("Delete")) {
      std::vector<assetdb::Ref> refs;
      if (!remove(entry.rel, false, &refs)) {
        delete_pending_ = entry.rel;
        delete_refs_ = refs;
      }
    }
    ImGui::PopStyleColor();
    if (refs_for_ == entry.rel) {
      if (refs_shown_.empty()) {
        ImGui::TextDisabled("No references: safe to delete.");
      } else {
        for (const assetdb::Ref& r : refs_shown_) ImGui::BulletText("%s (%d)", r.file.c_str(), r.count);
      }
    }
    ImGui::TableNextColumn();
    ImGui::TextColored(theme::Accent(), "Import settings");
    draw_settings(entry);
    if (entry.meta.kind == Kind::Model &&
        gmaimport::draw_source_materials(db_, entry, &relink_picker_, &relink_for_)) {
      reimport(entry.rel);
    }
    ImGui::EndTable();
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
