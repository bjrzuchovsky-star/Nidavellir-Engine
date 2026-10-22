#include "editor/screens/Editor2DScreen.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

void Editor2DScreen::draw_hierarchy() {
  hierarchy_focused_ =
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

  char caption[64];
  std::snprintf(caption, sizeof(caption), "%zu entit%s | %zu selected",
                workspace_.entities().size(),
                workspace_.entities().size() == 1 ? "y" : "ies",
                workspace_.selection_count());
  theme::SectionHeader("Scene", caption);

  const float bw = std::max(54.0f, (ImGui::GetContentRegionAvail().x -
                                    3.0f * ImGui::GetStyle().ItemSpacing.x) /
                                       4.0f);
  if (theme::PrimaryButton("+ New", ImVec2(bw, 0))) {
    create_entity("Entity");
  }
  const bool has_sel = workspace_.selection_count() > 0;
  ImGui::BeginDisabled(!has_sel);
  ImGui::SameLine();
  if (theme::CopperButton("Dupe", ImVec2(bw, 0))) {
    duplicate_selected();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Duplicate selection (Ctrl+D)");
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Rename", ImVec2(bw, 0))) {
    if (const Entity2D* e = workspace_.selected()) {
      begin_rename(e->id);
    }
  }
  ImGui::SameLine();
  if (theme::DangerButton("Delete", ImVec2(bw, 0))) {
    delete_selected();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Delete selection (Del)");
  }
  ImGui::EndDisabled();

  theme::Divider();

  // Search filter: name, id or component tag. Matches show flat.
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##HierFilter", "Search entities...", hierarchy_filter_,
                           sizeof(hierarchy_filter_));
  const bool filtering = hierarchy_filter_[0] != '\0';

  hier_rows_prev_.swap(hier_rows_);
  hier_rows_.clear();
  hier_open_context_ = false;
  hier_drop_id_ = 0;
  hier_drop_root_ = false;
  hier_toggle_lock_ = 0;
  hier_toggle_hide_ = 0;

  ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::TreeNodeEx("Scene##root",
                        ImGuiTreeNodeFlags_DefaultOpen |
                            ImGuiTreeNodeFlags_SpanAvailWidth)) {
    // Dropping a row on the "Scene" header moves it to the scene root.
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("TS_ENTITY2D")) {
        hier_drop_id_ = *static_cast<const std::uint64_t*>(pl->Data);
        hier_drop_root_ = true;
      }
      ImGui::EndDragDropTarget();
    }
    for (const Entity2D& e : workspace_.entities()) {
      if (filtering) {
        if (hierarchy_row_visible(e)) draw_hierarchy_row(e, 0);
      } else if (e.parent == 0) {
        draw_hierarchy_row(e, 0);
      }
    }
    ImGui::TreePop();
  }

  // Queued row actions (safe now that iteration is over).
  if (script_drop_id_ != 0) {
    attach_script(script_drop_id_, script_drop_rel_);
    script_drop_id_ = 0;
    script_drop_rel_.clear();
  }
  if (hier_toggle_lock_ != 0) {
    if (const Entity2D* e = workspace_.find(hier_toggle_lock_)) {
      set_entity_locked(e->id, !e->locked);
    }
  }
  if (hier_toggle_hide_ != 0) {
    if (const Entity2D* e = workspace_.find(hier_toggle_hide_)) {
      set_entity_hidden(e->id, !e->hidden);
    }
  }
  if (hier_drop_id_ != 0) {
    if (hier_drop_root_) {
      reparent(hier_drop_id_, 0);
    } else {
      if (hier_drop_where_ == Workspace2D::HierDrop::Inside && ImGui::GetIO().KeyAlt) {
        // Alt+drag onto a row: attach and snap onto it.
        if (!workspace_.is_selected(hier_drop_id_)) workspace_.select(hier_drop_id_);
        attach_selected_2d(hier_drop_target_, true);
      } else {
        hierarchy_drop(hier_drop_id_, hier_drop_target_, hier_drop_where_);
      }
    }
  }
  if (hier_open_context_) {
    ImGui::OpenPopup("##HierarchyContext");
  }
  if (ImGui::BeginPopup("##HierarchyContext")) {
    draw_entity_context_menu(context_id_);
    ImGui::EndPopup();
  }

  // Click on empty space below the list clears the selection.
  const ImVec2 rest = ImGui::GetContentRegionAvail();
  if (rest.x > 1.0f && rest.y > 1.0f) {
    ImGui::InvisibleButton("##HierarchyEmpty", rest);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
        !(ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift)) {
      workspace_.clear_selection();
    }
    // Drop below the list: unparent to the scene root.
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("TS_ENTITY2D")) {
        reparent(*static_cast<const std::uint64_t*>(pl->Data), 0);
      }
      ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      context_id_ = 0;
      ImGui::OpenPopup("##HierarchyEmptyContext");
    }
    if (ImGui::BeginPopup("##HierarchyEmptyContext")) {
      draw_entity_context_menu(0);
      ImGui::EndPopup();
    }
  }
}

