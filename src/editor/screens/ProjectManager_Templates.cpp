// Project Manager: the template picker ("Break Ground"), Save as Template
// and Claim Settings (the VR switch on 3D claims).

#include "editor/screens/ProjectManagerScreen.h"

#include "editor/projects/ProjectStore.h"
#include "editor/templates/TemplateStore.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

constexpr const char* kPickerTitle = "Break Ground###TemplatePicker";

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool template_matches(const ProjectTemplate& t, int filter, const char* search) {
  // Templates with an alt dimension (Truly Blank) show under 2D and 3D.
  const bool any_2d = t.kind == ProjectKind::TwoD || (t.has_alt && t.alt_kind == ProjectKind::TwoD);
  const bool any_3d = t.kind != ProjectKind::TwoD || (t.has_alt && t.alt_kind != ProjectKind::TwoD);
  switch (filter) {
    case 1: if (!any_2d) return false; break;
    case 2: if (!any_3d) return false; break;
    case 3: if (!t.vr) return false; break;
    case 4: if (!t.user) return false; break;
    default: break;
  }
  if (!search || !*search) return true;
  const std::string q = lower(search);
  if (lower(t.name).find(q) != std::string::npos) return true;
  if (lower(t.description).find(q) != std::string::npos) return true;
  for (const std::string& tag : t.tags) {
    if (lower(tag).find(q) != std::string::npos) return true;
  }
  for (const std::string& p : t.plugins) {
    if (lower(p).find(q) != std::string::npos) return true;
  }
  return false;
}

ImU32 mix(const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f) {
  return theme::U32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                           a.z + (b.z - a.z) * t, alpha));
}

std::string plugin_short(const std::string& id) {
  if (id == "ts.posse") return "Posse";
  if (id == "ts.frontier") return "Frontier";
  if (id == "ts.shipyard") return "Shipyard";
  if (id == "ts.tumbleweed") return "Tumbleweed";
  if (id == "sample.lantern") return "Lantern";
  return id;
}

}  // namespace

std::string ProjectManagerScreen::default_claim_name(const ProjectTemplate& t) const {
  if (t.blank() || t.empty_group()) {
    const bool two_d = t.has_alt && picker_dim_ >= 0 ? picker_dim_ == 0 : t.kind == ProjectKind::TwoD;
    if (two_d) return "New 2D Claim";
    return t.vr ? "New VR Claim" : "New 3D Claim";
  }
  return "My " + t.name;
}

void ProjectManagerScreen::pick_template(const std::string& id) {
  const ProjectTemplate* t = templates_ ? templates_->find(id) : nullptr;
  if (!t) return;
  picked_template_id_ = id;
  picker_vr_ = t->vr;
  picker_dim_ = -1;
  std::memset(picker_name_buf_, 0, sizeof(picker_name_buf_));
  std::snprintf(picker_name_buf_, sizeof(picker_name_buf_), "%s", default_claim_name(*t).c_str());
}

void ProjectManagerScreen::open_template_picker(std::string preselect_id) {
  picker_open_ = true;
  picker_popup_pending_ = true;
  picker_filter_ = 0;
  std::memset(picker_search_, 0, sizeof(picker_search_));
  if (!preselect_id.empty()) {
    picked_template_id_ = std::move(preselect_id);
  } else if ((picked_template_id_.empty() || !templates_ || !templates_->find(picked_template_id_)) &&
             templates_ && !templates_->templates().empty()) {
    picked_template_id_ = templates_->templates().front().id;  // Empty 2D
  }
  const ProjectTemplate* t = templates_ ? templates_->find(picked_template_id_) : nullptr;
  if (t) {
    pick_template(t->id);
  } else {
    std::memset(picker_name_buf_, 0, sizeof(picker_name_buf_));
    std::snprintf(picker_name_buf_, sizeof(picker_name_buf_), "%s", "New Claim");
  }
}

