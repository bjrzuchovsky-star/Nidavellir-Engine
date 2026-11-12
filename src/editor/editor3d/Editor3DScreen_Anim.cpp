// Animation preview panel + Animator inspector.
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/ux/Tooltip.h"

#include "editor/scripting/GraphCanvas.h"
#include "editor/ui/Theme.h"
#include "render3d/Animator.h"
#include "render3d/Retarget.h"
#include "render3d/Skeleton.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

bool edit_str(const char* label, std::string* s) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s", s->c_str());
  if (ImGui::InputText(label, buf, sizeof(buf))) {
    *s = buf;
    return true;
  }
  return false;
}

// Combo over the model's clips (free text when there is no model).
bool clip_combo(const char* label, std::string* clip, const render3d::Model* m) {
  if (!m || m->clips.empty()) return edit_str(label, clip);
  bool changed = false;
  if (ImGui::BeginCombo(label, clip->empty() ? "(none)" : clip->c_str())) {
    for (const render3d::Clip& c : m->clips) {
      if (ImGui::Selectable(c.name.c_str(), c.name == *clip)) {
        *clip = c.name;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

bool name_combo(const char* label, std::string* value, const std::vector<std::string>& names,
                const char* any = nullptr) {
  bool changed = false;
  if (ImGui::BeginCombo(label, value->empty() ? "(none)" : value->c_str())) {
    if (any && ImGui::Selectable(any, *value == any)) {
      *value = any;
      changed = true;
    }
    for (const std::string& n : names) {
      if (ImGui::Selectable(n.c_str(), n == *value)) {
        *value = n;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

}  // namespace

void draw_animator_fields(scene3d::Animator3D& a, const render3d::Model* model,
                          const std::function<void(const char*)>& after, const char** commit,
                          const std::function<void()>& open_graph) {
  using scene3d::AnimParam3D;
  using scene3d::AnimTransition3D;
  auto discrete = [&](bool changed, const char* what) {
    if (changed) *commit = what;
  };
  if (model && model->skinned()) {
    ImGui::TextDisabled("%zu bones, %zu clips", model->skeleton.joints.size(), model->clips.size());
  } else {
    ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "Mesh has no skin: pick a skinned glTF.");
  }
  edit_str("Model", &a.model);
  after("Animator model");
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) ux::SetTooltip("Empty = the entity's mesh");
  ImGui::DragFloat("Playback speed", &a.speed, 0.01f, 0.0f, 5.0f, "%.2fx");
  after("Animator speed");
  discrete(ImGui::Checkbox("Driven by rider", &a.drive_from_player), "Driven by rider");
  ImGui::SameLine();
  discrete(ImGui::Checkbox("Root motion", &a.root_motion), "Root motion");
  if (a.root_motion) {
    std::vector<std::string> bones;
    if (model) {
      for (const render3d::Joint& j : model->skeleton.joints) bones.push_back(j.name);
    }
    discrete(name_combo("Root bone", &a.root_bone, bones), "Root bone");
  }
  if (ImGui::Button("Open Animator Graph") && open_graph) open_graph();

  // Parameters.
  if (ImGui::TreeNodeEx("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (std::size_t i = 0; i < a.params.size(); ++i) {
      AnimParam3D& p = a.params[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::SetNextItemWidth(110);
      edit_str("##pn", &p.name);
      after("Param name");
      ImGui::SameLine();
      int ty = static_cast<int>(p.type);
      ImGui::SetNextItemWidth(80);
      if (ImGui::Combo("##pt", &ty, "Float\0Bool\0Trigger\0")) {
        p.type = static_cast<AnimParam3D::Type>(ty);
        *commit = "Param type";
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70);
      ImGui::DragFloat("##pv", &p.value, 0.05f);
      after("Param value");
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        a.params.erase(a.params.begin() + static_cast<std::ptrdiff_t>(i));
        *commit = "Remove param";
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Param")) {
      a.params.push_back({"param" + std::to_string(a.params.size()), AnimParam3D::Type::Float, 0.0f});
      *commit = "Add param";
    }
    ImGui::TreePop();
  }
}

// ---- Preview -----------------------------------------------------------------------

const render3d::Model* Editor3DScreen::entity_model(const scene3d::Entity3D& e) {
  ensure_renderer();
  if (!view_) return nullptr;
  std::string src = e.animator && !e.animator->model.empty() ? e.animator->model : "";
  if (src.empty() && e.mesh && !e.mesh->is_primitive()) src = e.mesh->source;
  if (src.empty()) return nullptr;
  return view_->model(src);
}

namespace {
const render3d::Clip* preview_clip(const render3d::Model* m, std::string* name) {
  if (!m || !m->skinned() || m->clips.empty()) return nullptr;
  const int i = m->find_clip(*name);
  if (i >= 0) return &m->clips[static_cast<std::size_t>(i)];
  *name = m->clips.front().name;
  return &m->clips.front();
}
}  // namespace

void Editor3DScreen::anim_preview_tick(float dt) {
  AnimPreview& p = anim_preview_;
  if (!p.open || !p.playing) return;
  const scene3d::Entity3D* e = find(primary());
  if (!e) return;
  const render3d::Clip* c = authored_clip(p.clip);
  if (!c) c = preview_clip(entity_model(*e), &p.clip);
  if (!c || c->duration <= 0.0f) return;
  p.time = std::fmod(p.time + dt * p.speed, c->duration);
  if (p.time < 0.0f) p.time += c->duration;
}

bool Editor3DScreen::anim_preview_pose(scene3d::Entity3D* e) {
  const render3d::Model* m = entity_model(*e);
  // An unsaved (or saved-but-still-open) authored clip wins over the file.
  const render3d::Clip* c = authored_clip(anim_preview_.clip);
  if (!c) c = preview_clip(m, &anim_preview_.clip);
  if (!c) return false;
  render3d::Pose pose = render3d::rest_pose(m->skeleton);
  render3d::sample_clip(*c, anim_preview_.time, anim_preview_.loop, &pose);
  if (anim_preview_.in_place) {
    const render3d::HumanoidMap map = render3d::map_humanoid(m->skeleton);
    int hip = map.get(render3d::HumanSlot::Hips);
    if (hip < 0) hip = 0;
    if (static_cast<std::size_t>(hip) < pose.size()) {
      pose[static_cast<std::size_t>(hip)].t = m->skeleton.joints[static_cast<std::size_t>(hip)].t;
    }
  }
  auto pal = std::make_shared<std::vector<render3d::Mat4>>();
  render3d::skin_palette(m->skeleton, pose, pal.get());
  e->skin_palette = std::move(pal);
  return true;
}

void Editor3DScreen::draw_anim_preview() {
  AnimPreview& p = anim_preview_;
  const scene3d::Entity3D* e = find(primary());
  if (!e) {
    ImGui::TextDisabled("Select a skinned entity (e.g. the Drifter).");
    return;
  }
  const render3d::Model* m = entity_model(*e);
  if (!m || !m->skinned()) {
    ImGui::TextDisabled("\"%s\" has no skinned mesh.", e->name.c_str());
    return;
  }
  if (m->clips.empty()) {
    ImGui::TextDisabled("\"%s\" has a skeleton but no clips.", e->name.c_str());
    return;
  }
  const render3d::Clip* cur = preview_clip(m, &p.clip);
  ImGui::Text("%s  |  %zu bones", e->name.c_str(), m->skeleton.joints.size());
  if (playing()) ImGui::TextDisabled("Paused while riding: the animator drives the pose.");
  theme::SectionHeader("Clips");
  if (ImGui::BeginListBox("##clips", ImVec2(-1, std::min(8.0f, static_cast<float>(m->clips.size())) *
                                                    ImGui::GetTextLineHeightWithSpacing() + 6))) {
    for (const render3d::Clip& c : m->clips) {
      char label[160];
      std::snprintf(label, sizeof(label), "%s  (%.2f s, %zu ch)", c.name.c_str(), c.duration, c.channels.size());
      if (ImGui::Selectable(label, c.name == p.clip)) {
        p.clip = c.name;
        p.time = 0.0f;
      }
    }
    for (const render3d::Clip& c : timeline_.clips) {
      if (m->find_clip(c.name) >= 0) continue;
      char label[160];
      std::snprintf(label, sizeof(label), "%s  (authored, %.2f s)", c.name.c_str(), c.duration);
      if (ImGui::Selectable(label, c.name == p.clip)) {
        p.clip = c.name;
        p.time = 0.0f;
      }
    }
    ImGui::EndListBox();
  }
  if (ImGui::Button(p.playing ? "Pause" : "Play", ImVec2(70, 0))) p.playing = !p.playing;
  ImGui::SameLine();
  if (ImGui::Button("|<")) p.time = 0.0f;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  ImGui::SliderFloat("##speed", &p.speed, -2.0f, 3.0f, "speed %.2fx");
  float t = p.time;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderFloat("##time", &t, 0.0f, std::max(cur->duration, 0.001f), "%.3f s")) {
    p.time = t;
    p.playing = false;  // scrubbing pauses
  }
  // Event ticks of this entity's animator on the timeline.
  if (e->animator) {
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const scene3d::AnimEvent3D& ev : e->animator->events) {
      if (ev.clip != p.clip || cur->duration <= 0.0f) continue;
      const float x = lo.x + (hi.x - lo.x) * std::clamp(ev.time / cur->duration, 0.0f, 1.0f);
      dl->AddLine(ImVec2(x, lo.y), ImVec2(x, lo.y + 5), IM_COL32(255, 190, 80, 255), 2.0f);
    }
  }
  int interp[3] = {0, 0, 0};
  for (const render3d::Channel& ch : cur->channels) ++interp[static_cast<int>(ch.interp)];
  ImGui::TextDisabled("%d linear, %d step, %d cubic-spline channels", interp[0], interp[1], interp[2]);
  // Highlight one bone in the viewport overlay (View > Bones).
  ux::BeginAutoTips("AnimationPreview");
  const char* bone_label = bone_joint_ >= 0 && static_cast<std::size_t>(bone_joint_) < m->skeleton.joints.size()
                               ? m->skeleton.joints[static_cast<std::size_t>(bone_joint_)].name.c_str()
                               : "(all)";
  if (ImGui::BeginCombo("Bone", bone_label)) {
    if (ImGui::Selectable("(all)", bone_joint_ < 0)) bone_joint_ = -1;
    for (std::size_t i = 0; i < m->skeleton.joints.size(); ++i) {
      if (ImGui::Selectable(m->skeleton.joints[i].name.c_str(), bone_joint_ == static_cast<int>(i))) {
        bone_joint_ = static_cast<int>(i);
      }
    }
    ImGui::EndCombo();
  }
  if (!show_bones_) ImGui::TextDisabled("View > Bones is off, so the skeleton is hidden.");
  ux::EndAutoTips();
}

void Editor3DScreen::draw_anim_graph() {
  const scene3d::Entity3D* cur = find(primary());
  if (!cur) {
    ImGui::TextDisabled("Select a character.");
    return;
  }
  if (playing()) {
    ImGui::TextDisabled("Stop riding to edit the graph. The animator keeps playing.");
    return;
  }
  if (const std::uint64_t skin = resolve_skinned_entity(cur->id)) cur = find(skin);
  const render3d::Model* model = cur ? entity_model(*cur) : nullptr;
  const bool skinned = model && model->skinned();
  if (!cur) return;
  scene3d::Entity3D ent = *cur;
  const char* what = "Animator graph";
  auto touch = [&](bool commit_now) {
    scene3d::Entity3D* live = scene_.find(ent.id);
    if (!live) return;
    if (*live == ent) {
      if (commit_now && anim_graph_.before) {
        push_undo(what, *anim_graph_.before);
        anim_graph_.before.reset();
      }
      return;
    }
    if (!anim_graph_.before) anim_graph_.before = scene_;
    *live = ent;
    if (commit_now) {
      push_undo(what, *anim_graph_.before);
      anim_graph_.before.reset();
    }
  };

  if (!skinned) {
    ImGui::TextWrapped("\"%s\" has no skeleton. The graph plays on a rigged character.", ent.name.c_str());
    return;
  }
  if (!ent.animator) {
    ImGui::TextWrapped("No Animator on \"%s\" yet. Locomotion builds idle / walk / run by speed. One clip plays the preview.",
                       ent.name.c_str());
    if (ImGui::Button("Locomotion template")) {
      ent.animator = scene3d::default_locomotion_animator();
      what = "Locomotion template";
      touch(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("One clip") && model && !model->clips.empty()) {
      scene3d::Animator3D an;
      an.drive_from_player = false;
      scene3d::AnimState3D st;
      st.name = anim_preview_.clip.empty() ? model->clips.front().name : anim_preview_.clip;
      st.clip = st.name;
      st.loop = anim_preview_.loop;
      scene3d::AnimLayer3D layer;
      layer.entry = st.name;
      layer.states = {st};
      an.layers = {layer};
      an.speed = anim_preview_.speed;
      ent.animator = std::move(an);
      what = "One clip";
      touch(true);
    }
    return;
  }

  if (ImGui::SmallButton("Locomotion template")) {
    const std::string keep = ent.animator->model;
    const bool drive = ent.animator->drive_from_player;
    ent.animator = scene3d::default_locomotion_animator();
    ent.animator->model = keep;
    ent.animator->drive_from_player = drive;
    anim_graph_.pos.clear();
    anim_graph_.selected = "locomotion";
    anim_graph_.link_from.clear();
    anim_graph_.layer = 0;
    what = "Locomotion template";
    touch(true);
    return;
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("Replace this Animator with idle / walk / run blended by speed, plus a jump.");
  }

  scene3d::Animator3D& anim = *ent.animator;
  if (anim.layers.empty()) anim.layers.push_back({});
  if (anim_graph_.layer < 0 || anim_graph_.layer >= static_cast<int>(anim.layers.size())) anim_graph_.layer = 0;

  ImGui::SameLine();
  if (ImGui::BeginCombo("Layer", anim.layers[static_cast<std::size_t>(anim_graph_.layer)].name.c_str())) {
    for (int i = 0; i < static_cast<int>(anim.layers.size()); ++i) {
      if (ImGui::Selectable(anim.layers[static_cast<std::size_t>(i)].name.c_str(), i == anim_graph_.layer)) {
        anim_graph_.layer = i;
        anim_graph_.selected.clear();
        anim_graph_.link_from.clear();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("+ State")) {
    scene3d::AnimLayer3D& layer = anim.layers[static_cast<std::size_t>(anim_graph_.layer)];
    scene3d::AnimState3D st;
    st.name = "state";
    int n = 2;
    auto taken = [&](const std::string& name) {
      for (const scene3d::AnimState3D& s : layer.states)
        if (s.name == name) return true;
      return false;
    };
    while (taken(st.name)) st.name = "state" + std::to_string(n++);
    if (model && !model->clips.empty()) st.clip = model->clips.front().name;
    if (layer.entry.empty()) layer.entry = st.name;
    anim_graph_.selected = st.name;
    layer.states.push_back(std::move(st));
    what = "Add state";
    touch(true);
  }
  ImGui::SameLine();
  const bool linking = !anim_graph_.link_from.empty();
  if (ImGui::SmallButton(linking ? "Cancel link" : "Transition")) {
    if (linking) anim_graph_.link_from.clear();
    else if (!anim_graph_.selected.empty()) anim_graph_.link_from = anim_graph_.selected;
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip(linking ? "Click the state this should switch to."
                           : "Or drag from a node's out pin to another state's in pin. Speed > 0.1 when that parameter exists.");
  }

  scene3d::AnimLayer3D& layer = anim.layers[static_cast<std::size_t>(anim_graph_.layer)];
  const std::string layer_key = std::to_string(anim_graph_.layer) + "/";
  auto pos_of = [&](const std::string& name, int index) -> std::pair<float, float>& {
    const std::string key = layer_key + name;
    auto it = anim_graph_.pos.find(key);
    if (it == anim_graph_.pos.end()) {
      float x = 280.0f + static_cast<float>(std::max(index, 0) % 3) * 230.0f;
      float y = 48.0f + static_cast<float>(std::max(index, 0) / 3) * 150.0f;
      if (name == "##entry") {
        x = 24.0f;
        y = 48.0f;
      } else if (name == "##any") {
        x = 24.0f;
        y = 230.0f;
      }
      it = anim_graph_.pos.emplace(key, std::make_pair(x, y)).first;
    }
    return it->second;
  };

  ImGui::BeginChild("##graph_body", ImVec2(0, 0));
  ImGui::BeginChild("##params", ImVec2(168, 0), true);
  ImGui::TextDisabled("Parameters");
  for (std::size_t i = 0; i < anim.params.size(); ++i) {
    scene3d::AnimParam3D& p = anim.params[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetNextItemWidth(150);
    if (edit_str("##pn", &p.name)) touch(false);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      what = "Parameter";
      touch(true);
    }
    int ty = static_cast<int>(p.type);
    ImGui::SetNextItemWidth(78);
    if (ImGui::Combo("##pt", &ty, "Float\0Bool\0Trigger\0")) {
      p.type = scene3d::AnimParam3D::Type(ty);
      what = "Parameter";
      touch(true);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64);
    if (ImGui::DragFloat("##pv", &p.value, 0.05f)) {
      what = "Parameter";
      touch(false);
    }
    if (ImGui::SmallButton("x##p")) {
      anim.params.erase(anim.params.begin() + static_cast<std::ptrdiff_t>(i));
      what = "Remove parameter";
      touch(true);
      ImGui::PopID();
      break;
    }
    ImGui::PopID();
  }
  if (ImGui::SmallButton("+ Param")) {
    anim.params.push_back({"param" + std::to_string(anim.params.size()), scene3d::AnimParam3D::Type::Float, 0.0f});
    what = "Add parameter";
    touch(true);
  }
  ImGui::EndChild();
  ImGui::SameLine();

  ImGui::BeginChild("##canvas_col", ImVec2(-230, 0), true, ImGuiWindowFlags_NoScrollbar);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 size(std::max(avail.x, 40.0f), std::max(avail.y, 40.0f));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float z = anim_graph_.zoom;
  const float nw = graphcanvas::kNodeW * z;
  const float nh = (graphcanvas::kHeaderH + graphcanvas::kRowH + 8.0f) * z;
  graphcanvas::draw_grid(dl, origin, size, anim_graph_.pan_x, anim_graph_.pan_y, z);
  auto to_screen = [&](float x, float y) {
    return ImVec2(origin.x + anim_graph_.pan_x + x * z, origin.y + anim_graph_.pan_y + y * z);
  };
  auto card_at = [&](const std::string& name, int index) {
    const auto& p = pos_of(name, index);
    return to_screen(p.first, p.second);
  };
  auto pin_y = [&](ImVec2 a) { return a.y + (graphcanvas::kHeaderH + graphcanvas::kRowH * 0.5f + 4.0f) * z; };
  auto out_pin = [&](ImVec2 a) { return ImVec2(a.x + nw, pin_y(a)); };
  auto in_pin = [&](ImVec2 a) { return ImVec2(a.x, pin_y(a)); };
  auto index_of = [&](const std::string& name) {
    for (int i = 0; i < static_cast<int>(layer.states.size()); ++i)
      if (layer.states[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
  };
  auto cond_text = [](const scene3d::AnimTransition3D& tr) {
    const char* op = "?";
    switch (tr.op) {
      case scene3d::AnimTransition3D::Op::Greater: op = ">"; break;
      case scene3d::AnimTransition3D::Op::Less: op = "<"; break;
      case scene3d::AnimTransition3D::Op::True: op = "on"; break;
      case scene3d::AnimTransition3D::Op::False: op = "off"; break;
      case scene3d::AnimTransition3D::Op::Trigger: op = "trigger"; break;
      case scene3d::AnimTransition3D::Op::Always: op = "always"; break;
    }
    char label[96];
    if (tr.op == scene3d::AnimTransition3D::Op::Greater || tr.op == scene3d::AnimTransition3D::Op::Less)
      std::snprintf(label, sizeof(label), "%s %s %.2f", tr.param.c_str(), op, tr.value);
    else if (tr.param.empty())
      std::snprintf(label, sizeof(label), "%s", op);
    else
      std::snprintf(label, sizeof(label), "%s %s", tr.param.c_str(), op);
    return std::string(label);
  };
  auto source_of = [&](const std::string& from) -> ImVec2 {
    if (from == "##entry") return out_pin(card_at("##entry", -1));
    if (from == "##any" || from == "*") return out_pin(card_at("##any", -1));
    const int i = index_of(from);
    if (i < 0) return out_pin(card_at("##any", -1));
    return out_pin(card_at(layer.states[static_cast<std::size_t>(i)].name, i));
  };
  const int entry_i = index_of(layer.entry);
  if (entry_i >= 0) {
    const ImVec2 a = source_of("##entry");
    const ImVec2 b = in_pin(card_at(layer.entry, entry_i));
    graphcanvas::draw_wire(dl, a, b, theme::U32(theme::Copper(), 0.9f), 2.4f * z);
    theme::DrawSizedText(dl, ImVec2((a.x + b.x) * 0.5f - 16.0f, (a.y + b.y) * 0.5f - 16.0f), 12.0f * z,
                         theme::U32(theme::Text()), "entry");
  }
  for (std::size_t ti = 0; ti < layer.transitions.size(); ++ti) {
    const scene3d::AnimTransition3D& tr = layer.transitions[ti];
    const int to = index_of(tr.to);
    if (to < 0) continue;
    const ImVec2 a = source_of(tr.from);
    const ImVec2 b = in_pin(card_at(tr.to, to));
    const bool hot = anim_graph_.selected_tr == static_cast<int>(ti);
    graphcanvas::draw_wire(dl, a, b, hot ? theme::U32(theme::Accent()) : theme::U32(theme::Text(), 0.8f),
                           (hot ? 3.2f : 2.2f) * z);
    const std::string label = cond_text(tr);
    const ImVec2 m = theme::MeasureText(12.0f * z, label.c_str());
    const ImVec2 lp((a.x + b.x) * 0.5f - m.x * 0.5f, (a.y + b.y) * 0.5f - m.y - 2.0f);
    theme::DrawSizedText(dl, lp, 12.0f * z, theme::U32(hot ? theme::Accent() : theme::Text()), label.c_str());
  }
  if (!anim_graph_.link_from.empty()) {
    ImVec2 a = source_of(anim_graph_.link_from);
    graphcanvas::draw_wire(dl, a, ImGui::GetIO().MousePos, theme::U32(theme::Accent()), 2.4f * z);
  }
  auto draw_special = [&](const char* key, const char* title, const char* cat, ImVec4 header) {
    const ImVec2 a = card_at(key, -1);
    const ImVec2 b(a.x + nw, a.y + nh);
    graphcanvas::draw_node_card(dl, a, b, z, title, cat, header, anim_graph_.link_from == key);
  };
  draw_special("##entry", "Entry", "Flow", theme::Copper());
  draw_special("##any", "Any State", "Flow", theme::Sand());
  for (int i = 0; i < static_cast<int>(layer.states.size()); ++i) {
    const scene3d::AnimState3D& st = layer.states[static_cast<std::size_t>(i)];
    const ImVec2 a = card_at(st.name, i);
    const ImVec2 b(a.x + nw, a.y + nh);
    const bool blend = !st.blend.empty();
    const bool sel = st.name == anim_graph_.selected;
    graphcanvas::draw_node_card(dl, a, b, z, st.name.c_str(), blend ? "Blend" : "State",
                               blend ? theme::Sand() : theme::Accent(), sel);
    std::string sub = blend ? (st.blend_param.empty() ? "blend" : st.blend_param + " blend") : st.clip;
    if (sub.size() > 22) sub = sub.substr(0, 20) + "..";
    theme::DrawSizedText(dl, ImVec2(a.x + 12.0f * z, a.y + (graphcanvas::kHeaderH + 4.0f) * z), 12.0f * z,
                         theme::U32(theme::Text(), 0.75f), sub.c_str());
    const ImVec2 ip = in_pin(a);
    const ImVec2 op = out_pin(a);
    dl->AddCircleFilled(ip, graphcanvas::kPinR * z, theme::U32(theme::Text()));
    dl->AddCircleFilled(op, graphcanvas::kPinR * z, theme::U32(theme::Accent()));
  }
  {
    const ImVec2 e = out_pin(card_at("##entry", -1));
    const ImVec2 any = out_pin(card_at("##any", -1));
    dl->AddCircleFilled(e, graphcanvas::kPinR * z, theme::U32(theme::Copper()));
    dl->AddCircleFilled(any, graphcanvas::kPinR * z, theme::U32(theme::Sand()));
  }
  char info[96];
  std::snprintf(info, sizeof(info), "%zu states  %zu wires  |  zoom %d%%   drag a pin, right-click to add",
                layer.states.size(), layer.transitions.size(), static_cast<int>(std::round(z * 100)));
  theme::DrawSizedText(dl, ImVec2(origin.x + 12, origin.y + size.y - 22), 13, theme::U32(theme::TextMuted()), info);
  dl->PopClipRect();

  ImGui::InvisibleButton("##canvas", size);
  const bool over = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (over) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float pin_r = (graphcanvas::kPinR + 4.0f) * z;
    int pin_node = -999;
    bool pin_out = false;
    auto consider_pin = [&](int node, ImVec2 p, bool output) {
      if (std::hypot(mouse.x - p.x, mouse.y - p.y) <= pin_r) {
        pin_node = node;
        pin_out = output;
      }
    };
    consider_pin(-2, out_pin(card_at("##entry", -1)), true);
    consider_pin(-3, out_pin(card_at("##any", -1)), true);
    int hit = -1;
    for (int i = static_cast<int>(layer.states.size()) - 1; i >= 0; --i) {
      const ImVec2 a = card_at(layer.states[static_cast<std::size_t>(i)].name, i);
      consider_pin(i, in_pin(a), false);
      consider_pin(i, out_pin(a), true);
      if (mouse.x >= a.x && mouse.x <= a.x + nw && mouse.y >= a.y && mouse.y <= a.y + nh) hit = i;
    }
    auto name_of_node = [&](int node) -> std::string {
      if (node == -2) return "##entry";
      if (node == -3) return "##any";
      if (node >= 0 && node < static_cast<int>(layer.states.size()))
        return layer.states[static_cast<std::size_t>(node)].name;
      return {};
    };
    auto finish_link = [&](const std::string& to_name) {
      if (anim_graph_.link_from.empty() || anim_graph_.link_from == to_name) return;
      if (anim_graph_.link_from == "##entry") {
        layer.entry = to_name;
        anim_graph_.selected = to_name;
        anim_graph_.link_from.clear();
        what = "Entry state";
        touch(true);
        return;
      }
      scene3d::AnimTransition3D tr;
      tr.from = anim_graph_.link_from == "##any" ? "*" : anim_graph_.link_from;
      tr.to = to_name;
      tr.duration = 0.2f;
      const scene3d::AnimParam3D* speed = nullptr;
      for (const scene3d::AnimParam3D& p : anim.params)
        if (p.name == "speed") speed = &p;
      const scene3d::AnimParam3D* use = speed ? speed : (anim.params.empty() ? nullptr : &anim.params.front());
      if (!use) {
        tr.op = scene3d::AnimTransition3D::Op::Always;
      } else {
        tr.param = use->name;
        if (use->type == scene3d::AnimParam3D::Type::Trigger) tr.op = scene3d::AnimTransition3D::Op::Trigger;
        else if (use->type == scene3d::AnimParam3D::Type::Bool) tr.op = scene3d::AnimTransition3D::Op::True;
        else {
          tr.op = scene3d::AnimTransition3D::Op::Greater;
          tr.value = speed ? 0.1f : 0.5f;
        }
      }
      layer.transitions.push_back(std::move(tr));
      anim_graph_.selected_tr = static_cast<int>(layer.transitions.size()) - 1;
      anim_graph_.link_from.clear();
      anim_graph_.selected = to_name;
      what = "Add transition";
      touch(true);
    };
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (pin_node != -999 && pin_out) {
        anim_graph_.link_from = name_of_node(pin_node);
        anim_graph_.dragging = -1;
      } else if (pin_node >= 0 && !pin_out && !anim_graph_.link_from.empty()) {
        finish_link(name_of_node(pin_node));
      } else if (hit >= 0) {
        const std::string name = layer.states[static_cast<std::size_t>(hit)].name;
        if (!anim_graph_.link_from.empty() && anim_graph_.link_from != name) {
          finish_link(name);
        } else {
          anim_graph_.selected = name;
          anim_graph_.selected_tr = -1;
          anim_graph_.dragging = hit;
          const ImVec2 a = card_at(name, hit);
          anim_graph_.grab_x = (mouse.x - a.x) / z;
          anim_graph_.grab_y = (mouse.y - a.y) / z;
        }
      } else {
        int special = 0;
        auto on_special = [&](const char* key) {
          const ImVec2 a = card_at(key, -1);
          return mouse.x >= a.x && mouse.x <= a.x + nw && mouse.y >= a.y && mouse.y <= a.y + nh;
        };
        if (on_special("##entry")) special = -2;
        else if (on_special("##any")) special = -3;
        if (special != 0) {
          anim_graph_.dragging = special;
          const char* key = special == -2 ? "##entry" : "##any";
          const ImVec2 a = card_at(key, -1);
          anim_graph_.grab_x = (mouse.x - a.x) / z;
          anim_graph_.grab_y = (mouse.y - a.y) / z;
        } else {
        int wire = -1;
        float best = 18.0f;
        for (std::size_t ti = 0; ti < layer.transitions.size(); ++ti) {
          const scene3d::AnimTransition3D& tr = layer.transitions[ti];
          const int to = index_of(tr.to);
          if (to < 0) continue;
          const ImVec2 a = source_of(tr.from);
          const ImVec2 b = in_pin(card_at(tr.to, to));
          const float d = std::hypot(mouse.x - (a.x + b.x) * 0.5f, mouse.y - (a.y + b.y) * 0.5f);
          if (d < best) {
            best = d;
            wire = static_cast<int>(ti);
          }
        }
        if (wire >= 0) {
          anim_graph_.selected_tr = wire;
          anim_graph_.selected = layer.transitions[static_cast<std::size_t>(wire)].to;
          anim_graph_.link_from.clear();
        } else if (anim_graph_.link_from.empty()) {
          anim_graph_.selected.clear();
          anim_graph_.selected_tr = -1;
          anim_graph_.dragging = -4;
        }
        }
      }
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (anim_graph_.dragging >= 0 && anim_graph_.dragging < static_cast<int>(layer.states.size())) {
        auto& p = pos_of(layer.states[static_cast<std::size_t>(anim_graph_.dragging)].name, anim_graph_.dragging);
        p.first = (mouse.x - origin.x - anim_graph_.pan_x) / z - anim_graph_.grab_x;
        p.second = (mouse.y - origin.y - anim_graph_.pan_y) / z - anim_graph_.grab_y;
      } else if (anim_graph_.dragging == -2 || anim_graph_.dragging == -3) {
        auto& p = pos_of(anim_graph_.dragging == -2 ? "##entry" : "##any", -1);
        p.first = (mouse.x - origin.x - anim_graph_.pan_x) / z - anim_graph_.grab_x;
        p.second = (mouse.y - origin.y - anim_graph_.pan_y) / z - anim_graph_.grab_y;
      } else if (anim_graph_.dragging == -4) {
        anim_graph_.pan_x += ImGui::GetIO().MouseDelta.x;
        anim_graph_.pan_y += ImGui::GetIO().MouseDelta.y;
      }
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
      anim_graph_.pan_x += ImGui::GetIO().MouseDelta.x;
      anim_graph_.pan_y += ImGui::GetIO().MouseDelta.y;
    }
    if (ImGui::GetIO().MouseWheel != 0.0f) {
      const float gx = (mouse.x - origin.x - anim_graph_.pan_x) / z;
      const float gy = (mouse.y - origin.y - anim_graph_.pan_y) / z;
      anim_graph_.zoom = std::clamp(anim_graph_.zoom * (ImGui::GetIO().MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.4f, 1.8f);
      anim_graph_.pan_x = mouse.x - origin.x - gx * anim_graph_.zoom;
      anim_graph_.pan_y = mouse.y - origin.y - gy * anim_graph_.zoom;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##anim_palette");
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && anim_graph_.dragging != -1) anim_graph_.dragging = -1;
  if (ImGui::BeginPopup("##anim_palette")) {
    if (ImGui::MenuItem("Add state")) {
      scene3d::AnimState3D st;
      st.name = "state";
      int n = 2;
      auto taken = [&](const std::string& name) {
        for (const scene3d::AnimState3D& s : layer.states)
          if (s.name == name) return true;
        return false;
      };
      while (taken(st.name)) st.name = "state" + std::to_string(n++);
      if (model && !model->clips.empty()) st.clip = model->clips.front().name;
      if (layer.entry.empty()) layer.entry = st.name;
      anim_graph_.selected = st.name;
      layer.states.push_back(std::move(st));
      what = "Add state";
      touch(true);
    }
    if (ImGui::MenuItem("Add Speed blend")) {
      scene3d::AnimState3D st;
      st.name = "locomotion";
      int n = 2;
      auto taken = [&](const std::string& name) {
        for (const scene3d::AnimState3D& s : layer.states)
          if (s.name == name) return true;
        return false;
      };
      while (taken(st.name)) st.name = "locomotion" + std::to_string(n++);
      const char* names[3] = {"idle", "walk", "run"};
      const float at[3] = {0.0f, 1.8f, 4.5f};
      if (model) {
        for (int i = 0; i < 3; ++i)
          if (model->find_clip(names[i]) >= 0) st.blend.push_back({names[i], at[i]});
      }
      if (st.blend.empty()) {
        const std::string clip = model && !model->clips.empty() ? model->clips.front().name : std::string("clip");
        st.blend.push_back({clip, 0.0f});
        st.blend.push_back({clip, 1.8f});
        st.blend.push_back({clip, 4.5f});
      }
      st.blend_param = "speed";
      bool have = false;
      for (const scene3d::AnimParam3D& p : anim.params)
        if (p.name == "speed") have = true;
      if (!have) anim.params.push_back({"speed", scene3d::AnimParam3D::Type::Float, 0.0f});
      if (layer.entry.empty()) layer.entry = st.name;
      anim_graph_.selected = st.name;
      layer.states.push_back(std::move(st));
      what = "Add blend";
      touch(true);
    }
    ImGui::EndPopup();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  ImGui::BeginChild("##state", ImVec2(0, 0), true);
  scene3d::AnimState3D* sel = nullptr;
  for (scene3d::AnimState3D& st : layer.states)
    if (st.name == anim_graph_.selected) sel = &st;
  if (!sel) {
    ImGui::TextDisabled(linking ? "Click the state to enter." : "Select a state.");
  } else {
    ImGui::TextDisabled("State");
    if (edit_str("Name", &sel->name)) {
      anim_graph_.selected = sel->name;
      touch(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      anim_graph_.selected = sel->name;
      what = "Rename state";
      touch(true);
    }
    if (sel->blend.empty()) {
      if (clip_combo("Clip", &sel->clip, model)) {
        what = "State clip";
        touch(true);
      }
    }
    if (ImGui::Checkbox("Loop", &sel->loop)) {
      what = "State loop";
      touch(true);
    }
    ImGui::SetNextItemWidth(80);
    if (ImGui::DragFloat("Speed", &sel->speed, 0.01f, 0.0f, 5.0f, "%.2fx")) {
      what = "State speed";
      touch(false);
    }
    bool tree = !sel->blend.empty();
    if (ImGui::Checkbox("Blend tree", &tree)) {
      if (tree) {
        const float at[3] = {0.0f, 1.8f, 4.5f};
        const char* names[3] = {"idle", "walk", "run"};
        int n = 0;
        if (model) {
          for (int i = 0; i < 3; ++i) {
            if (model->find_clip(names[i]) >= 0) {
              sel->blend.push_back({names[i], at[i]});
              ++n;
            }
          }
        }
        if (n == 0) {
          const std::string clip = !sel->clip.empty() ? sel->clip
                                   : (model && !model->clips.empty() ? model->clips.front().name : std::string("clip"));
          sel->blend.push_back({clip, 0.0f});
        }
        if (sel->blend_param.empty()) sel->blend_param = "speed";
        bool have_speed = false;
        for (const scene3d::AnimParam3D& p : anim.params)
          if (p.name == "speed") have_speed = true;
        if (!have_speed)
          anim.params.push_back({"speed", scene3d::AnimParam3D::Type::Float, 0.0f});
      } else {
        if (!sel->blend.empty()) sel->clip = sel->blend.front().clip;
        sel->blend.clear();
        sel->blend_param.clear();
      }
      what = "Blend tree";
      touch(true);
    }
    if (!sel->blend.empty()) {
      std::vector<std::string> pnames;
      for (const scene3d::AnimParam3D& p : anim.params)
        if (p.type == scene3d::AnimParam3D::Type::Float) pnames.push_back(p.name);
      if (name_combo("Blend by", &sel->blend_param, pnames)) {
        what = "Blend parameter";
        touch(true);
      }
      for (std::size_t i = 0; i < sel->blend.size(); ++i) {
        ImGui::PushID(static_cast<int>(100 + i));
        if (clip_combo("##bc", &sel->blend[i].clip, model)) {
          what = "Blend point";
          touch(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(56);
        if (ImGui::DragFloat("##at", &sel->blend[i].at, 0.05f)) {
          what = "Blend point";
          touch(false);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
          sel->blend.erase(sel->blend.begin() + static_cast<std::ptrdiff_t>(i));
          what = "Blend point";
          touch(true);
          ImGui::PopID();
          break;
        }
        ImGui::PopID();
      }
      if (ImGui::SmallButton("+ Point")) {
        const std::string clip = model && !model->clips.empty() ? model->clips.front().name : sel->clip;
        const float at = sel->blend.empty() ? 0.0f : sel->blend.back().at + 1.0f;
        sel->blend.push_back({clip, at});
        what = "Blend point";
        touch(true);
      }
    }
    if (ImGui::SmallButton("Set as entry")) {
      layer.entry = sel->name;
      what = "Entry state";
      touch(true);
    }
    if (ImGui::SmallButton("Delete state")) {
      const std::string gone = sel->name;
      layer.states.erase(std::remove_if(layer.states.begin(), layer.states.end(),
                                        [&](const scene3d::AnimState3D& s) { return s.name == gone; }),
                         layer.states.end());
      layer.transitions.erase(std::remove_if(layer.transitions.begin(), layer.transitions.end(),
                                             [&](const scene3d::AnimTransition3D& t) {
                                               return t.from == gone || t.to == gone;
                                             }),
                              layer.transitions.end());
      if (layer.entry == gone) layer.entry = layer.states.empty() ? std::string() : layer.states.front().name;
      anim_graph_.selected.clear();
      anim_graph_.link_from.clear();
      sel = nullptr;
      what = "Delete state";
      touch(true);
    }
  }
  if (sel) {
    ImGui::Separator();
    ImGui::TextDisabled("Transitions");
    for (std::size_t i = 0; i < layer.transitions.size();) {
      scene3d::AnimTransition3D& tr = layer.transitions[i];
      if (tr.from != sel->name && tr.to != sel->name && tr.from != "*") {
        ++i;
        continue;
      }
      ImGui::PushID(static_cast<int>(300 + i));
      ImGui::Text("%s -> %s", tr.from.c_str(), tr.to.c_str());
      std::vector<std::string> pnames;
      for (const scene3d::AnimParam3D& p : anim.params) pnames.push_back(p.name);
      if (name_combo("When", &tr.param, pnames)) {
        what = "Transition";
        touch(true);
      }
      int op = static_cast<int>(tr.op);
      if (ImGui::Combo("Test", &op, "Greater\0Less\0True\0False\0Trigger\0Always\0")) {
        tr.op = scene3d::AnimTransition3D::Op(op);
        what = "Transition";
        touch(true);
      }
      if (tr.op == scene3d::AnimTransition3D::Op::Greater || tr.op == scene3d::AnimTransition3D::Op::Less) {
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragFloat("Value", &tr.value, 0.05f)) {
          what = "Transition";
          touch(false);
        }
      }
      ImGui::SetNextItemWidth(80);
      if (ImGui::DragFloat("Fade", &tr.duration, 0.01f, 0.0f, 5.0f, "%.2f s")) {
        what = "Transition";
        touch(false);
      }
      if (ImGui::SmallButton("Remove")) {
        layer.transitions.erase(layer.transitions.begin() + static_cast<std::ptrdiff_t>(i));
        what = "Remove transition";
        touch(true);
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
      ++i;
    }
  }
  ImGui::EndChild();
  ImGui::EndChild();
  if (anim_graph_.before && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    push_undo(what, *anim_graph_.before);
    anim_graph_.before.reset();
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