void Editor2DScreen::draw_hierarchy_row(const Entity2D& e, int depth) {
  const ImGuiIO& io = ImGui::GetIO();
  const std::size_t row = hier_rows_.size();
  hier_rows_.push_back(e.id);
  const bool selected = workspace_.is_selected(e.id);
  const bool primary =
      workspace_.selected_id().has_value() && *workspace_.selected_id() == e.id;
  const bool filtering = hierarchy_filter_[0] != '\0';
  const std::vector<std::uint64_t> kids =
      filtering ? std::vector<std::uint64_t>{} : workspace_.children_of(e.id);

  if (renaming_ && rename_id_ == e.id) {
    ImGui::PushID(static_cast<int>(e.id));
    ImGui::Indent(depth * 12.0f);
    ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##rename", rename_buf_, sizeof(rename_buf_),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll)) {
      commit_rename();
    }
    if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered()) {
      commit_rename();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      cancel_rename();
    }
    ImGui::Unindent(depth * 12.0f);
    ImGui::PopID();
    return;
  }

  // Eye + lock toggles at the row start.
  ImGui::PushID(static_cast<int>(e.id));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, e.hidden ? theme::TextMuted() : theme::Sand());
  if (ImGui::SmallButton(e.hidden ? "-##eye" : "o##eye")) hier_toggle_hide_ = e.id;
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(e.hidden ? "Show (H)" : "Hide (H)");
  ImGui::SameLine(0.0f, 2.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, e.locked ? theme::Warning() : theme::TextMuted());
  if (ImGui::SmallButton(e.locked ? "L##lock" : ".##lock")) hier_toggle_lock_ = e.id;
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(e.locked ? "Unlock (Ctrl+Shift+L)" : "Lock (Ctrl+L)");
  ImGui::PopID();
  ImGui::SameLine(0.0f, 4.0f);

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                             ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
  if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  if (selected) flags |= ImGuiTreeNodeFlags_Selected;
  const bool dim = workspace_.effectively_hidden(e);
  if (dim) ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(e.id)),
                                      flags, "%s", e.name.c_str());
  if (dim) ImGui::PopStyleColor();

  std::string tag;
  if (e.tilemap || e.sprite) tag = e.tilemap ? "tiles" : "sprite";
  if (e.player) tag += (tag.empty() ? "P" : " P") + std::to_string(e.player->slot + 1);
  if (e.camera) tag += tag.empty() ? "cam" : " cam";
  if (e.spawn) {
    tag += (tag.empty() ? "spawn P" : " spawn P") + std::to_string(e.spawn->slot + 1);
  }
  if (!kids.empty() && e.color[3] == 0.0f && !e.sprite && !e.tilemap) {
    tag += tag.empty() ? "group" : " group";
  }
  const ImVec2 mn = ImGui::GetItemRectMin();
  const ImVec2 mx = ImGui::GetItemRectMax();
  if (!tag.empty()) {
    const float tw = ImGui::CalcTextSize(tag.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(mx.x - tw - 6.0f, mn.y + ImGui::GetStyle().FramePadding.y),
        theme::U32(theme::TextMuted()), tag.c_str());
  }
  if (primary) {
    ImGui::GetWindowDrawList()->AddRectFilled(mn, ImVec2(mn.x + 3.0f, mx.y),
                                              theme::U32(theme::Accent()));
  }

  // Drag source / target: top quarter = before, bottom quarter = after,
  // middle = make it a child.
  if (!is_playing() && ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("TS_ENTITY2D", &e.id, sizeof(e.id));
    ImGui::Text("%s", e.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (dragging_script() && !e.tilemap && ImGui::BeginDragDropTarget()) {
    // A script from the Scripts panel / Asset Browser: attach it to this row.
    if (std::string rel = accept_script_payload(); !rel.empty()) {
      script_drop_id_ = e.id;
      script_drop_rel_ = std::move(rel);
    }
    ImGui::EndDragDropTarget();
  } else if (ImGui::BeginDragDropTarget()) {
    const float h = mx.y - mn.y;
    const float my = io.MousePos.y - mn.y;
    Workspace2D::HierDrop where = Workspace2D::HierDrop::Inside;
    if (my < h * 0.25f) {
      where = Workspace2D::HierDrop::Before;
    } else if (my > h * 0.75f) {
      where = Workspace2D::HierDrop::After;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = theme::U32(theme::Accent());
    if (where == Workspace2D::HierDrop::Before) {
      dl->AddLine(ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y), col, 2.0f);
    } else if (where == Workspace2D::HierDrop::After) {
      dl->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y), col, 2.0f);
    } else {
      dl->AddRect(mn, mx, col, 2.0f, 0, 1.5f);
    }
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(
            "TS_ENTITY2D", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      hier_drop_id_ = *static_cast<const std::uint64_t*>(pl->Data);
      hier_drop_target_ = e.id;
      hier_drop_where_ = where;
    }
    ImGui::EndDragDropTarget();
  }

  if (target_picking_2d() && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    finish_target_pick_2d(e.id);
  } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
    const bool ctrl = io.KeyCtrl || io.KeySuper;
    if (io.KeyShift && has_range_anchor_ && range_anchor_ < hier_rows_prev_.size() &&
        row < hier_rows_prev_.size()) {
      std::vector<std::uint64_t> ids;
      if (ctrl) ids = workspace_.selection();
      const std::size_t lo = std::min(range_anchor_, row);
      const std::size_t hi = std::max(range_anchor_, row);
      for (std::size_t k = lo; k <= hi; ++k) ids.push_back(hier_rows_prev_[k]);
      workspace_.set_selection(ids, e.id);
    } else if (ctrl) {
      workspace_.toggle_selection(e.id);
      range_anchor_ = row;
      has_range_anchor_ = true;
    } else {
      workspace_.select(e.id);
      range_anchor_ = row;
      has_range_anchor_ = true;
    }
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    begin_rename(e.id);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    if (!selected) workspace_.select(e.id);
    context_id_ = e.id;
    hier_open_context_ = true;
  }

  if (open && !kids.empty()) {
    for (std::uint64_t k : kids) {
      if (const Entity2D* c = workspace_.find(k)) draw_hierarchy_row(*c, depth + 1);
    }
    ImGui::TreePop();
  }
}