void ProjectManagerScreen::request_new_from_template(std::string name, std::string template_id,
                                                     int vr, int dimension) {
  pending_new_project_name_ = std::move(name);
  pending_template_id_ = std::move(template_id);
  pending_template_vr_ = vr;
  pending_template_dim_ = dimension;
  template_create_requested_ = true;
  std::cout << "[ProjectManager] New claim from template \"" << pending_template_id_
            << "\": \"" << pending_new_project_name_ << "\"\n";
}

void ProjectManagerScreen::clear_template_create_request() {
  template_create_requested_ = false;
  pending_template_id_.clear();
  pending_template_vr_ = -1;
  pending_template_dim_ = -1;
}

void ProjectManagerScreen::request_save_as_template(std::string name, std::string description) {
  if (!highlighted_index_.has_value()) {
    error_message_ = "Pick a claim first, then Save as Template.";
    return;
  }
  pending_template_name_ = std::move(name);
  pending_template_desc_ = std::move(description);
  pending_delete_index_.reset();
  save_template_requested_ = true;
}

void ProjectManagerScreen::request_set_vr(bool on) {
  if (!highlighted_index_.has_value()) {
    error_message_ = "Pick a claim first.";
    return;
  }
  pending_vr_ = on;
  set_vr_requested_ = true;
}

void ProjectManagerScreen::draw_template_thumb(const ProjectTemplate& t, const ImVec2& p0,
                                               const ImVec2& p1) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = p1.x - p0.x;
  const float h = p1.y - p0.y;
  if (!t.thumbnail.empty()) {
    const gfx::TextureInfo& tex = thumbs_.get(t.thumbnail);
    if (tex.ok && tex.handle != 0) {
      // Cover-fit the picture into the slot.
      const float ta = static_cast<float>(tex.width) / std::max(1, tex.height);
      const float sa = w / h;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (ta > sa) {
        const float cut = (1.0f - sa / ta) * 0.5f;
        uv0.x = cut;
        uv1.x = 1.0f - cut;
      } else {
        const float cut = (1.0f - ta / sa) * 0.5f;
        uv0.y = cut;
        uv1.y = 1.0f - cut;
      }
      dl->AddImageRounded(static_cast<ImTextureID>(tex.handle), p0, p1, uv0, uv1,
                          IM_COL32_WHITE, theme::metrics::kRadiusCard,
                          ImDrawFlags_RoundCornersTop);
      return;
    }
  }
  // Painted fallback: dusk sky, the ground, and a silhouette per kind.
  const ImVec4 sky_top(0.16f, 0.12f, 0.20f, 1.0f);
  const ImVec4 sky_low = theme::Copper();
  dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + h * 0.72f), mix(sky_top, sky_top, 0),
                              mix(sky_top, sky_top, 0), mix(sky_top, sky_low, 0.75f),
                              mix(sky_top, sky_low, 0.75f));
  const float gy = p0.y + h * 0.72f;
  dl->AddRectFilled(ImVec2(p0.x, gy), p1, theme::U32(theme::Charcoal()));
  dl->AddCircleFilled(ImVec2(p0.x + w * 0.78f, gy - h * 0.18f), h * 0.13f,
                      theme::U32(theme::Accent(), 0.85f));
  const ImU32 ink = theme::U32(theme::Charcoal());
  if (t.kind == ProjectKind::TwoD) {
    // Tiles + a rider block + a false-front store.
    for (int i = 0; i < 9; ++i) {
      const float x = p0.x + i * w / 9.0f;
      dl->AddRect(ImVec2(x, gy), ImVec2(x + w / 9.0f, gy + h * 0.12f),
                  theme::U32(theme::Border()), 0.0f, 0, 1.0f);
    }
    dl->AddRectFilled(ImVec2(p0.x + w * 0.12f, gy - h * 0.40f),
                      ImVec2(p0.x + w * 0.40f, gy), ink);
    dl->AddRectFilled(ImVec2(p0.x + w * 0.09f, gy - h * 0.46f),
                      ImVec2(p0.x + w * 0.43f, gy - h * 0.36f), ink);
    dl->AddRectFilled(ImVec2(p0.x + w * 0.55f, gy - h * 0.16f),
                      ImVec2(p0.x + w * 0.60f, gy), theme::U32(theme::Sand()));
  } else {
    // Perspective street + buildings in depth.
    const ImVec2 vp(p0.x + w * 0.5f, gy);
    for (int i = -4; i <= 4; ++i) {
      dl->AddLine(vp, ImVec2(p0.x + w * 0.5f + i * w * 0.18f, p1.y),
                  theme::U32(theme::Border(), 0.8f), 1.0f);
    }
    dl->AddQuadFilled(ImVec2(p0.x + w * 0.05f, p1.y), ImVec2(p0.x + w * 0.05f, gy - h * 0.45f),
                      ImVec2(p0.x + w * 0.36f, gy - h * 0.22f), ImVec2(p0.x + w * 0.36f, gy), ink);
    dl->AddQuadFilled(ImVec2(p0.x + w * 0.95f, p1.y), ImVec2(p0.x + w * 0.95f, gy - h * 0.40f),
                      ImVec2(p0.x + w * 0.64f, gy - h * 0.20f), ImVec2(p0.x + w * 0.64f, gy), ink);
    if (t.vr) {
      // Goggles.
      const ImVec2 c(p0.x + w * 0.5f, p0.y + h * 0.30f);
      const float r = h * 0.11f;
      dl->AddRectFilled(ImVec2(c.x - r * 2.4f, c.y - r), ImVec2(c.x + r * 2.4f, c.y + r),
                        theme::U32(theme::Sand()), r);
      dl->AddCircleFilled(ImVec2(c.x - r * 1.1f, c.y), r * 0.7f, ink);
      dl->AddCircleFilled(ImVec2(c.x + r * 1.1f, c.y), r * 0.7f, ink);
    }
  }
  if (t.blank()) {
    theme::DrawSizedText(dl, ImVec2(p0.x + 10.0f, p0.y + 8.0f), 26.0f,
                         theme::U32(theme::Sand()),
                         t.kind == ProjectKind::TwoD ? "2D" : "3D");
  }
}

