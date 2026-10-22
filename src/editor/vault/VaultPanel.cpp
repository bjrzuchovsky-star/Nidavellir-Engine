#include "editor/vault/VaultPanel.h"
#include "editor/ux/FriendlyError.h"
#include "editor/ux/EmptyState.h"
#include "editor/ux/Tooltip.h"

#include "editor/settings/Settings.h"
#include "editor/vault/FilePick.h"
#include "editor/ui/Theme.h"
#include "vault/VaultBackend.h"
#include "vault/VaultLock.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

const char* const kKinds[] = {"All kinds", "Textures", "Models", "Audio", "Fonts", "Prefabs"};
const char* const kKindNames[] = {"", "texture", "model", "audio", "font", "prefab"};
const char* const kDims[] = {"2D/3D/VR", "2D", "3D", "VR"};
const char* const kDimNames[] = {"", "2d", "3d", "vr"};
const char* const kLicences[] = {"CC0-1.0", "CC-BY-4.0", "CC-BY-SA-4.0", "MIT", "Proprietary (studio only)",
                                 "Custom..."};
constexpr int kLicenceCount = 6;

ImVec4 kind_colour(const std::string& k) {
  if (k == "texture") return theme::Accent();
  if (k == "model") return theme::Copper();
  if (k == "audio") return ImVec4(0.45f, 0.70f, 0.55f, 1);
  if (k == "font") return theme::Sand();
  if (k == "prefab") return ImVec4(0.55f, 0.62f, 0.85f, 1);
  return theme::TextMuted();
}

const char* kind_glyph(const std::string& k) {
  if (k == "texture") return "IMG";
  if (k == "model") return "3D";
  if (k == "audio") return "~)";
  if (k == "font") return "Aa";
  if (k == "prefab") return "[P]";
  return "?";
}