bool Editor2DScreen::transform_field(const char* label, float* field, int slot,
                                     float vmin, float vmax, bool is_extent) {
  const bool snap = workspace_.snap_enabled();
  const std::uint64_t owner =
      workspace_.selected_id() ? *workspace_.selected_id() : 0;
  const bool tracking = insp_drag_slot_ == slot && insp_drag_entity_ == owner;
  // While dragging with snap on, edit an unsnapped shadow value so small
  // mouse motions accumulate instead of being rounded away every frame.
  float v = (snap && tracking) ? insp_drag_raw_ : *field;
  const float speed = snap ? std::max(0.5f, workspace_.grid_size() * 0.05f)
                           : 0.5f;
  const bool changed =
      ImGui::DragFloat(label, &v, speed, vmin, vmax, snap ? "%.0f" : "%.2f");
  if (ImGui::IsItemActive()) {
    insp_drag_slot_ = slot;
    insp_drag_entity_ = owner;
    insp_drag_raw_ = v;
  } else if (tracking) {
    insp_drag_slot_ = -1;
  }
  bool applied = false;
  if (changed) {
    float nv = v;
    if (snap) {
      nv = is_extent ? workspace_.snap_extent(v) : workspace_.snap_value(v);
    }
    nv = std::clamp(nv, vmin, vmax);
    if (nv != *field) {
      *field = nv;
      applied = true;
      mark_dirty();
    }
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    if (snap) {
      *field = is_extent ? workspace_.snap_extent(*field)
                         : workspace_.snap_value(*field);
    }
    mark_dirty();
  }
  // One undo step per field edit: opens on activation, commits (and
  // autosaves) on deactivation.
  char step[32];
  std::snprintf(step, sizeof(step), "Edit %s", label);
  track_inspector_item(step);
  return applied;
}