void ProjectManagerScreen::draw_template_card(const ProjectTemplate& t, float w, float h) {
  const bool on = picked_template_id_ == t.id;
  ImGui::PushID(t.id.c_str());
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("tpl", ImVec2(w, h))) {
    if (picked_template_id_ != t.id) pick_template(t.id);
  }
  const bool hovered = ImGui::IsItemHovered();
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    const bool two_d = t.has_alt && picker_dim_ >= 0 ? picker_dim_ == 0 : t.kind == ProjectKind::TwoD;
    request_new_from_template(picker_name_buf_, t.id, two_d ? 0 : picker_vr_,
                              t.has_alt ? picker_dim_ : -1);
    picker_open_ = false;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p1(p0.x + w, p0.y + h);
  dl->AddRectFilled(p0, p1, mix(theme::PanelBg(), theme::Accent(), on ? 0.14f : hovered ? 0.06f : 0.0f),
                    theme::metrics::kRadiusCard);
  const float th = w * 0.52f;
  dl->PushClipRect(p0, ImVec2(p1.x, p0.y + th), true);
  draw_template_thumb(t, p0, ImVec2(p1.x, p0.y + th));
  dl->PopClipRect();
  dl->AddRect(p0, p1, theme::U32(on ? theme::Accent() : theme::Border()),
              theme::metrics::kRadiusCard, 0, on ? 2.0f : 1.0f);

  // Badges on the picture: dimension, VR, "Mine".
  float bx = p1.x - 8.0f;
  auto badge = [&](const char* text, const ImVec4& col) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 b0(bx - ts.x - 10.0f, p0.y + 7.0f);
    dl->AddRectFilled(b0, ImVec2(bx, b0.y + ts.y + 4.0f), theme::U32(theme::Charcoal(), 0.85f), 4.0f);
    dl->AddRect(b0, ImVec2(bx, b0.y + ts.y + 4.0f), theme::U32(col), 4.0f);
    dl->AddText(ImVec2(b0.x + 5.0f, b0.y + 2.0f), theme::U32(col), text);
    bx = b0.x - 4.0f;
  };
  if (t.vr) badge("VR", theme::Success());
  badge(t.has_alt ? "2D / 3D" : t.kind == ProjectKind::TwoD ? "2D" : "3D", theme::Accent());
  if (t.user) badge("Mine", theme::Sand());
  if (t.empty_group()) badge("Empty", theme::Sand());

  // Name, description (two lines), plugin chips.
  float y = p0.y + th + 6.0f;
  dl->AddText(ImVec2(p0.x + 10.0f, y), theme::U32(on ? theme::Accent() : theme::Text()), t.name.c_str());
  y += ImGui::GetTextLineHeight() + 3.0f;
  const ImVec4 clip(p0.x + 10.0f, y, p1.x - 8.0f, y + ImGui::GetTextLineHeight() * 2.0f + 2.0f);
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.92f, ImVec2(p0.x + 10.0f, y),
              theme::U32(theme::TextMuted()), t.description.c_str(), nullptr, w - 20.0f, &clip);
  y = p1.y - ImGui::GetTextLineHeight() - 8.0f;
  float cx = p0.x + 10.0f;
  if (t.plugins.empty()) {
    dl->AddText(ImVec2(cx, y), theme::U32(theme::TextMuted(), 0.7f),
                t.plugins_set ? "no plugins" : "default plugins");
  }
  for (const std::string& id : t.plugins) {
    const std::string label = plugin_short(id);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    if (cx + ts.x + 10.0f > p1.x - 8.0f) break;
    dl->AddRectFilled(ImVec2(cx, y - 2.0f), ImVec2(cx + ts.x + 10.0f, y + ts.y + 2.0f),
                      theme::U32(theme::Copper(), 0.28f), 4.0f);
    dl->AddText(ImVec2(cx + 5.0f, y), theme::U32(theme::Sand()), label.c_str());
    cx += ts.x + 16.0f;
  }
  if (hovered) {
    ImGui::SetTooltip("%s\n\n%s%s", t.name.c_str(), t.description.c_str(),
                      t.user ? "\n\n(your template)" : "");
  }
  ImGui::PopID();
}

