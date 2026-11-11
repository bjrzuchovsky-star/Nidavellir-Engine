// Animation preview panel + Animator inspector.
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/ux/Tooltip.h"

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
                          const std::function<void(const char*)>& after, const char** commit) {
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
  if (ImGui::SmallButton("Reset to default locomotion")) {
    const std::string keep = a.model;
    a = scene3d::default_locomotion_animator();
    a.model = keep;
    *commit = "Default locomotion";
  }

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
  std::vector<std::string> pnames;
  for (const AnimParam3D& p : a.params) pnames.push_back(p.name);

  // Layers.
  for (std::size_t li = 0; li < a.layers.size(); ++li) {
    scene3d::AnimLayer3D& l = a.layers[li];
    ImGui::PushID(static_cast<int>(1000 + li));
    const std::string head = "Layer: " + l.name + "###layer";
    if (ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      edit_str("Name", &l.name);
      after("Layer name");
      ImGui::SliderFloat("Weight", &l.weight, 0.0f, 1.0f);
      after("Layer weight");
      std::string mask;
      for (const std::string& m : l.mask) mask += (mask.empty() ? "" : ", ") + m;
      if (edit_str("Mask bones", &mask)) {
        l.mask.clear();
        std::size_t at = 0;
        while (at <= mask.size()) {
          std::size_t c = mask.find(',', at);
          if (c == std::string::npos) c = mask.size();
          std::string b = mask.substr(at, c - at);
          b.erase(0, b.find_first_not_of(' '));
          b.erase(b.find_last_not_of(' ') + 1);
          if (!b.empty()) l.mask.push_back(b);
          at = c + 1;
        }
      }
      after("Layer mask");
      if (ImGui::IsItemHovered()) ux::SetTooltip("Each bone takes its subtree; empty = whole body");
      std::vector<std::string> snames;
      for (const scene3d::AnimState3D& st : l.states) snames.push_back(st.name);
      discrete(name_combo("Entry", &l.entry, snames), "Entry state");

      theme::SectionHeader("States");
      for (std::size_t si = 0; si < l.states.size(); ++si) {
        scene3d::AnimState3D& st = l.states[si];
        ImGui::PushID(static_cast<int>(si));
        const bool open = ImGui::TreeNode("##st", "%s%s", st.name.c_str(), st.blend.empty() ? "" : "  (blend)");
        ImGui::SameLine();
        bool removed = false;
        if (ImGui::SmallButton("x")) {
          l.states.erase(l.states.begin() + static_cast<std::ptrdiff_t>(si));
          *commit = "Remove state";
          removed = true;
        }
        if (open) {
          if (!removed) {
            edit_str("Name", &st.name);
            after("State name");
            if (st.blend.empty()) {
              discrete(clip_combo("Clip", &st.clip, model), "State clip");
            } else {
              discrete(name_combo("Blend by", &st.blend_param, pnames), "Blend param");
              for (std::size_t bi = 0; bi < st.blend.size(); ++bi) {
                ImGui::PushID(static_cast<int>(bi));
                ImGui::SetNextItemWidth(120);
                discrete(clip_combo("##bc", &st.blend[bi].clip, model), "Blend clip");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                ImGui::DragFloat("at", &st.blend[bi].at, 0.05f);
                after("Blend point");
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                  st.blend.erase(st.blend.begin() + static_cast<std::ptrdiff_t>(bi));
                  *commit = "Remove blend point";
                  ImGui::PopID();
                  break;
                }
                ImGui::PopID();
              }
            }
            if (ImGui::SmallButton(st.blend.empty() ? "Make blend space" : "+ Blend point")) {
              st.blend.push_back({st.clip, st.blend.empty() ? 0.0f : st.blend.back().at + 1.0f});
              *commit = "Add blend point";
            }
            ImGui::DragFloat("Speed", &st.speed, 0.01f, 0.0f, 5.0f, "%.2fx");
            after("State speed");
            discrete(ImGui::Checkbox("Loop", &st.loop), "State loop");
          }
          ImGui::TreePop();
        }
        ImGui::PopID();
        if (removed) break;
      }
      if (ImGui::SmallButton("+ State")) {
        scene3d::AnimState3D st;
        st.name = "state" + std::to_string(l.states.size());
        if (model && !model->clips.empty()) st.clip = model->clips.front().name;
        l.states.push_back(st);
        *commit = "Add state";
      }

      theme::SectionHeader("Transitions");
      static const char* kOps = "> value\0< value\0is true\0is false\0trigger\0always\0";
      for (std::size_t ti = 0; ti < l.transitions.size(); ++ti) {
        AnimTransition3D& tr = l.transitions[ti];
        ImGui::PushID(static_cast<int>(5000 + ti));
        ImGui::SetNextItemWidth(95);
        discrete(name_combo("##from", &tr.from, snames, "*"), "Transition from");
        ImGui::SameLine();
        ImGui::TextUnformatted("->");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(95);
        discrete(name_combo("##to", &tr.to, snames), "Transition to");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
          l.transitions.erase(l.transitions.begin() + static_cast<std::ptrdiff_t>(ti));
          *commit = "Remove transition";
          ImGui::PopID();
          break;
        }
        ImGui::Indent();
        int op = static_cast<int>(tr.op);
        if (tr.op != AnimTransition3D::Op::Always) {
          ImGui::SetNextItemWidth(95);
          discrete(name_combo("##p", &tr.param, pnames), "Transition param");
          ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##op", &op, kOps)) {
          tr.op = static_cast<AnimTransition3D::Op>(op);
          *commit = "Transition condition";
        }
        if (tr.op == AnimTransition3D::Op::Greater || tr.op == AnimTransition3D::Op::Less) {
          ImGui::SameLine();
          ImGui::SetNextItemWidth(60);
          ImGui::DragFloat("##v", &tr.value, 0.05f);
          after("Transition value");
        }
        ImGui::SetNextItemWidth(90);
        ImGui::DragFloat("Fade", &tr.duration, 0.01f, 0.0f, 5.0f, "%.2f s");
        after("Transition fade");
        ImGui::SameLine();
        discrete(ImGui::Checkbox("At end", &tr.at_end), "Transition at end");
        ImGui::Unindent();
        ImGui::PopID();
      }
      if (ImGui::SmallButton("+ Transition")) {
        AnimTransition3D tr;
        if (!snames.empty()) tr.to = snames.front();
        if (!pnames.empty()) tr.param = pnames.front();
        l.transitions.push_back(tr);
        *commit = "Add transition";
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (ImGui::SmallButton("+ Layer")) {
    scene3d::AnimLayer3D l;
    l.name = "Layer " + std::to_string(a.layers.size());
    a.layers.push_back(l);
    *commit = "Add layer";
  }
  if (a.layers.size() > 1) {
    ImGui::SameLine();
    if (ImGui::SmallButton("- Top layer")) {
      a.layers.pop_back();
      *commit = "Remove layer";
    }
  }

  // Events.
  if (ImGui::TreeNode("Events", "Events (%zu)", a.events.size())) {
    for (std::size_t i = 0; i < a.events.size(); ++i) {
      scene3d::AnimEvent3D& ev = a.events[i];
      ImGui::PushID(static_cast<int>(9000 + i));
      ImGui::SetNextItemWidth(95);
      discrete(clip_combo("##c", &ev.clip, model), "Event clip");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(60);
      ImGui::DragFloat("##t", &ev.time, 0.01f, 0.0f, 600.0f, "%.2fs");
      after("Event time");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90);
      edit_str("##n", &ev.name);
      after("Event name");
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        a.events.erase(a.events.begin() + static_cast<std::ptrdiff_t>(i));
        *commit = "Remove event";
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Event")) {
      scene3d::AnimEvent3D ev;
      ev.name = "event";
      if (model && !model->clips.empty()) ev.clip = model->clips.front().name;
      a.events.push_back(ev);
      *commit = "Add event";
    }
    ImGui::TextDisabled("Scripts get on_anim_event(name, time).");
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
  const render3d::Model* model = entity_model(*cur);
  const bool skinned = model && model->skinned();
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
                           : "Then click another state. The arrow uses the first parameter.");
  }

  scene3d::AnimLayer3D& layer = anim.layers[static_cast<std::size_t>(anim_graph_.layer)];
  const std::string layer_key = std::to_string(anim_graph_.layer) + "/";
  auto pos_of = [&](const std::string& name, int index) -> std::pair<float, float>& {
    const std::string key = layer_key + name;
    auto it = anim_graph_.pos.find(key);
    if (it == anim_graph_.pos.end()) {
      const float x = 28.0f + static_cast<float>(index % 4) * 172.0f;
      const float y = 28.0f + static_cast<float>(index / 4) * 108.0f;
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
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(22, 18, 16, 255));
  for (float g = 0; g < size.x; g += 24.0f)
    dl->AddLine(ImVec2(origin.x + g, origin.y), ImVec2(origin.x + g, origin.y + size.y), IM_COL32(40, 34, 30, 255));
  for (float g = 0; g < size.y; g += 24.0f)
    dl->AddLine(ImVec2(origin.x, origin.y + g), ImVec2(origin.x + size.x, origin.y + g), IM_COL32(40, 34, 30, 255));
  constexpr float kW = 148.0f, kH = 62.0f;
  auto box_at = [&](int index) {
    const auto& p = pos_of(layer.states[static_cast<std::size_t>(index)].name, index);
    return ImVec2(origin.x + p.first, origin.y + p.second);
  };
  auto center_of = [&](int index) {
    const ImVec2 b = box_at(index);
    return ImVec2(b.x + kW * 0.5f, b.y + kH * 0.5f);
  };
  auto index_of = [&](const std::string& name) {
    for (int i = 0; i < static_cast<int>(layer.states.size()); ++i)
      if (layer.states[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
  };
  for (const scene3d::AnimTransition3D& tr : layer.transitions) {
    const int to = index_of(tr.to);
    if (to < 0) continue;
    const int from = tr.from == "*" ? -1 : index_of(tr.from);
    const ImVec2 b = center_of(to);
    const ImVec2 a = from >= 0 ? center_of(from) : ImVec2(b.x - kW * 0.5f - 18.0f, b.y);
    dl->AddLine(a, b, ImGui::GetColorU32(theme::Accent()), 2.0f);
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
    else
      std::snprintf(label, sizeof(label), "%s %s", tr.param.c_str(), op);
    dl->AddText(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - 8.0f),
                ImGui::GetColorU32(theme::Text()), label);
  }
  for (int i = 0; i < static_cast<int>(layer.states.size()); ++i) {
    const scene3d::AnimState3D& st = layer.states[static_cast<std::size_t>(i)];
    const ImVec2 b = box_at(i);
    const bool sel = st.name == anim_graph_.selected;
    const bool entry = st.name == layer.entry;
    dl->AddRectFilled(b, ImVec2(b.x + kW, b.y + kH), IM_COL32(42, 34, 28, 255), 6.0f);
    dl->AddRect(b, ImVec2(b.x + kW, b.y + kH),
                ImGui::GetColorU32(sel || entry ? theme::Accent() : theme::Text()), 6.0f, 0, sel ? 2.5f : 1.0f);
    dl->AddText(ImVec2(b.x + 8, b.y + 6), ImGui::GetColorU32(theme::Text()), st.name.c_str());
    const std::string sub = st.blend.empty() ? st.clip : ("blend " + st.blend_param);
    ImVec4 dim = theme::Text();
    dim.w *= 0.65f;
    dl->AddText(ImVec2(b.x + 8, b.y + 26), ImGui::GetColorU32(dim), sub.c_str());
    if (entry) dl->AddText(ImVec2(b.x + 8, b.y + 44), ImGui::GetColorU32(theme::Accent()), "entry");
  }
  ImGui::InvisibleButton("##canvas", size);
  const bool over = ImGui::IsItemHovered() || ImGui::IsItemActive();
  int hit = -1;
  if (over) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int i = static_cast<int>(layer.states.size()) - 1; i >= 0; --i) {
      const ImVec2 b = box_at(i);
      if (mouse.x >= b.x && mouse.x <= b.x + kW && mouse.y >= b.y && mouse.y <= b.y + kH) {
        hit = i;
        break;
      }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (hit >= 0) {
        const std::string name = layer.states[static_cast<std::size_t>(hit)].name;
        if (!anim_graph_.link_from.empty() && anim_graph_.link_from != name) {
          scene3d::AnimTransition3D tr;
          tr.from = anim_graph_.link_from;
          tr.to = name;
          tr.duration = 0.2f;
          if (!anim.params.empty()) {
            tr.param = anim.params.front().name;
            tr.op = anim.params.front().type == scene3d::AnimParam3D::Type::Trigger
                        ? scene3d::AnimTransition3D::Op::Trigger
                        : anim.params.front().type == scene3d::AnimParam3D::Type::Bool
                              ? scene3d::AnimTransition3D::Op::True
                              : scene3d::AnimTransition3D::Op::Greater;
            tr.value = 0.5f;
          } else {
            tr.op = scene3d::AnimTransition3D::Op::Always;
          }
          layer.transitions.push_back(std::move(tr));
          anim_graph_.link_from.clear();
          anim_graph_.selected = name;
          what = "Add transition";
          touch(true);
        } else {
          anim_graph_.selected = name;
          anim_graph_.dragging = hit;
          const ImVec2 b = box_at(hit);
          anim_graph_.grab_x = mouse.x - b.x;
          anim_graph_.grab_y = mouse.y - b.y;
        }
      } else if (anim_graph_.link_from.empty()) {
        anim_graph_.selected.clear();
      }
    }
    if (anim_graph_.dragging >= 0 && anim_graph_.dragging < static_cast<int>(layer.states.size()) &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      auto& p = pos_of(layer.states[static_cast<std::size_t>(anim_graph_.dragging)].name, anim_graph_.dragging);
      p.first = std::max(8.0f, mouse.x - origin.x - anim_graph_.grab_x);
      p.second = std::max(8.0f, mouse.y - origin.y - anim_graph_.grab_y);
    }
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) anim_graph_.dragging = -1;
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