void Editor2DScreen::draw_inspector() {
  Entity2D* e = workspace_.selected();
  // Remember the primary as it is before any widget touches it this frame;
  // begin_inspector_edit() uses it as the before-state.
  if (edit_source_ != EditSource::Inspector) {
    insp_frame_valid_ = e != nullptr;
    if (e) {
      insp_frame_entity_ = *e;
    }
  }
  const std::size_t count = workspace_.selection_count();
  if (!e) {
    theme::SectionHeader("Inspector", "nothing selected");
    ImGui::Spacing();
    theme::StatusInfo(
        "Nothing in the crosshairs. Click an entity in the viewport or the "
        "Scene list. Ctrl+click or drag a box to round up several.");
  } else {
    char sel_caption[48];
    std::snprintf(sel_caption, sizeof(sel_caption), "%zu selected", count);
    theme::SectionHeader("Inspector", sel_caption);
    if (count > 1) {
      char buf[96];
      std::snprintf(buf, sizeof(buf),
                    "%zu selected. Fields edit the primary (amber); drag, "
                    "nudge, Dupe and Delete hit them all.",
                    count);
      theme::StatusWarn(buf);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    ImGui::TextUnformatted(e->name.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("id %llu", static_cast<unsigned long long>(e->id));
    ImGui::Spacing();

    char name_buf[128];
    std::memset(name_buf, 0, sizeof(name_buf));
    std::strncpy(name_buf, e->name.c_str(), sizeof(name_buf) - 1);
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
      if (name_buf[0] != '\0') {
        e->name = name_buf;
        mark_dirty();
      }
    }
    track_inspector_item("Rename");

    ImGui::SeparatorText(workspace_.snap_enabled() ? "Transform  [snap]"
                                                   : "Transform");
    const float old_x = e->x, old_y = e->y;
    transform_field("X", &e->x, 0, -1.0e6f, 1.0e6f, false);
    transform_field("Y", &e->y, 1, -1.0e6f, 1.0e6f, false);
    if (e->x != old_x || e->y != old_y) {
      // Children ride along with their parent.
      const float dx = e->x - old_x, dy = e->y - old_y;
      const std::uint64_t self = e->id;
      for (std::uint64_t d : workspace_.descendants_of(self)) {
        if (Entity2D* c = workspace_.find(d)) {
          c->x += dx;
          c->y += dy;
        }
      }
      e = workspace_.find(self);
    }
    if (e->tilemap) {
      // A TileMap's size is its grid: edit Cols / Rows / Tile px below.
      ImGui::BeginDisabled();
      float wh[2] = {e->w, e->h};
      ImGui::InputFloat2("W / H", wh, "%.0f");
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Follows Cols x Tile px and Rows x Tile px.");
      }
    } else {
      transform_field("W", &e->w, 2, 1.0f, 4096.0f, true);
      transform_field("H", &e->h, 3, 1.0f, 4096.0f, true);
    }
    if (!e->tilemap) {
      float rot = e->rotation;
      if (ImGui::DragFloat("Rotation", &rot, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
        e->rotation = rot;
        mark_dirty();
      }
      track_inspector_item("Rotate");
    }
    {
      const std::uint64_t self = e->id;
      bool locked = e->locked;
      bool hidden = e->hidden;
      const bool lock_hit = ImGui::Checkbox("Locked", &locked);
      ImGui::SameLine();
      const bool hide_hit = ImGui::Checkbox("Hidden (editor)", &hidden);
      if (lock_hit) set_entity_locked(self, locked);
      if (hide_hit) set_entity_hidden(self, hidden);
      e = workspace_.find(self);
      if (e->parent != 0) {
        const Entity2D* par = workspace_.find(e->parent);
        ImGui::TextDisabled("Parent: %s", par ? par->name.c_str() : "?");
        ImGui::SameLine();
        if (ImGui::SmallButton("Unparent")) {
          reparent(self, 0);
          e = workspace_.find(self);
        }
      }
    }
    if (count > 1 || !workspace_.snap_enabled()) {

      if (theme::SecondaryButton("Snap selection to grid", ImVec2(-1, 0))) {
        snap_selected_to_grid();
      }
    }

    ImGui::SeparatorText("Appearance");
    if (ImGui::ColorEdit4("Tint", e->color,
                          ImGuiColorEditFlags_AlphaBar |
                              ImGuiColorEditFlags_Float)) {
      mark_dirty();
    }
    track_inspector_item("Edit Tint");
    if (ImGui::InputInt("Layer / Z", &e->layer)) {
      mark_dirty();
    }
    track_inspector_item("Edit Layer");

    // TileMap or Sprite section. Uses discrete steps that may flush the
    // coalesced edit above, so re-fetch the entity afterwards.
    const std::uint64_t eid = e->id;
    draw_inspector_components(*e);
    // Player / Camera2D / Spawn Point (same re-fetch rule).
    if (workspace_.find(eid)) {
      draw_inspector_gameplay(eid);
    }
    if (workspace_.find(eid)) {
      draw_inspector_follow_2d(eid);
    }
    // Animator (idle / walk clips from a .anim.json).
    if (workspace_.find(eid)) {
      draw_inspector_animator(eid);
    }
    e = workspace_.find(eid);
  }

  ImGui::Spacing();
  theme::Divider();
  if (theme::SecondaryButton("Back to Projects", ImVec2(-1, 0))) {
    request_back_to_projects();
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