std::string human_size(std::uint64_t b) {
  char buf[32];
  if (b >= (1u << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
  else if (b >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
  else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
  return buf;
}

void copy_to(char* dst, std::size_t n, const std::string& s) { std::snprintf(dst, n, "%s", s.c_str()); }

}  // namespace

VaultPanel::~VaultPanel() {
  if (job_.valid()) job_.wait();
}

void VaultPanel::open(const std::string& project_dir, VaultHost host) {
  if (job_.valid()) job_.wait();
  job_ = {};
  project_dir_ = project_dir;
  host_ = std::move(host);
  thumb_dir_ = (fs::path(SettingsStore::default_config_dir()) / "vault_thumbs").string();
  items_.clear();
  status_.clear();
  selected_.clear();
  loaded_ = false;
  load_settings();
}

void VaultPanel::load_settings() {
  const Settings s = SettingsStore::load();
  backend_ = s.vault_backend;
  copy_to(local_dir_, sizeof(local_dir_), s.vault_local_dir);
  copy_to(url_, sizeof(url_), s.vault_url);
  copy_to(key_, sizeof(key_), s.vault_api_key);
  username_ = s.username;
}

void VaultPanel::use_library(const std::string& backend, const std::string& local_dir, const std::string& url,
                             const std::string& api_key) {
  backend_ = backend == "remote" ? "remote" : "local";
  copy_to(local_dir_, sizeof(local_dir_), local_dir);
  copy_to(url_, sizeof(url_), url);
  copy_to(key_, sizeof(key_), api_key);
  loaded_ = false;
}

void VaultPanel::save_settings() {
  Settings s = SettingsStore::load();
  s.vault_backend = backend_;
  s.vault_local_dir = local_dir_;
  s.vault_url = url_;
  s.vault_api_key = key_;
  std::string err;
  if (!SettingsStore::save(s, &err)) error_ = err;
}

std::unique_ptr<vault::Backend> VaultPanel::make_backend() const {
  return vault::make_backend(backend_, local_dir_, url_, key_);
}

void VaultPanel::start(const std::string& label, Job job, std::function<void(const JobResult&)> then) {
  if (job_.valid()) {
    status_line_ = "Busy: " + job_label_;
    return;
  }
  job_label_ = label;
  status_line_ = label + "...";
  error_.clear();
  job_then_ = std::move(then);
  job_ = std::async(std::launch::async, std::move(job));
}

void VaultPanel::update() {
  if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  JobResult r = job_.get();
  job_ = {};
  if (r.listed) {
    items_ = std::move(r.items);
    status_ = std::move(r.status);
    for (auto& [k, v] : r.thumbs) thumbs_[k] = v;
    loaded_ = true;
  }
  if (r.has_detail) {
    detail_ = r.detail;
    for (auto& [k, v] : r.thumbs) thumbs_[k] = v;
  }
  if (r.ok) {
    status_line_ = r.message.empty() ? job_label_ + ": done" : r.message;
  } else {
    error_ = r.error;
    status_line_.clear();
  }
  if (r.changed_project && host_.after_pull) host_.after_pull();
  // Re-list (thumbnails, badges) on the next frame.
  if (r.changed_project) loaded_ = false;
  if (!r.message.empty() && host_.note && (r.pulled || r.changed_project)) host_.note("Vault: " + r.message);
  if (!r.ok && host_.note) host_.note("Vault: " + r.error);
  auto then = std::move(job_then_);
  job_then_ = {};
  if (then) then(r);
}

void VaultPanel::refresh() {
  auto backend = make_backend();
  const std::string project = project_dir_;
  const std::string thumbs = thumb_dir_;
  const std::string sel = selected_;
  start("Loading the library", [backend = std::shared_ptr<vault::Backend>(std::move(backend)), project, thumbs, sel] {
    JobResult r;
    r.listed = true;
    if (!backend->list({}, &r.items, &r.error)) {
      r.ok = false;
      return r;
    }
    std::string err;
    if (!project.empty()) vaultp::project_status(project, *backend, &r.status, &err);
    for (const vault::Item& it : r.items) {
      if (it.thumbnail.empty()) continue;
      const std::string p = vaultp::cached_thumbnail(*backend, it.thumbnail, thumbs);
      if (!p.empty()) r.thumbs[it.thumbnail] = p;
    }
    if (!sel.empty() && backend->get(sel, &r.detail, &err)) r.has_detail = true;
    std::size_t updates = 0;
    for (const auto& s : r.status) updates += s.update_available;
    r.message = std::to_string(r.items.size()) + " item(s) in " + backend->label() +
                (updates ? ", " + std::to_string(updates) + " update(s) for this claim" : "");
    return r;
  });
}

void VaultPanel::select(const std::string& id) {
  if (selected_ == id) return;
  selected_ = id;
  pull_version_ = 0;
  detail_ = vault::Item{};
  for (const vault::Item& it : items_) {
    if (it.id == id) detail_ = it;  // versions without files until the fetch lands
  }
  auto backend = std::shared_ptr<vault::Backend>(make_backend());
  if (job_.valid()) return;
  start("Reading item", [backend, id] {
    JobResult r;
    r.has_detail = backend->get(id, &r.detail, &r.error);
    r.ok = r.has_detail;
    r.message = r.ok ? r.detail.name + ": " + std::to_string(r.detail.versions.size()) + " version(s)" : "";
    return r;
  });
}

const vaultp::ItemStatus* VaultPanel::status_of(const std::string& id) const {
  for (const auto& s : status_) {
    if (s.item == id) return &s;
  }
  return nullptr;
}

int VaultPanel::badge(const std::string& rel) const {
  for (const auto& s : status_) {
    if (s.main != rel) continue;
    if (s.origin == "pushed") return 3;
    return s.update_available ? 2 : 1;
  }
  return 0;
}

std::string VaultPanel::origin_label(const std::string& rel) const {
  for (const auto& s : status_) {
    if (s.main != rel) continue;
    std::string t = (s.origin == "pushed" ? "Sent to the Vault: " : "From the Vault: ") + s.name + " v" +
                    std::to_string(s.version);
    if (s.pinned) t += " (pinned)";
    if (s.update_available) t += " - update v" + std::to_string(s.latest) + " available";
    if (s.modified) t += " - changed here";
    return t;
  }
  return {};
}

void VaultPanel::pull_then(const std::string& item, std::function<void(const vaultp::PullResult&)> done) {
  auto backend = std::shared_ptr<vault::Backend>(make_backend());
  const std::string project = project_dir_;
  const int version = item == selected_ ? pull_version_ : 0;
  start("Pulling", [backend, project, item, version] {
    JobResult r;
    vaultp::PullOptions opt;
    opt.version = version;
    r.ok = vaultp::pull_item(project, nullptr, *backend, item, opt, &r.pull, &r.error);
    r.pulled = r.ok;
    r.changed_project = true;
    if (r.ok) {
      r.message = "pulled " + vault::join(r.pull.items) + " (" + std::to_string(r.pull.written.size()) +
                  " file(s) written)";
    }
    std::string err;
    vaultp::project_status(project, *backend, &r.status, &err);
    r.listed = backend->list({}, &r.items, &err);
    return r;
  }, [done = std::move(done)](const JobResult& r) {
    if (r.ok && done) done(r.pull);
  });
}

void VaultPanel::send_to_vault(const std::string& rel) {
  open_upload_ = true;
  up_rel_ = rel;
  up_source_ = 0;
  up_plan_.clear();
  up_plan_error_.clear();
  copy_to(up_name_, sizeof(up_name_), "");
  if (!up_author_[0]) copy_to(up_author_, sizeof(up_author_), username_);
  up_note_[0] = 0;
  up_desc_[0] = 0;
  up_thumb_[0] = 0;
  const bool vr = false;
  up_dims_[0] = host_.dimension == "2d";
  up_dims_[1] = host_.dimension == "3d";
  up_dims_[2] = vr;
  replan_upload();
}

void VaultPanel::replan_upload() {
  up_plan_.clear();
  up_plan_error_.clear();
  std::string rel = up_rel_;
  if (up_source_ == 1 && host_.prefabs) {
    const auto list = host_.prefabs();
    rel = up_prefab_ >= 0 && up_prefab_ < static_cast<int>(list.size()) ? list[up_prefab_] : std::string();
  }
  assetdb::AssetDb* db = host_.db ? host_.db() : nullptr;
  if (rel.empty() || !db) {
    up_plan_error_ = "Pick an asset in the Asset Browser (right-click > Send to Vault) or a prefab.";
    return;
  }
  if (!vaultp::plan_push(project_dir_, *db, rel, &up_plan_, &up_plan_error_)) up_plan_.clear();
}

// ---- drawing ---------------------------------------------------------------------

ux::FriendlyError friendly_vault_error(const std::string& raw, bool remote) {
  ux::FriendlyError f = ux::explain(raw, remote ? ux::ErrorKind::VaultConnection : ux::ErrorKind::Unknown,
                                    remote ? "Vault server" : "Vault (local folder)");
  if (f.kind != ux::ErrorKind::VaultConnection) f.fix += " You can pick another Vault folder in Vault settings.";
  f.fix_label = "Vault settings";
  f.fix_action.clear();  // the panel opens its own settings popup
  return f;
}

void VaultPanel::draw() {
  if (!loaded_ && !job_.valid() && error_.empty()) refresh();
  thumb_tex_.poll_changes(ImGui::GetTime(), 2.0);
  draw_toolbar();
  if (!error_.empty()) {
    if (ux::ErrorBox("vault_error", friendly_vault_error(error_, backend_ == "remote"))) open_settings_ = true;
  } else if (!status_line_.empty()) {
    if (job_.valid()) ImGui::TextColored(theme::Accent(), "%s", status_line_.c_str());
    else ImGui::TextColored(theme::TextMuted(), "%s", status_line_.c_str());
  }
  const float details_w = selected_.empty() ? 0.0f : 330.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  if (ImGui::BeginChild("##vaultgrid", ImVec2(avail - details_w - (details_w > 0 ? 6 : 0), 0), ImGuiChildFlags_Borders)) {
    draw_grid();
  }
  ImGui::EndChild();
  if (!selected_.empty()) {
    ImGui::SameLine(0, 6);
    if (ImGui::BeginChild("##vaultdetails", ImVec2(0, 0), ImGuiChildFlags_Borders)) draw_details();
    ImGui::EndChild();
  }
  draw_settings_popup();
  draw_upload_modal();
  draw_delete_modal();
}

void VaultPanel::draw_toolbar() {
  int b = backend_ == "remote" ? 1 : 0;
  const char* backs[] = {"Local library", "Vault server"};
  ImGui::SetNextItemWidth(132);
  if (ImGui::Combo("##vaultbackend", &b, backs, 2)) {
    backend_ = b == 1 ? "remote" : "local";
    save_settings();
    selected_.clear();
    refresh();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(170);
  ImGui::InputTextWithHint("##vaultsearch", "Search the Vault...", search_, sizeof(search_));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(95);
  ImGui::Combo("##vaultkind", &kind_filter_, kKinds, 6);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  ImGui::Combo("##vaultdim", &dim_filter_, kDims, 4);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80);
  ImGui::InputTextWithHint("##vaulttag", "tag", tag_, sizeof(tag_));
  ImGui::SameLine();
  ImGui::BeginDisabled(job_.valid());
  if (theme::SecondaryButton("Refresh")) refresh();
  ImGui::SameLine();
  if (host_.manager) {
    if (theme::PrimaryButton("Add files...")) {
      // Native picker where there is one; else the dialog takes pasted paths / drops.
      std::vector<std::string> picked;
      if (filepick::available()) {
        picked = filepick::open_files("Add files to the Vault");
        if (!picked.empty()) add_files(picked);
      } else {
        add_files({});
      }
    }
    if (ImGui::IsItemHovered()) ux::SetTooltip("Send loose files straight to the Vault (or drop them on this window)");
  } else if (theme::PrimaryButton("Send to Vault...")) {
    send_to_vault(host_.selected_asset ? host_.selected_asset() : "");
  }
  std::size_t updates = 0;
  for (const auto& s : status_) updates += s.update_available && !s.pinned && s.origin == "pulled";
  if (updates) {
    ImGui::SameLine();
    const std::string label = "Update all (" + std::to_string(updates) + ")";
    if (theme::CopperButton(label.c_str())) {
      auto backend = std::shared_ptr<vault::Backend>(make_backend());
      const std::string project = project_dir_;
      start("Updating", [backend, project] {
        JobResult r;
        std::vector<std::string> done;
        r.ok = vaultp::update_all(project, nullptr, *backend, &done, &r.error);
        r.changed_project = true;
        r.message = done.empty() ? "nothing to update" : "updated " + vault::join(done);
        std::string err;
        vaultp::project_status(project, *backend, &r.status, &err);
        r.listed = backend->list({}, &r.items, &err);
        return r;
      });
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Settings")) open_settings_ = true;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70);
  ImGui::SliderFloat("##vaulttile", &tile_size_, 56.0f, 140.0f, "%.0f px");
}

bool VaultPanel::tile(const vault::Item& it, float size) {
  ImGui::PushID(it.id.c_str());
  ImGui::BeginGroup();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##vtile", ImVec2(size, size + 34));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool sel = selected_ == it.id;
  dl->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size + 34),
                    theme::U32(sel ? theme::AccentMuted() : (hovered ? theme::HeaderBg() : theme::PanelBg())), 5.0f);
  const ImVec2 t0(p0.x + 4, p0.y + 4), t1(p0.x + size - 4, p0.y + size - 4);
  const ImVec4 kc = kind_colour(it.kind);
  bool drew = false;
  const auto th = thumbs_.find(it.thumbnail);
  if (!it.thumbnail.empty() && th != thumbs_.end()) {
    const gfx::TextureInfo& ti = thumb_tex_.get(th->second);
    if (ti.ok && ti.handle && ti.width > 0 && ti.height > 0) {
      const float s = std::min((t1.x - t0.x) / ti.width, (t1.y - t0.y) / ti.height);
      const float w = ti.width * s, h = ti.height * s;
      const ImVec2 c((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f);
      dl->AddRectFilled(t0, t1, theme::U32(theme::Charcoal()), 4.0f);
      dl->AddImage((ImTextureID)(std::uintptr_t)ti.handle, ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2));
      drew = true;
    }
  }
  if (!drew) {
    dl->AddRectFilled(t0, t1, theme::U32(kc, 0.22f), 6.0f);
    dl->AddRect(t0, t1, theme::U32(kc, 0.8f), 6.0f, 0, 1.5f);
    const char* g = kind_glyph(it.kind);
    const float px = size * 0.28f;
    const ImVec2 ts = theme::MeasureText(px, g);
    theme::DrawSizedText(dl, ImVec2((t0.x + t1.x - ts.x) * 0.5f, (t0.y + t1.y - ts.y) * 0.5f), px, theme::U32(kc), g);
  }
  // Kind strip + version chip.
  dl->AddRectFilled(ImVec2(t0.x, t1.y - 3), ImVec2(t1.x, t1.y), theme::U32(kc));
  const std::string ver = "v" + std::to_string(it.latest);
  const ImVec2 vs = ImGui::CalcTextSize(ver.c_str());
  dl->AddRectFilled(ImVec2(t1.x - vs.x - 8, t0.y + 2), ImVec2(t1.x - 2, t0.y + vs.y + 4), theme::U32(theme::Charcoal(), 0.85f), 3.0f);
  dl->AddText(ImVec2(t1.x - vs.x - 5, t0.y + 3), theme::U32(theme::Sand()), ver.c_str());
  // In this claim / update badges.
  if (const vaultp::ItemStatus* s = status_of(it.id)) {
    if (s->update_available) {
      const char* up = s->pinned ? "PINNED" : "UPDATE";
      const ImVec2 us = ImGui::CalcTextSize(up);
      dl->AddRectFilled(ImVec2(t0.x + 2, t1.y - us.y - 8), ImVec2(t0.x + us.x + 10, t1.y - 4),
                        theme::U32(s->pinned ? theme::TextMuted() : theme::Copper()), 3.0f);
      dl->AddText(ImVec2(t0.x + 6, t1.y - us.y - 6), theme::U32(theme::TextOnAccent()), up);
    }
    dl->AddCircleFilled(ImVec2(t0.x + 8, t0.y + 8), 5.0f, theme::U32(s->origin == "pushed" ? theme::Accent() : theme::Success()));
  }
  std::string name = it.name;
  while (name.size() > 3 && ImGui::CalcTextSize(name.c_str()).x > size - 6) name = name.substr(0, name.size() - 3) + "..";
  dl->AddText(ImVec2(p0.x + 3, p0.y + size - 1), theme::U32(sel ? theme::Accent() : theme::Text()), name.c_str());
  std::string sub = it.author.empty() ? it.kind : it.author;
  if (!it.dims.empty()) sub += "  " + vault::join(it.dims, "/");
  while (sub.size() > 3 && ImGui::CalcTextSize(sub.c_str()).x > size - 6) sub = sub.substr(0, sub.size() - 3) + "..";
  dl->AddText(ImVec2(p0.x + 3, p0.y + size + 15), theme::U32(theme::TextMuted()), sub.c_str());
  ImGui::EndGroup();
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload(kVaultItemPayload, it.id.c_str(), it.id.size() + 1);
    ImGui::Text("Pull %s v%d", it.name.c_str(), it.latest);
    ImGui::TextDisabled("Drop on the Asset Browser or the scene");
    ImGui::EndDragDropSource();
  }
  if (hovered) {
    ux::SetTooltip("%s (%s)\n%s%s%s\nv%d  %s", it.name.c_str(), it.kind.c_str(),
                      it.author.empty() ? "" : ("by " + it.author + "  ").c_str(),
                      it.licence.c_str(), it.tags.empty() ? "" : ("\n#" + vault::join(it.tags, " #")).c_str(),
                      it.latest, it.id.c_str());
  }
  ImGui::PopID();
  return clicked;
}

void VaultPanel::draw_grid() {
  vault::Query q;
  q.text = search_;
  q.kind = kKindNames[kind_filter_];
  q.dim = kDimNames[dim_filter_];
  q.tag = tag_;
  for (char& c : q.tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const float size = tile_size_;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per_row = std::max(1, static_cast<int>(avail / (size + 8)));
  int col = 0;
  std::size_t shown = 0;
  for (const vault::Item& it : items_) {
    if (!vault::item_matches(it, q)) continue;
    if (col > 0) ImGui::SameLine(0, 8);
    if (tile(it, size)) select(it.id);
    col = (col + 1) % per_row;
    ++shown;
  }
  if (shown == 0) {
    if (!loaded_) theme::StatusInfo("Opening the library...");
    else if (items_.empty() && host_.manager)
      ux::EmptyState("vault", {{"Add files...", [this] {
                                  if (filepick::available()) {
                                    const std::vector<std::string> picked = filepick::open_files("Add files to the Vault");
                                    if (!picked.empty()) add_files(picked);
                                  } else {
                                    add_files({});
                                  }
                                }},
                               {"Vault settings", [this] { open_settings_ = true; }, "Pick a local folder or a shared Vault server"}},
                     "The Vault keeps assets you share between claims. Add files here, or drop them on this window.");
    else if (items_.empty())
      ux::EmptyState("vault", {{"Send selected asset", [this] { send_to_vault(host_.selected_asset ? host_.selected_asset() : ""); },
                                "Send the asset selected in the Asset Browser"},
                               {"Vault settings", [this] { open_settings_ = true; }, "Pick a local folder or a shared Vault server"}});
    else
      ux::EmptyState("vault_search", {{"Clear filters", [this] {
                                         search_[0] = '\0';
                                         tag_[0] = '\0';
                                         kind_filter_ = dim_filter_ = 0;
                                       }}});
  }
}

void VaultPanel::draw_details() {
  const vault::Item& it = detail_;
  if (it.id.empty()) {
    ImGui::TextDisabled("...");
    return;
  }
  theme::SectionHeader(it.name.c_str(), it.kind.c_str());
  for (const std::string& d : it.dims) {
    std::string up = d;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    theme::DimensionBadge(up.c_str());
    ImGui::SameLine();
  }
  ImGui::NewLine();
  if (!it.author.empty()) ImGui::Text("Author   %s", it.author.c_str());
  ImGui::Text("Licence  %s", it.licence.empty() ? "(not given)" : it.licence.c_str());
  if (!it.tags.empty()) ImGui::TextColored(theme::Sand(), "#%s", vault::join(it.tags, " #").c_str());
  ImGui::TextDisabled("GUID %s", it.id.c_str());
  if (!it.description.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(it.description.c_str());
    ImGui::PopTextWrapPos();
  }
  theme::Divider();
  const vaultp::ItemStatus* st = status_of(it.id);
  if (st) {
    const std::string line = std::string(st->origin == "pushed" ? "Sent from this claim" : "In this claim") +
                             ": v" + std::to_string(st->version) + (st->pinned ? " (pinned)" : "") +
                             (st->modified ? ", changed here" : "");
    if (st->update_available) theme::StatusWarn((line + " - v" + std::to_string(st->latest) + " available").c_str());
    else theme::StatusSuccess(line.c_str());
  }
  // Version picker (newest first).
  ImGui::TextColored(theme::Accent(), "Versions");
  if (ImGui::BeginChild("##vers", ImVec2(0, 96), ImGuiChildFlags_Borders)) {
    if (ImGui::Selectable("Latest", pull_version_ == 0)) pull_version_ = 0;
    for (auto v = it.versions.rbegin(); v != it.versions.rend(); ++v) {
      const std::string label = "v" + std::to_string(v->version) + "  " + v->created.substr(0, 10) + "  " +
                                human_size(v->size) + (v->note.empty() ? "" : "  - " + v->note);
      if (ImGui::Selectable(label.c_str(), pull_version_ == v->version)) pull_version_ = v->version;
    }
  }
  ImGui::EndChild();
  if (const vault::Version* v = it.version(pull_version_)) {
    if (!v->files.empty()) ImGui::TextDisabled("%zu file(s), %s", v->files.size(), human_size(v->size).c_str());
    if (!v->deps.empty()) {
      std::string deps;
      for (const vault::Dep& d : v->deps) {
        std::string n = d.item.substr(0, 8);
        for (const vault::Item& o : items_) {
          if (o.id == d.item) n = o.name;
        }
        deps += (deps.empty() ? "" : ", ") + n + " v" + std::to_string(d.version);
      }
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("Brings along: %s", deps.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  if (host_.manager) {
    draw_manager_actions(it);
    return;
  }
  ImGui::BeginDisabled(job_.valid() || project_dir_.empty());
  const int target = pull_version_ ? pull_version_ : it.latest;
  std::string label;
  if (st && st->version == target) label = "Pull again (v" + std::to_string(target) + ")";
  else if (st) label = "Update to v" + std::to_string(target);
  else label = "Pull v" + std::to_string(target) + " into this claim";
  if (theme::PrimaryButton(label.c_str())) pull_then(it.id, {});
  if (st) {
    ImGui::SameLine();
    bool pinned = st->pinned;
    if (ImGui::Checkbox("Pin", &pinned)) {
      std::string err;
      if (vaultp::set_pinned(project_dir_, it.id, pinned, &err)) {
        for (auto& s : status_) {
          if (s.item == it.id) s.pinned = pinned;
        }
      } else {
        error_ = err;
      }
    }
    if (ImGui::IsItemHovered()) ux::SetTooltip("Pinned items keep their version when you Update all");
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled("Or drag the tile into the Asset Browser / scene.");
}

void VaultPanel::draw_settings_popup() {
  if (open_settings_) {
    ImGui::OpenPopup("Vault settings");
    open_settings_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Vault settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  theme::SectionHeader("Library", "where items live");
  int b = backend_ == "remote" ? 1 : 0;
  ImGui::RadioButton("Local folder (every claim on this PC)", &b, 0);
  ImGui::RadioButton("Vault server (ts_vault)", &b, 1);
  backend_ = b == 1 ? "remote" : "local";
  ImGui::SetNextItemWidth(380);
  ImGui::InputTextWithHint("Local folder", vault::default_local_library().c_str(), local_dir_, sizeof(local_dir_));
  ImGui::SetNextItemWidth(380);
  ImGui::InputTextWithHint("Server URL", "https://vault.example.com or http://192.168.1.20:8740", url_, sizeof(url_));
  ImGui::SetNextItemWidth(380);
  ImGui::InputTextWithHint("API key", "tsv_... (from ts_vault --add-key)", key_, sizeof(key_), ImGuiInputTextFlags_Password);
  ImGui::TextDisabled("The key is kept in TombStoneConfig/settings.json and never logged.");
  if (theme::PrimaryButton("Save")) {
    save_settings();
    selected_.clear();
    ImGui::CloseCurrentPopup();
    refresh();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(job_.valid());
  if (theme::SecondaryButton("Test connection")) {
    auto backend = std::shared_ptr<vault::Backend>(make_backend());
    start("Testing", [backend] {
      JobResult r;
      std::string info;
      r.ok = backend->ping(&info, &r.error);
      r.message = info;
      return r;
    });
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    load_settings();
    ImGui::CloseCurrentPopup();
  }
  if (!error_.empty()) theme::StatusError(error_.c_str());
  else if (!status_line_.empty()) ImGui::TextColored(theme::TextMuted(), "%s", status_line_.c_str());
  ImGui::EndPopup();
}

void VaultPanel::draw_upload_modal() {
  if (open_upload_) {
    ImGui::OpenPopup("Send to Vault");
    open_upload_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Send to Vault", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  theme::SectionHeader(up_files_mode_ ? "Add files to the Vault" : "Send to Vault", backend_ == "remote" ? url_ : "local library");
  if (up_files_mode_) draw_add_files_body();
  const std::vector<std::string> prefabs =
      !up_files_mode_ && host_.prefabs ? host_.prefabs() : std::vector<std::string>{};
  bool replan = false;
  if (!up_files_mode_) {
  replan |= ImGui::RadioButton(("Asset: " + (up_rel_.empty() ? std::string("(select one in the Asset Browser)") : up_rel_)).c_str(), &up_source_, 0);
  if (!prefabs.empty()) {
    replan |= ImGui::RadioButton("Prefab", &up_source_, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##upprefab", prefabs[std::min<std::size_t>(up_prefab_, prefabs.size() - 1)].c_str())) {
      for (int i = 0; i < static_cast<int>(prefabs.size()); ++i) {
        if (ImGui::Selectable(prefabs[i].c_str(), i == up_prefab_)) {
          up_prefab_ = i;
          up_source_ = 1;
          replan = true;
        }
      }
      ImGui::EndCombo();
    }
  }
  }
  if (replan) replan_upload();
  std::string name_hint = up_plan_.empty() ? "" : fs::path(up_plan_.back().main).stem().string();
  if (up_files_mode_) {
    name_hint = up_files_.size() == 1 ? fs::u8path(up_files_[0]).stem().string() : "file names (one item per asset)";
  }
  ImGui::SetNextItemWidth(320);
  ImGui::BeginDisabled(up_files_mode_ && up_files_.size() > 1);
  ImGui::InputTextWithHint("Name", name_hint.c_str(), up_name_, sizeof(up_name_));
  ImGui::EndDisabled();
  ImGui::SetNextItemWidth(320);
  ImGui::InputText("Author", up_author_, sizeof(up_author_));
  ImGui::SetNextItemWidth(220);
  ImGui::Combo("Licence", &up_licence_, kLicences, kLicenceCount);
  if (up_licence_ == kLicenceCount - 1) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputTextWithHint("##lic", "licence", up_licence_custom_, sizeof(up_licence_custom_));
  }
  ImGui::SetNextItemWidth(320);
  ImGui::InputTextWithHint("Tags", "comma separated: western, props, wood", up_tags_, sizeof(up_tags_));
  ImGui::Checkbox("2D", &up_dims_[0]);
  ImGui::SameLine();
  ImGui::Checkbox("3D", &up_dims_[1]);
  ImGui::SameLine();
  ImGui::Checkbox("VR", &up_dims_[2]);
  ImGui::InputTextMultiline("Description", up_desc_, sizeof(up_desc_), ImVec2(320, 54));
  ImGui::SetNextItemWidth(320);
  ImGui::InputTextWithHint("Version note", "what changed", up_note_, sizeof(up_note_));
  ImGui::SetNextItemWidth(320);
  ImGui::InputTextWithHint("Thumbnail", "optional .png/.jpg (else made from the asset)", up_thumb_, sizeof(up_thumb_));
  theme::Divider();
  if (up_files_mode_) {
    if (up_files_.empty()) theme::StatusInfo("Add a file above, or drop files from Explorer on this window.");
    else ImGui::TextColored(theme::Accent(), "%zu file(s); a model brings its textures along as dependencies", up_files_.size());
  } else if (!up_plan_error_.empty()) {
    theme::StatusWarn(up_plan_error_.c_str());
  } else if (!up_plan_.empty()) {
    std::uint64_t bytes = 0;
    std::size_t files = 0;
    for (const auto& p : up_plan_) {
      for (const auto& f : p.files) bytes += f.size;
      files += p.files.size();
    }
    ImGui::TextColored(theme::Accent(), "%zu item(s), %zu file(s), %s", up_plan_.size(), files, human_size(bytes).c_str());
    for (const auto& p : up_plan_) {
      const bool main = &p == &up_plan_.back();
      ImGui::BulletText("%s%s  (%s, %zu file(s)%s)", main ? "" : "dependency: ", p.main.c_str(), p.kind.c_str(),
                        p.files.size(), p.item.empty() ? "" : (", GUID " + p.item.substr(0, 8)).c_str());
    }
  }
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520);
  ImGui::TextColored(theme::Warning(), "%s", vault::kVaultRightsNote);
  ImGui::PopTextWrapPos();
  ImGui::BeginDisabled((up_files_mode_ ? up_files_.empty() : up_plan_.empty()) || job_.valid());
  if (theme::PrimaryButton("Send")) {
    vaultp::PushOptions opt;
    opt.name = up_name_;
    opt.author = up_author_;
    opt.licence = up_licence_ == kLicenceCount - 1 ? std::string(up_licence_custom_) : std::string(kLicences[up_licence_]);
    opt.description = up_desc_;
    opt.note = up_note_;
    opt.tags = vault::split_tags(up_tags_);
    if (up_dims_[0]) opt.dims.push_back("2d");
    if (up_dims_[1]) opt.dims.push_back("3d");
    if (up_dims_[2]) opt.dims.push_back("vr");
    opt.thumbnail_png = up_thumb_;
    if (up_files_mode_) {
      start_add_files(opt);
      ImGui::CloseCurrentPopup();
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Button("Cancel");
      ImGui::EndPopup();
      return;
    }
    const std::string rel = up_plan_.back().main;
    auto backend = std::shared_ptr<vault::Backend>(make_backend());
    const std::string project = project_dir_;
    start("Sending to the Vault", [backend, project, rel, opt] {
      JobResult r;
      vaultp::PushResult pr;
      r.ok = vaultp::push_asset(project, nullptr, rel, *backend, opt, &pr, &r.error);
      r.changed_project = r.ok;
      if (r.ok) r.message = "sent " + vault::join(pr.pushed) + " (" + human_size(pr.bytes) + " uploaded)";
      std::string err;
      vaultp::project_status(project, *backend, &r.status, &err);
      r.listed = backend->list({}, &r.items, &err);
      return r;
    });
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ---- Vault manager (Project Manager) ------------------------------------------------

void VaultPanel::add_files(const std::vector<std::string>& paths) {
  if (!up_files_mode_ || !ImGui::IsPopupOpen("Send to Vault")) {
    up_files_mode_ = true;
    up_files_.clear();
    up_plan_.clear();
    up_plan_error_.clear();
    up_name_[0] = 0;
    if (!up_author_[0]) copy_to(up_author_, sizeof(up_author_), username_);
    up_note_[0] = 0;
    up_desc_[0] = 0;
    up_thumb_[0] = 0;
    up_path_[0] = 0;
    up_dims_[0] = host_.dimension == "2d";
    up_dims_[1] = host_.dimension == "3d";
    up_dims_[2] = false;
  }
  for (const std::string& p : paths) {
    if (!p.empty() && std::find(up_files_.begin(), up_files_.end(), p) == up_files_.end()) up_files_.push_back(p);
  }
  open_upload_ = true;
}

void VaultPanel::draw_add_files_body() {
  ImGui::TextColored(theme::TextMuted(), "Files from anywhere on this PC; no claim needed.");
  if (ImGui::BeginChild("##upfiles", ImVec2(520, 92), ImGuiChildFlags_Borders)) {
    int remove = -1;
    for (int i = 0; i < static_cast<int>(up_files_.size()); ++i) {
      ImGui::PushID(i);
      if (ImGui::SmallButton("x")) remove = i;
      ImGui::SameLine();
      const std::string kind = vaultp::vault_kind(fs::u8path(up_files_[i]).filename().string());
      ImGui::TextColored(kind.empty() ? theme::TextMuted() : theme::Text(), "%s%s", up_files_[i].c_str(),
                         kind.empty() ? "  (not a Vault asset, or a model's companion file)" : "");
      ImGui::PopID();
    }
    if (remove >= 0) up_files_.erase(up_files_.begin() + remove);
    if (up_files_.empty()) ImGui::TextDisabled("Drop files from Explorer here, or add a path below.");
  }
  ImGui::EndChild();
  ImGui::SetNextItemWidth(380);
  const bool enter = ImGui::InputTextWithHint("##uppath", "C:\\path\\to\\file.png", up_path_, sizeof(up_path_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if (theme::SecondaryButton("Add path") || enter) {
    std::string p = up_path_;
    while (!p.empty() && (p.back() == '"' || p.back() == ' ')) p.pop_back();
    while (!p.empty() && (p.front() == '"' || p.front() == ' ')) p.erase(p.begin());
    std::error_code ec;
    if (!p.empty() && fs::exists(fs::u8path(p), ec)) {
      if (std::find(up_files_.begin(), up_files_.end(), p) == up_files_.end()) up_files_.push_back(p);
      up_path_[0] = 0;
    } else if (!p.empty()) {
      up_plan_error_ = "No such file: " + p;
    }
  }
  if (filepick::available()) {
    ImGui::SameLine();
    if (theme::SecondaryButton("Browse...")) {
      for (const std::string& p : filepick::open_files("Add files to the Vault")) {
        if (std::find(up_files_.begin(), up_files_.end(), p) == up_files_.end()) up_files_.push_back(p);
      }
    }
  }
  if (!up_plan_error_.empty()) theme::StatusWarn(up_plan_error_.c_str());
  theme::Divider();
}

void VaultPanel::start_add_files(const vaultp::PushOptions& opt) {
  auto backend = std::shared_ptr<vault::Backend>(make_backend());
  const std::vector<std::string> files = up_files_;
  std::string scratch = host_.scratch_dir;
  if (scratch.empty()) scratch = (fs::path(SettingsStore::default_config_dir()) / "vault_add").string();
  const std::string thumbs = thumb_dir_;
  start("Adding to the Vault", [backend, files, opt, scratch, thumbs] {
    JobResult r;
    vaultp::AddFilesResult ar;
    r.ok = vaultp::add_files(files, *backend, opt, scratch, &ar, &r.error);
    if (r.ok) {
      r.message = "added " + vault::join(ar.pushed) + " (" + human_size(ar.bytes) + " uploaded)";
      if (!ar.skipped.empty()) r.message += "; skipped " + vault::join(ar.skipped);
    }
    std::string err;
    r.listed = backend->list({}, &r.items, &err);
    for (const vault::Item& it : r.items) {
      if (it.thumbnail.empty()) continue;
      const std::string p = vaultp::cached_thumbnail(*backend, it.thumbnail, thumbs);
      if (!p.empty()) r.thumbs[it.thumbnail] = p;
    }
    if (r.ok && !ar.items.empty()) r.has_detail = backend->get(ar.items.front(), &r.detail, &err);
    return r;
  }, [this](const JobResult& r) {
    if (r.ok && r.has_detail) {
      selected_ = r.detail.id;
      pull_version_ = 0;
      editing_ = false;
    }
    if (r.ok) up_files_.clear();
  });
}

void VaultPanel::pull_into(const std::string& claim_dir, const std::string& item, int version) {
  auto backend = std::shared_ptr<vault::Backend>(make_backend());
  std::string claim_name = fs::u8path(claim_dir).filename().string();
  if (host_.claims) {
    for (const auto& c : host_.claims()) {
      if (c.path == claim_dir) claim_name = c.name;
    }
  }
  start("Pulling into " + claim_name, [backend, claim_dir, claim_name, item, version] {
    JobResult r;
    vaultp::PullOptions opt;
    opt.version = version;
    r.ok = vaultp::pull_item(claim_dir, nullptr, *backend, item, opt, &r.pull, &r.error);
    r.pulled = r.ok;
    if (r.ok) {
      r.message = "pulled " + vault::join(r.pull.items) + " into " + claim_name + " (" +
                  std::to_string(r.pull.written.size()) + " file(s) written, " + r.pull.main + ")";
    }
    return r;
  });
}

void VaultPanel::begin_edit() {
  const vault::Item& it = detail_;
  if (it.id.empty()) return;
  editing_ = true;
  copy_to(ed_name_, sizeof(ed_name_), it.name);
  copy_to(ed_author_, sizeof(ed_author_), it.author);
  copy_to(ed_tags_, sizeof(ed_tags_), vault::join(it.tags));
  copy_to(ed_desc_, sizeof(ed_desc_), it.description);
  ed_licence_ = kLicenceCount - 1;
  ed_licence_custom_[0] = 0;
  for (int i = 0; i < kLicenceCount - 1; ++i) {
    if (it.licence == kLicences[i]) ed_licence_ = i;
  }
  if (ed_licence_ == kLicenceCount - 1) copy_to(ed_licence_custom_, sizeof(ed_licence_custom_), it.licence);
  for (int i = 0; i < 3; ++i) {
    ed_dims_[i] = std::find(it.dims.begin(), it.dims.end(), kDimNames[i + 1]) != it.dims.end();
  }
}

void VaultPanel::draw_edit_form(const vault::Item& it) {
  theme::SectionHeader("Edit details", "name, tags, description, licence");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##edname", "Name", ed_name_, sizeof(ed_name_));
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##edauthor", "Author", ed_author_, sizeof(ed_author_));
  ImGui::SetNextItemWidth(170);
  ImGui::Combo("##edlic", &ed_licence_, kLicences, kLicenceCount);
  if (ed_licence_ == kLicenceCount - 1) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##edlicc", "licence", ed_licence_custom_, sizeof(ed_licence_custom_));
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##edtags", "tags, comma separated", ed_tags_, sizeof(ed_tags_));
  ImGui::Checkbox("2D", &ed_dims_[0]);
  ImGui::SameLine();
  ImGui::Checkbox("3D", &ed_dims_[1]);
  ImGui::SameLine();
  ImGui::Checkbox("VR", &ed_dims_[2]);
  ImGui::InputTextMultiline("##eddesc", ed_desc_, sizeof(ed_desc_), ImVec2(-1, 60));
  ImGui::BeginDisabled(job_.valid() || !ed_name_[0]);
  if (theme::PrimaryButton("Save details")) {
    vault::Item meta = it;
    meta.versions.clear();
    meta.name = ed_name_;
    meta.author = ed_author_;
    meta.licence = ed_licence_ == kLicenceCount - 1 ? std::string(ed_licence_custom_) : std::string(kLicences[ed_licence_]);
    meta.description = ed_desc_;
    meta.tags = vault::split_tags(ed_tags_);
    meta.dims.clear();
    for (int i = 0; i < 3; ++i) {
      if (ed_dims_[i]) meta.dims.push_back(kDimNames[i + 1]);
    }
    auto backend = std::shared_ptr<vault::Backend>(make_backend());
    start("Saving details", [backend, meta] {
      JobResult r;
      r.ok = backend->update_item(meta, &r.detail, &r.error);
      r.has_detail = r.ok;
      if (r.ok) r.message = "saved " + r.detail.name;
      std::string err;
      r.listed = backend->list({}, &r.items, &err);
      return r;
    }, [this](const JobResult& r) {
      if (r.ok) editing_ = false;
    });
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (theme::SecondaryButton("Cancel")) editing_ = false;
}

void VaultPanel::draw_manager_actions(const vault::Item& it) {
  if (editing_) {
    draw_edit_form(it);
    return;
  }
  ImGui::BeginDisabled(job_.valid());
  // Pull into a claim.
  std::vector<VaultHost::Claim> claims = host_.claims ? host_.claims() : std::vector<VaultHost::Claim>{};
  if (claim_pick_ >= static_cast<int>(claims.size())) claim_pick_ = -1;
  const int target = pull_version_ ? pull_version_ : it.latest;
  ImGui::TextColored(theme::Accent(), "Pull into claim");
  ImGui::SetNextItemWidth(-1);
  const std::string preview = claim_pick_ < 0 ? std::string(claims.empty() ? "(no claims yet)" : "Pick a claim...")
                                              : claims[claim_pick_].name + "  (" + claims[claim_pick_].kind + ")";
  if (ImGui::BeginCombo("##claimpick", preview.c_str())) {
    for (int i = 0; i < static_cast<int>(claims.size()); ++i) {
      const std::string label = claims[i].name + "  (" + claims[i].kind + ")##" + claims[i].path;
      if (ImGui::Selectable(label.c_str(), i == claim_pick_)) claim_pick_ = i;
      if (ImGui::IsItemHovered()) ux::SetTooltip("%s", claims[i].path.c_str());
    }
    ImGui::EndCombo();
  }
  ImGui::BeginDisabled(claim_pick_ < 0);
  const std::string pull_label = "Pull v" + std::to_string(target) + " into claim";
  if (theme::PrimaryButton(pull_label.c_str())) pull_into(claims[claim_pick_].path, it.id, pull_version_);
  ImGui::EndDisabled();
  theme::Divider();
  if (theme::SecondaryButton("Edit details")) begin_edit();
  ImGui::SameLine();
  if (pull_version_ != 0 && it.versions.size() > 1) {
    const std::string dv = "Delete v" + std::to_string(pull_version_);
    if (theme::DangerButton(dv.c_str())) {
      del_item_ = it.id;
      del_name_ = it.name;
      del_version_ = pull_version_;
      open_delete_ = true;
    }
    ImGui::SameLine();
  }
  if (theme::DangerButton("Delete item")) {
    del_item_ = it.id;
    del_name_ = it.name;
    del_version_ = 0;
    open_delete_ = true;
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled(it.versions.size() > 1 ? "Pick a version above to delete just that one."
                                             : "Pulled copies in claims are not touched.");
}

void VaultPanel::draw_delete_modal() {
  if (open_delete_) {
    ImGui::OpenPopup("Delete from the Vault?");
    open_delete_ = false;
  }
  if (!ImGui::BeginPopupModal("Delete from the Vault?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  if (del_version_ == 0) ImGui::Text("Delete \"%s\" and every version from the Vault?", del_name_.c_str());
  else ImGui::Text("Delete v%d of \"%s\" from the Vault?", del_version_, del_name_.c_str());
  theme::StatusWarn("Claims that already pulled it keep their copies. No coming back.");
  ImGui::Spacing();
  if (theme::DangerButton("Delete", ImVec2(120, 0))) {
    auto backend = std::shared_ptr<vault::Backend>(make_backend());
    const std::string id = del_item_, name = del_name_;
    const int ver = del_version_;
    start("Deleting", [backend, id, name, ver] {
      JobResult r;
      r.ok = ver == 0 ? backend->remove_item(id, &r.error) : backend->remove_version(id, ver, &r.error);
      if (r.ok) r.message = ver == 0 ? "deleted " + name : "deleted " + name + " v" + std::to_string(ver);
      std::string err;
      if (r.ok && ver != 0) r.has_detail = backend->get(id, &r.detail, &err);
      r.listed = backend->list({}, &r.items, &err);
      return r;
    }, [this, id, ver](const JobResult& r) {
      if (!r.ok) return;
      pull_version_ = 0;
      if (ver == 0 && selected_ == id) {
        selected_.clear();
        detail_ = vault::Item{};
      }
    });
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Keep it", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