void ProjectManagerScreen::draw_template_group(const char* title, const char* caption,
                                               const std::string& group) {
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted(title);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", caption);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
                theme::U32(theme::AccentMuted()), 1.0f);
    ImGui::Dummy(ImVec2(0, 4));
  }
  const float avail = ImGui::GetContentRegionAvail().x;
  const float gap = 12.0f;
  const int cols = std::max(1, static_cast<int>((avail + gap) / (220.0f + gap)));
  const float cw = (avail - gap * (cols - 1)) / cols;
  const float chh = cw * 0.52f + 92.0f;
  int col = 0;
  ImGui::PushID(group.c_str());
  for (const ProjectTemplate& t : templates_->templates()) {
    if (t.group != group || !template_matches(t, picker_filter_, picker_search_)) continue;
    if (col > 0) ImGui::SameLine(0.0f, gap);
    draw_template_card(t, cw, chh);
    if (++col == cols) {
      col = 0;
      ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    }
  }
  if (col != 0) ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
  ImGui::PopID();
}

void ProjectManagerScreen::draw_template_picker() {
  if (picker_popup_pending_) {
    ImGui::OpenPopup(kPickerTitle);
    picker_popup_pending_ = false;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                 vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(980.0f, vp->WorkSize.x - 40.0f),
                                  std::min(900.0f, vp->WorkSize.y - 40.0f)),
                           ImGuiCond_Appearing);
  bool keep = picker_open_;
  if (!ImGui::BeginPopupModal(kPickerTitle, &keep)) {
    picker_open_ = false;
    return;
  }
  if (!keep || !picker_open_) {
    picker_open_ = false;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  std::size_t total = templates_ ? templates_->templates().size() : 0;
  char caption[64];
  std::snprintf(caption, sizeof(caption), "%zu template%s", total, total == 1 ? "" : "s");
  theme::SectionHeader("Pick a Claim Pattern", caption);

  static const char* kFilters[] = {"All", "2D", "3D", "VR", "Mine"};
  for (int i = 0; i < 5; ++i) {
    if (i) ImGui::SameLine();
    bool sel = picker_filter_ == i;
    if (theme::ToggleButton(kFilters[i], &sel) && sel) picker_filter_ = i;
  }
  ImGui::SameLine(0.0f, 24.0f);
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##tplsearch", "Search templates (name, tag, plugin)",
                           picker_search_, sizeof(picker_search_));
  ImGui::Spacing();

  const ProjectTemplate* picked = templates_ ? templates_->find(picked_template_id_) : nullptr;
  ImGui::BeginChild("tplgrid", ImVec2(0, -118), ImGuiChildFlags_None);
  if (!templates_ || total == 0) {
    theme::StatusWarn("No templates found. The engine looks in <ts_admin folder>/templates, "
                      "the source templates/ folder and $TS_TEMPLATES_PATH.");
  } else {
    int shown = 0;
    for (const char* group : {"empty", "samples", "mine"}) {
      int n = 0;
      for (const ProjectTemplate& t : templates_->templates()) {
        n += t.group == group && template_matches(t, picker_filter_, picker_search_) ? 1 : 0;
      }
      if (n == 0) continue;
      shown += n;
      const std::string g = group;
      if (g == "empty") {
        draw_template_group("Empty", "clean starting points: no samples, no plugins", g);
      } else if (g == "samples") {
        draw_template_group("Samples", "playable sample claims to learn from", g);
      } else {
        draw_template_group("Mine", "saved with Save as Template", g);
      }
    }
    if (shown == 0) ImGui::TextDisabled("Nothing on the shelf matches.");
    for (const std::string& e : templates_->errors()) theme::StatusError(e.c_str());
  }
  ImGui::EndChild();

  theme::Divider();
  const bool picked_2d = picked && (picked->has_alt && picker_dim_ >= 0
                                        ? picker_dim_ == 0
                                        : picked->kind == ProjectKind::TwoD);
  if (picked) {
    ImGui::TextUnformatted(picked->name.c_str());
    ImGui::SameLine();
    theme::DimensionBadge(picked_2d ? "2D" : "3D");
    if (picked->has_alt) {
      // Truly Blank: pick the dimension.
      ImGui::SameLine();
      bool two = picked_2d, three = !picked_2d;
      if (theme::ToggleButton("2D##tpldim", &two) && two) {
        picker_dim_ = picked->kind == ProjectKind::TwoD ? -1 : 0;
        std::snprintf(picker_name_buf_, sizeof(picker_name_buf_), "%s", default_claim_name(*picked).c_str());
      }
      ImGui::SameLine();
      if (theme::ToggleButton("3D##tpldim", &three) && three) {
        picker_dim_ = picked->kind == ProjectKind::TwoD ? 1 : -1;
        std::snprintf(picker_name_buf_, sizeof(picker_name_buf_), "%s", default_claim_name(*picked).c_str());
      }
    }
    std::string plugs;
    for (const std::string& p : picked->plugins) plugs += (plugs.empty() ? "" : ", ") + p;
    ImGui::SameLine();
    ImGui::TextDisabled("plugins: %s", plugs.empty() ? (picked->plugins_set ? "none" : "defaults")
                                                     : plugs.c_str());
  } else {
    ImGui::TextDisabled("Pick a template above.");
  }
  ImGui::SetNextItemWidth(300.0f);
  ImGui::InputTextWithHint("##tplname", "Name your claim", picker_name_buf_, sizeof(picker_name_buf_));
  ImGui::SameLine();
  const bool three_d = picked && !picked_2d;
  ImGui::BeginDisabled(!three_d);
  bool vr = three_d && picker_vr_;
  if (ImGui::Checkbox("VR (headset rig, grab, teleport)", &vr)) picker_vr_ = vr;
  ImGui::EndDisabled();
  if (!three_d && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("VR is a switch on 3D claims.");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!picked);
  if (theme::PrimaryButton("Break Ground", ImVec2(130, 0)) && picked) {
    std::string err;
    if (!ProjectStore::validate_project_name(picker_name_buf_, &err)) {
      error_message_ = err;
    } else {
      request_new_from_template(picker_name_buf_, picked->id, three_d ? (picker_vr_ ? 1 : 0) : 0,
                                picked->has_alt ? picker_dim_ : -1);
      picker_open_ = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (theme::SecondaryButton("Cancel", ImVec2(90, 0))) {
    picker_open_ = false;
    ImGui::CloseCurrentPopup();
  }
  if (!error_message_.empty()) theme::StatusError(error_message_.c_str());
  ImGui::EndPopup();
}

void ProjectManagerScreen::draw_save_template_popup() {
  if (save_popup_pending_) {
    ImGui::OpenPopup("Save as Template");
    save_popup_pending_ = false;
  }
  if (!ImGui::BeginPopupModal("Save as Template", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  const ProjectInfo* p = highlighted_index_ && *highlighted_index_ < projects_.size()
                             ? &projects_[*highlighted_index_]
                             : nullptr;
  ImGui::Text("Save \"%s\" as a template", p ? p->name.c_str() : "(none)");
  ImGui::TextDisabled("Copies the claim (scenes, scripts, assets, plugins) into your templates.");
  ImGui::SetNextItemWidth(360.0f);
  ImGui::InputText("Name", save_name_buf_, sizeof(save_name_buf_));
  ImGui::SetNextItemWidth(360.0f);
  ImGui::InputTextMultiline("Description", save_desc_buf_, sizeof(save_desc_buf_),
                            ImVec2(360.0f, 60.0f));
  if (theme::PrimaryButton("Save Template", ImVec2(140, 0)) && p) {
    request_save_as_template(save_name_buf_, save_desc_buf_);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void ProjectManagerScreen::draw_claim_settings_popup() {
  if (settings_popup_pending_) {
    ImGui::OpenPopup("Claim Settings");
    settings_popup_pending_ = false;
  }
  if (!ImGui::BeginPopupModal("Claim Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  const ProjectInfo* p = highlighted_index_ && *highlighted_index_ < projects_.size()
                             ? &projects_[*highlighted_index_]
                             : nullptr;
  if (p) {
    ImGui::Text("%s", p->name.c_str());
    ImGui::SameLine();
    theme::DimensionBadge(p->kind == ProjectKind::TwoD ? "2D" : "3D");
    ImGui::TextDisabled("%s", p->path.c_str());
    ImGui::Spacing();
    if (p->kind == ProjectKind::TwoD) {
      ImGui::TextDisabled("VR is a switch on 3D claims.");
    } else {
      bool vr = vr_enabled(*p);
      if (ImGui::Checkbox("VR", &vr)) request_set_vr(vr);
      ImGui::TextDisabled("On: Preview in headset, VR Rig / Grabbable inspectors, mirror.\n"
                          "ts_game rides in a headset when one is there, else on the desktop.");
    }
  }
  if (theme::SecondaryButton("Close", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
