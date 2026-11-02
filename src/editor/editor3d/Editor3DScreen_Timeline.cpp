// Keyframe timeline: author skeletal clips beside a rigged model.
// The working copy is timeline_.clips; Save writes <stem>.anim3d.json and
// the next load merges it onto the imported rig (ClipFile).

#include "editor/editor3d/Editor3DScreen.h"

#include "editor/ui/Theme.h"
#include "render3d/ClipFile.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

const char* path_label(render3d::ChannelPath path) {
  switch (path) {
    case render3d::ChannelPath::Rotation: return "Rotation";
    case render3d::ChannelPath::Scale: return "Scale";
    default: return "Translation";
  }
}

render3d::Pose pose_of(const render3d::Model& model, const render3d::Clip* clip, float time) {
  render3d::Pose pose = render3d::rest_pose(model.skeleton);
  if (clip) render3d::sample_clip(*clip, time, false, &pose);
  return pose;
}

}  // namespace

std::string Editor3DScreen::entity_model_rel(const scene3d::Entity3D& e) const {
  if (e.animator && !e.animator->model.empty()) return e.animator->model;
  if (e.mesh && !e.mesh->is_primitive()) return e.mesh->source;
  return {};
}

render3d::Clip* Editor3DScreen::timeline_find(const std::string& name) {
  for (render3d::Clip& c : timeline_.clips) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

const render3d::Clip* Editor3DScreen::authored_clip(const std::string& name) const {
  for (const render3d::Clip& c : timeline_.clips) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

bool Editor3DScreen::timeline_bind(std::uint64_t entity) {
  const scene3d::Entity3D* e = find(entity);
  if (!e) return false;
  const render3d::Model* model = entity_model(*e);
  if (!model || !model->skinned()) return false;
  const std::string rel = entity_model_rel(*e);
  if (rel.empty()) return false;
  if (timeline_.loaded && timeline_.model_rel == rel) return true;
  if (timeline_.dirty && timeline_.loaded) return false;
  std::vector<render3d::Clip> clips;
  std::string err;
  const std::string abs = (fs::path(project_.path) / fs::path(rel)).string();
  if (!render3d::load_clip_file(render3d::clip_sidecar_path(abs), model->skeleton, &clips, &err)) {
    note(err.empty() ? "Could not read the clip file" : err, runtime::LogLevel::Warn);
    return false;
  }
  timeline_.clips = std::move(clips);
  timeline_.model_rel = rel;
  timeline_.loaded = true;
  timeline_.dirty = false;
  timeline_.drag_key = -1;
  if (!err.empty()) note(rel + ": " + err, runtime::LogLevel::Warn);
  return true;
}

bool Editor3DScreen::timeline_new_clip(const std::string& name, const std::string& copy_from) {
  if (name.empty() || timeline_find(name)) return false;
  const scene3d::Entity3D* e = find(primary());
  if (!e || !timeline_bind(e->id)) return false;
  const render3d::Model* model = entity_model(*e);
  if (!model) return false;
  render3d::Clip clip;
  clip.name = name;
  if (!copy_from.empty()) {
    if (const render3d::Clip* src = authored_clip(copy_from)) {
      clip = *src;
    } else {
      const int i = model->find_clip(copy_from);
      if (i < 0) return false;
      clip = model->clips[static_cast<std::size_t>(i)];
    }
    clip.name = name;
  }
  timeline_.clips.push_back(std::move(clip));
  timeline_.dirty = true;
  anim_preview_.clip = name;
  anim_preview_.time = 0.0f;
  anim_preview_.playing = false;
  return true;
}

bool Editor3DScreen::timeline_key_path(const std::string& clip, int joint, float time,
                                      render3d::ChannelPath path, const float* value) {
  render3d::Clip* c = timeline_find(clip);
  if (!c || !value || joint < 0) return false;
  render3d::Channel* ch = render3d::ensure_channel(c, joint, path, render3d::Interp::Linear);
  if (!ch || render3d::upsert_key(ch, time, value) < 0) return false;
  render3d::clip_duration(c);
  timeline_.dirty = true;
  return true;
}

bool Editor3DScreen::timeline_key(const std::string& clip, int joint, float time,
                                 const render3d::JointPose& pose) {
  float v[4];
  render3d::pose_channel_value(pose, render3d::ChannelPath::Translation, v);
  if (!timeline_key_path(clip, joint, time, render3d::ChannelPath::Translation, v)) return false;
  render3d::pose_channel_value(pose, render3d::ChannelPath::Rotation, v);
  if (!timeline_key_path(clip, joint, time, render3d::ChannelPath::Rotation, v)) return false;
  render3d::pose_channel_value(pose, render3d::ChannelPath::Scale, v);
  return timeline_key_path(clip, joint, time, render3d::ChannelPath::Scale, v);
}

bool Editor3DScreen::timeline_erase_key(const std::string& clip, int joint, render3d::ChannelPath path,
                                       int index) {
  render3d::Clip* c = timeline_find(clip);
  if (!c) return false;
  render3d::Channel* ch = render3d::ensure_channel(c, joint, path, render3d::Interp::Linear);
  if (!ch || !render3d::erase_key(ch, index)) return false;
  render3d::clip_duration(c);
  timeline_.dirty = true;
  timeline_.drag_key = -1;
  return true;
}

bool Editor3DScreen::timeline_move_key(const std::string& clip, int joint, render3d::ChannelPath path,
                                      int index, float time) {
  render3d::Clip* c = timeline_find(clip);
  if (!c) return false;
  render3d::Channel* ch = render3d::ensure_channel(c, joint, path, render3d::Interp::Linear);
  if (!ch || render3d::move_key(ch, index, time) < 0) return false;
  render3d::clip_duration(c);
  timeline_.dirty = true;
  return true;
}

bool Editor3DScreen::timeline_set_interp(const std::string& clip, int joint, render3d::ChannelPath path,
                                        render3d::Interp interp) {
  render3d::Clip* c = timeline_find(clip);
  if (!c) return false;
  render3d::Channel* ch = render3d::ensure_channel(c, joint, path, interp);
  if (!ch) return false;
  render3d::set_channel_interp(ch, interp);
  timeline_.dirty = true;
  return true;
}

bool Editor3DScreen::timeline_save(std::string* error) {
  const scene3d::Entity3D* e = find(primary());
  if (!e || !timeline_.loaded) {
    if (error) *error = "no clip is open";
    return false;
  }
  const render3d::Model* model = entity_model(*e);
  if (!model || entity_model_rel(*e) != timeline_.model_rel) {
    if (error) *error = "the selection is not the model these clips belong to";
    return false;
  }
  const std::string abs = (fs::path(project_.path) / fs::path(timeline_.model_rel)).string();
  std::string err;
  if (!render3d::save_clip_file(render3d::clip_sidecar_path(abs), model->skeleton, timeline_.clips, &err)) {
    if (error) *error = err;
    note(err.empty() ? "Could not save clips" : err, runtime::LogLevel::Warn);
    return false;
  }
  timeline_.dirty = false;
  if (view_) view_->clear();
  note("Saved clips for " + timeline_.model_rel);
  return true;
}

void Editor3DScreen::draw_anim_timeline() {
  const scene3d::Entity3D* e = find(primary());
  if (!e) {
    ImGui::TextDisabled("Select a skinned mesh.");
    return;
  }
  if (!timeline_bind(e->id)) {
    ImGui::TextDisabled("\"%s\" has no skeleton, or another model's clips are still unsaved.", e->name.c_str());
    return;
  }
  const render3d::Model* model = entity_model(*e);
  if (!model) return;
  ImGui::Text("%s", timeline_.model_rel.c_str());
  if (timeline_.dirty) {
    ImGui::SameLine();
    ImGui::TextDisabled("(unsaved)");
  }
  const std::string current = anim_preview_.clip;
  if (ImGui::BeginCombo("Clip", current.empty() ? "(none)" : current.c_str())) {
    for (const render3d::Clip& c : model->clips) {
      const bool authored = authored_clip(c.name) != nullptr;
      const std::string label = c.name + (authored ? "  (edit)" : "  (file)");
      if (ImGui::Selectable(label.c_str(), c.name == current)) {
        anim_preview_.clip = c.name;
        anim_preview_.time = 0.0f;
        anim_preview_.playing = false;
      }
    }
    for (const render3d::Clip& c : timeline_.clips) {
      if (model->find_clip(c.name) >= 0) continue;
      if (ImGui::Selectable((c.name + "  (new)").c_str(), c.name == current)) {
        anim_preview_.clip = c.name;
        anim_preview_.time = 0.0f;
        anim_preview_.playing = false;
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::SmallButton("New clip")) {
    std::string name = "clip";
    int n = 1;
    while (timeline_find(name) || model->find_clip(name) >= 0) name = "clip" + std::to_string(++n);
    timeline_new_clip(name);
  }
  ImGui::SameLine();
  const bool imported = !current.empty() && !authored_clip(current) && model->find_clip(current) >= 0;
  ImGui::BeginDisabled(!imported);
  if (ImGui::SmallButton("Edit a copy")) timeline_new_clip(current + "_edit", current);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!timeline_.dirty);
  if (ImGui::SmallButton("Save")) timeline_save();
  ImGui::EndDisabled();

  const char* bone = bone_joint_ >= 0 && static_cast<std::size_t>(bone_joint_) < model->skeleton.joints.size()
                         ? model->skeleton.joints[static_cast<std::size_t>(bone_joint_)].name.c_str()
                         : model->skeleton.joints.front().name.c_str();
  if (bone_joint_ < 0 && !model->skeleton.joints.empty()) bone_joint_ = 0;
  if (ImGui::BeginCombo("Bone", bone)) {
    for (std::size_t i = 0; i < model->skeleton.joints.size(); ++i) {
      if (ImGui::Selectable(model->skeleton.joints[i].name.c_str(), bone_joint_ == static_cast<int>(i))) {
        bone_joint_ = static_cast<int>(i);
        timeline_.drag_key = -1;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Combo("Channel", &timeline_.path, "Translation\0Rotation\0Scale\0");
  const auto path = static_cast<render3d::ChannelPath>(timeline_.path);
  render3d::Clip* edited = timeline_find(current);
  if (!edited && imported) {
    ImGui::TextDisabled("This clip is in the file. Edit a copy to key it.");
  }
  const render3d::Clip* sampled = edited ? edited : nullptr;
  if (!sampled && model->find_clip(current) >= 0) {
    sampled = &model->clips[static_cast<std::size_t>(model->find_clip(current))];
  }
  const float span = std::max(1.0f, std::max(anim_preview_.time, sampled ? sampled->duration : 0.0f));
  render3d::Pose pose = pose_of(*model, sampled, anim_preview_.time);
  const int joint = std::clamp(bone_joint_, 0, static_cast<int>(pose.size()) - 1);
  render3d::JointPose jp = pose[static_cast<std::size_t>(joint)];

  bool changed = false;
  if (path == render3d::ChannelPath::Rotation) {
    render3d::Vec3 euler = jp.r.to_euler_deg();
    changed = ImGui::DragFloat3("Euler", &euler.x, 1.0f);
    if (changed) jp.r = render3d::Quat::from_euler_deg(euler);
  } else if (path == render3d::ChannelPath::Scale) {
    changed = ImGui::DragFloat3("Scale", &jp.s.x, 0.01f, 0.01f, 100.0f);
  } else {
    changed = ImGui::DragFloat3("Position", &jp.t.x, 0.01f);
  }
  if (changed && edited) {
    float v[4];
    render3d::pose_channel_value(jp, path, v);
    timeline_key_path(edited->name, joint, anim_preview_.time, path, v);
    anim_preview_.playing = false;
  }
  int interp = 0;
  if (edited) {
    for (const render3d::Channel& ch : edited->channels) {
      if (ch.joint == joint && ch.path == path) interp = static_cast<int>(ch.interp);
    }
  }
  ImGui::BeginDisabled(edited == nullptr);
  if (ImGui::Combo("Interpolation", &interp, "Linear\0Step\0Cubic\0")) {
    timeline_set_interp(current, joint, path, static_cast<render3d::Interp>(interp));
  }
  if (ImGui::SmallButton("Key pose")) {
    timeline_key(current, joint, anim_preview_.time, jp);
    anim_preview_.playing = false;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Delete key")) {
    const render3d::Channel* ch = nullptr;
    if (edited) {
      for (const render3d::Channel& c : edited->channels) {
        if (c.joint == joint && c.path == path) ch = &c;
      }
    }
    int nearest = -1;
    float best = 1e9f;
    if (ch) {
      for (std::size_t i = 0; i < ch->times.size(); ++i) {
        const float d = std::fabs(ch->times[i] - anim_preview_.time);
        if (d < best) {
          best = d;
          nearest = static_cast<int>(i);
        }
      }
    }
    if (nearest >= 0 && best <= 0.05f) timeline_erase_key(current, joint, path, nearest);
  }
  ImGui::EndDisabled();

  // Dopesheet: diamonds on the selected channel. Drag moves a key; a click scrubs.
  render3d::Channel* sheet = nullptr;
  if (edited) {
    for (render3d::Channel& c : edited->channels) {
      if (c.joint == joint && c.path == path) sheet = &c;
    }
  }
  ImGui::TextDisabled("%s  %.3f s", path_label(path), static_cast<double>(anim_preview_.time));
  const float width = std::max(40.0f, ImGui::GetContentRegionAvail().x);
  ImGui::InvisibleButton("##dopesheet", ImVec2(width, 36.0f));
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(lo, hi, IM_COL32(32, 26, 22, 255));
  auto x_of = [&](float t) { return lo.x + (hi.x - lo.x) * std::clamp(t / span, 0.0f, 1.0f); };
  const float play_x = x_of(anim_preview_.time);
  dl->AddLine(ImVec2(play_x, lo.y), ImVec2(play_x, hi.y), IM_COL32(232, 196, 122, 255), 1.5f);
  if (sheet) {
    for (std::size_t i = 0; i < sheet->times.size(); ++i) {
      const float x = x_of(sheet->times[i]);
      const ImVec2 a(x, (lo.y + hi.y) * 0.5f);
      dl->AddQuadFilled(ImVec2(a.x, a.y - 7), ImVec2(a.x + 7, a.y), ImVec2(a.x, a.y + 7), ImVec2(a.x - 7, a.y),
                        IM_COL32(240, 164, 72, 255));
    }
  }
  if (ImGui::IsItemActive() && span > 0.0f) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - lo.x) / (hi.x - lo.x), 0.0f, 1.0f) * span;
    int hit = -1;
    if (sheet && timeline_.drag_key < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      float best = 8.0f;
      for (std::size_t i = 0; i < sheet->times.size(); ++i) {
        const float d = std::fabs(x_of(sheet->times[i]) - ImGui::GetIO().MousePos.x);
        if (d < best) {
          best = d;
          hit = static_cast<int>(i);
        }
      }
      timeline_.drag_key = hit;
    }
    if (sheet && timeline_.drag_key >= 0) {
      const int moved = render3d::move_key(sheet, timeline_.drag_key, t);
      if (moved < 0) timeline_.drag_key = -1;
      else {
        timeline_.drag_key = moved;
        render3d::clip_duration(edited);
        timeline_.dirty = true;
        anim_preview_.time = t;
        anim_preview_.playing = false;
      }
    } else {
      anim_preview_.time = t;
      anim_preview_.playing = false;
    }
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) timeline_.drag_key = -1;
  ImGui::TextDisabled("Drag a diamond to move it. Click empty space to scrub. Save writes %s",
                      render3d::clip_sidecar_path(timeline_.model_rel).c_str());
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
