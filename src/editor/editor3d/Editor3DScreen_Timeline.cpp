// Keyframe timeline: author skeletal clips beside a rigged model.
// The working copy is timeline_.clips; Save writes <stem>.anim3d.json and
// the next load merges it onto the imported rig (ClipFile).

#include "editor/editor3d/Editor3DScreen.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/ui/Theme.h"
#include "editor/ux/Tooltip.h"
#include "render3d/ClipFile.h"
#include "render3d/ModelImport.h"
#include "render3d/Retarget.h"
#include "store/Json.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

bool Editor3DScreen::timeline_add_event(const std::string& clip, float time, const std::string& name) {
  render3d::Clip* c = timeline_find(clip);
  if (!c || name.empty() || !std::isfinite(time)) return false;
  c->events.push_back({time, name});
  std::stable_sort(c->events.begin(), c->events.end(),
                   [](const render3d::ClipEvent& a, const render3d::ClipEvent& b) { return a.time < b.time; });
  timeline_.dirty = true;
  return true;
}

bool Editor3DScreen::timeline_erase_event(const std::string& clip, int index) {
  render3d::Clip* c = timeline_find(clip);
  if (!c || index < 0 || static_cast<std::size_t>(index) >= c->events.size()) return false;
  c->events.erase(c->events.begin() + index);
  timeline_.dirty = true;
  timeline_.drag_event = -1;
  return true;
}

namespace {

std::string avatar_file_for(const std::string& model_abs) {
  std::filesystem::path p = std::filesystem::u8path(model_abs);
  p.replace_extension(".avatar.json");
  return p.string();
}

std::vector<render3d::AvatarOverride> load_avatar(const std::string& path) {
  std::vector<render3d::AvatarOverride> out;
  store::Json j;
  std::string err;
  if (!store::read_json_file(path, &j, &err) || !j.is_object()) return out;
  for (const auto& [k, v] : j.members()) {
    if (!v.is_string() || v.as_string().empty()) continue;
    if (render3d::human_slot_from_name(k) < 0) continue;
    out.push_back({k, v.as_string()});
  }
  return out;
}

bool save_avatar(const std::string& path, const std::vector<render3d::AvatarOverride>& map) {
  store::Json j = store::Json::object();
  for (const render3d::AvatarOverride& o : map) {
    if (o.slot.empty() || o.bone.empty()) continue;
    j.set(o.slot, store::Json(o.bone));
  }
  return store::write_text_file(path, j.dump(), nullptr);
}

void bake_in_place(render3d::Clip* clip, const render3d::Skeleton& skel) {
  if (!clip) return;
  const render3d::HumanoidMap map = render3d::map_humanoid(skel);
  int hip = map.get(render3d::HumanSlot::Hips);
  if (hip < 0 && !skel.joints.empty()) hip = 0;
  if (hip < 0) return;
  const render3d::Vec3 rest = skel.joints[static_cast<std::size_t>(hip)].t;
  for (render3d::Channel& ch : clip->channels) {
    if (ch.joint != hip || ch.path != render3d::ChannelPath::Translation) continue;
    const int w = ch.width();
    const int stride = w * (ch.interp == render3d::Interp::CubicSpline ? 3 : 1);
    for (std::size_t i = 0; i + static_cast<std::size_t>(stride) <= ch.values.size(); i += static_cast<std::size_t>(stride)) {
      const std::size_t at = ch.interp == render3d::Interp::CubicSpline ? i + static_cast<std::size_t>(w) : i;
      ch.values[at] = rest.x;
      ch.values[at + 1] = rest.y;
      ch.values[at + 2] = rest.z;
    }
  }
}

std::string not_skinned_reason(const scene3d::Entity3D& e) {
  std::string why = "\"" + e.name + "\" has no skeleton, so a clip can't play on it. ";
  if (e.mesh && e.mesh->is_primitive())
    why += "A cube, sphere or other primitive is not a rigged character. ";
  else if (!e.mesh)
    why += "It has no mesh. ";
  else
    why += "This mesh was imported without bones. ";
  why += "Select a skinned character, then drop the animation on them or onto Animation in the Inspector.";
  return why;
}

}  // namespace

bool Editor3DScreen::timeline_apply_file(const std::string& path, std::string* error) {
  const scene3d::Entity3D* e = find(primary());
  if (!e) {
    if (error) *error = "Select a skinned character in the viewport first.";
    return false;
  }
  const render3d::Model* target = entity_model(*e);
  if (!target || !target->skinned()) {
    const std::string why = not_skinned_reason(*e);
    if (error) *error = why;
    note(why, runtime::LogLevel::Warn);
    return false;
  }
  if (!timeline_bind(e->id)) {
    const std::string why =
        timeline_.dirty && timeline_.loaded
            ? "This character's timeline has unsaved clips on another model. Save them in Animation Timeline, then drop the clip again."
            : "Could not open this character's clips.";
    if (error) *error = why;
    note(why, runtime::LogLevel::Warn);
    return false;
  }
  fs::path file = fs::u8path(path);
  std::error_code ec;
  if (!fs::exists(file, ec)) file = fs::path(project_.path) / fs::u8path(path);
  if (!fs::exists(file, ec)) {
    if (error) *error = "no such file: " + path;
    return false;
  }
  render3d::Model source;
  std::string err;
  if (!render3d::load_model(file.string(), &source, &err) || !source.skinned() || source.clips.empty()) {
    if (error) *error = err.empty() ? "that file has no skinned clips" : err;
    note(error ? *error : "that file has no skinned clips", runtime::LogLevel::Warn);
    return false;
  }
  render3d::RetargetReport report;
  const std::string model_abs = (fs::path(project_.path) / fs::path(entity_model_rel(*e))).string();
  const std::vector<render3d::AvatarOverride> avatar = load_avatar(avatar_file_for(model_abs));
  const std::vector<render3d::Clip> mapped = render3d::retarget_clips(
      source.skeleton, source.clips, target->skeleton, &report, avatar.empty() ? nullptr : &avatar);
  if (mapped.empty()) {
    if (error) *error = "no bones in common. " + report.detail;
    note(error ? *error : report.detail, runtime::LogLevel::Warn);
    return false;
  }
  for (const render3d::Clip& clip : mapped) {
    if (render3d::Clip* have = timeline_find(clip.name)) *have = clip;
    else timeline_.clips.push_back(clip);
  }
  timeline_.dirty = true;
  anim_preview_.clip = mapped.front().name;
  anim_preview_.time = 0.0f;
  anim_preview_.playing = false;
  anim_preview_.open = true;
  timeline_.apply_path = path;
  note("Retargeted " + report.detail);
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
  ux::BeginAutoTips("AnimationTimeline");
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
  if (model->skeleton.joints.size() > 80) {
    char filter[64];
    std::snprintf(filter, sizeof(filter), "%s", timeline_.bone_filter.c_str());
    if (ImGui::InputTextWithHint("Bones", "filter a large rig", filter, sizeof(filter))) timeline_.bone_filter = filter;
  }
  if (ImGui::BeginCombo("Bone", bone)) {
    int shown = 0;
    const bool filter = model->skeleton.joints.size() > 80;
    std::string want = timeline_.bone_filter;
    for (char& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (std::size_t i = 0; i < model->skeleton.joints.size(); ++i) {
      if (filter && static_cast<int>(i) != bone_joint_) {
        std::string n = model->skeleton.joints[i].name;
        for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (want.empty() || n.find(want) == std::string::npos) continue;
      }
      if (shown++ > 60 && static_cast<int>(i) != bone_joint_) continue;
      if (ImGui::Selectable(model->skeleton.joints[i].name.c_str(), bone_joint_ == static_cast<int>(i))) {
        bone_joint_ = static_cast<int>(i);
        timeline_.drag_key = -1;
      }
    }
    if (filter && shown > 60) ImGui::TextDisabled("Keep typing to narrow %zu bones.", model->skeleton.joints.size());
    ImGui::EndCombo();
  }
  if (model->skeleton.joints.size() > static_cast<std::size_t>(render3d::kMaxDrawnBones)) {
    ImGui::TextDisabled("Viewport draws %d of %zu bones so a large rig stays responsive.",
                        render3d::kMaxDrawnBones, model->skeleton.joints.size());
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
  theme::SectionHeader("Apply animation");
  {
    char path_buf[260];
    std::snprintf(path_buf, sizeof(path_buf), "%s", timeline_.apply_path.c_str());
    if (ImGui::InputText("Clip file", path_buf, sizeof(path_buf))) timeline_.apply_path = path_buf;
    if (ImGui::BeginCombo("Project file", timeline_.apply_path.empty() ? "(pick)" : timeline_.apply_path.c_str())) {
      for (const AssetEntry& a : assets_) {
        if (!a.model || a.rel == timeline_.model_rel) continue;
        if (ImGui::Selectable(a.rel.c_str(), a.rel == timeline_.apply_path)) timeline_.apply_path = a.rel;
      }
      ImGui::EndCombo();
    }
    if (ImGui::Button("Retarget onto character")) {
      std::string why;
      if (!timeline_apply_file(timeline_.apply_path, &why) && !why.empty()) note(why, runtime::LogLevel::Warn);
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(assets::kDragPayload)) {
        const std::string rel(static_cast<const char*>(payload->Data), payload->DataSize ? payload->DataSize - 1 : 0);
        timeline_.apply_path = rel;
        std::string why;
        timeline_apply_file(rel, &why);
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::TextWrapped(
        "Drop running.fbx here, or pick it above. Mixamo names (mixamorig:Hips, LeftArm) and this character's "
        "bones (hips, upper_arm.L) are matched, then Save writes the clip beside the model.");
  }
  if (edited) {
    theme::SectionHeader("Events");
    ImGui::InputText("Event", timeline_.event_name, sizeof(timeline_.event_name));
    ImGui::SameLine();
    if (ImGui::SmallButton("Add at playhead")) {
      timeline_add_event(edited->name, anim_preview_.time, timeline_.event_name);
    }
    for (std::size_t i = 0; i < edited->events.size(); ++i) {
      ImGui::PushID(static_cast<int>(1000 + i));
      ImGui::Text("%.2f s  %s", static_cast<double>(edited->events[i].time), edited->events[i].name.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        timeline_erase_event(edited->name, static_cast<int>(i));
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
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
    const std::size_t nkeys = sheet->times.size();
    const std::size_t step = nkeys > 2000 ? nkeys / 2000 : 1;
    for (std::size_t i = 0; i < nkeys; i += step) {
      const float x = x_of(sheet->times[i]);
      const ImVec2 a(x, (lo.y + hi.y) * 0.5f);
      dl->AddQuadFilled(ImVec2(a.x, a.y - 7), ImVec2(a.x + 7, a.y), ImVec2(a.x, a.y + 7), ImVec2(a.x - 7, a.y),
                        IM_COL32(240, 164, 72, 255));
    }
  }
  if (edited) {
    for (const render3d::ClipEvent& ev : edited->events) {
      const float x = x_of(ev.time);
      dl->AddTriangleFilled(ImVec2(x, lo.y + 2), ImVec2(x + 6, lo.y + 12), ImVec2(x - 6, lo.y + 12),
                            IM_COL32(120, 210, 255, 255));
    }
  }
  if (ImGui::IsItemActive() && span > 0.0f) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - lo.x) / (hi.x - lo.x), 0.0f, 1.0f) * span;
    if (timeline_.drag_key < 0 && timeline_.drag_event < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (edited) {
        float best = 8.0f;
        for (std::size_t i = 0; i < edited->events.size(); ++i) {
          const float d = std::fabs(x_of(edited->events[i].time) - ImGui::GetIO().MousePos.x);
          if (d < best) {
            best = d;
            timeline_.drag_event = static_cast<int>(i);
          }
        }
      }
      if (timeline_.drag_event < 0 && sheet) {
        float best = 8.0f;
        int hit = -1;
        for (std::size_t i = 0; i < sheet->times.size(); ++i) {
          const float d = std::fabs(x_of(sheet->times[i]) - ImGui::GetIO().MousePos.x);
          if (d < best) {
            best = d;
            hit = static_cast<int>(i);
          }
        }
        timeline_.drag_key = hit;
      }
    }
    if (edited && timeline_.drag_event >= 0 &&
        static_cast<std::size_t>(timeline_.drag_event) < edited->events.size()) {
      edited->events[static_cast<std::size_t>(timeline_.drag_event)].time = t;
      timeline_.dirty = true;
      anim_preview_.time = t;
      anim_preview_.playing = false;
    } else if (sheet && timeline_.drag_key >= 0) {
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
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    timeline_.drag_key = -1;
    timeline_.drag_event = -1;
  }
  ImGui::TextDisabled("Drag a diamond to move it. Click empty space to scrub. Save writes %s",
                      render3d::clip_sidecar_path(timeline_.model_rel).c_str());
  ux::EndAutoTips();
}

bool Editor3DScreen::apply_animation_asset(std::uint64_t id, const std::string& rel, std::string* error) {
  const scene3d::Entity3D* cur = find(id);
  if (!cur) {
    if (error) *error = "That object is no longer in the scene.";
    return false;
  }
  select(id);
  last_anim_target_ = id;
  if (!timeline_apply_file(rel, error)) return false;

  bool loop = true, root = false, in_place = false, play = true;
  float speed = 1.0f;
  std::string rig = "auto";
  auto read_settings = [&](const std::string& key) {
    const auto* a = asset_browser_.db().by_path(key);
    if (!a) return false;
    loop = a->meta.settings.flag("loop", true);
    root = a->meta.settings.flag("root_motion", false);
    in_place = a->meta.settings.flag("in_place", false);
    play = a->meta.settings.flag("play_on_start", true);
    speed = static_cast<float>(a->meta.settings.flt("playback_speed", 1.0));
    if (!(speed > 0.0f)) speed = 1.0f;
    rig = a->meta.settings.str("retarget_rig", "auto");
    if (rig.empty()) rig = "auto";
    return true;
  };
  if (!read_settings(rel)) {
    std::string stripped = rel;
    const std::string prefix = project_.path;
    if (stripped.rfind(prefix, 0) == 0) {
      stripped = stripped.substr(prefix.size());
      while (!stripped.empty() && (stripped[0] == '/' || stripped[0] == '\\')) stripped.erase(stripped.begin());
      read_settings(stripped);
    }
  }
  if (in_place) {
    if (const scene3d::Entity3D* e = find(id)) {
      if (const render3d::Model* m = entity_model(*e)) {
        if (render3d::Clip* c = timeline_find(anim_preview_.clip)) bake_in_place(c, m->skeleton);
      }
    }
  }
  anim_preview_.loop = loop;
  anim_preview_.in_place = in_place;
  anim_preview_.root_motion = root;
  anim_preview_.speed = speed;
  anim_preview_.playing = true;
  anim_preview_.open = true;
  anim_preview_.time = 0.0f;
  std::string save_err;
  timeline_save(&save_err);

  scene3d::Entity3D ent = *find(id);
  if (play) {
    const bool fresh = !ent.animator;
    if (fresh) {
      scene3d::Animator3D an;
      an.drive_from_player = false;
      scene3d::AnimLayer3D layer;
      layer.entry = anim_preview_.clip;
      scene3d::AnimState3D st;
      st.name = anim_preview_.clip.empty() ? "clip" : anim_preview_.clip;
      st.clip = anim_preview_.clip;
      st.loop = loop;
      layer.states = {std::move(st)};
      an.layers = {std::move(layer)};
      ent.animator = std::move(an);
    }
    if (ent.animator) {
      ent.animator->speed = speed;
      ent.animator->root_motion = root;
      if (!fresh && !ent.animator->layers.empty() && !anim_preview_.clip.empty()) {
        ent.animator->layers.front().entry = anim_preview_.clip;
      }
    }
    replace_entity(ent, "Apply animation");
  }
  const render3d::Model* posed = entity_model(*find(id));
  const std::string bones = posed ? std::to_string(posed->skeleton.joints.size()) + " bones, " : "";
  note("Playing " + fs::path(rel).filename().string() + " on " + ent.name + " (" + bones + "source rig " + rig +
       "). Preview is in the viewport; Play on start is in Animation.");
  return true;
}

bool Editor3DScreen::draw_animation_slot(scene3d::Entity3D& e, bool* touched, const char** commit) {
  auto mark = [&](const char* what) {
    if (touched) *touched = true;
    if (commit) *commit = what;
  };
  const bool open = ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen);
  if (!open) return false;
  bool replaced = false;
  const ImVec2 wide(-1.0f, 0.0f);
  if (ImGui::Button("Drop clip here", wide)) {
    note("Drag an animation from the Asset Browser onto this button, or onto the character.");
  }
  if (ImGui::BeginDragDropTarget()) {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TS_ASSET3D");
    if (!payload) payload = ImGui::AcceptDragDropPayload("TS_ASSET_ANY");
    if (!payload) payload = ImGui::AcceptDragDropPayload("TS_ASSET_IMAGE");
    if (payload && payload->Data) {
      const std::string rel(static_cast<const char*>(payload->Data));
      std::string err;
      if (apply_animation_asset(e.id, rel, &err)) {
        if (const scene3d::Entity3D* again = find(e.id)) e = *again;
        replaced = true;
      }
    }
    ImGui::EndDragDropTarget();
  }
  const render3d::Model* model = entity_model(e);
  const bool skinned = model && model->skinned();
  if (!skinned) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextWrapped("%s", not_skinned_reason(e).c_str());
    ImGui::PopTextWrapPos();
    return replaced;
  }
  ImGui::TextDisabled("%zu bones, %zu clips", model->skeleton.joints.size(), model->clips.size());

  std::vector<std::string> names;
  if (timeline_.loaded && timeline_.model_rel == entity_model_rel(e)) {
    for (const render3d::Clip& c : timeline_.clips) names.push_back(c.name);
  }
  if (names.empty()) {
    for (const render3d::Clip& c : model->clips) names.push_back(c.name);
  }
  if (anim_preview_.clip.empty() && !names.empty()) anim_preview_.clip = names.front();
  const char* preview = anim_preview_.clip.empty() ? "(none)" : anim_preview_.clip.c_str();
  if (ImGui::BeginCombo("Anim clip", preview)) {
    for (const std::string& n : names) {
      if (ImGui::Selectable(n.c_str(), n == anim_preview_.clip)) {
        anim_preview_.clip = n;
        anim_preview_.time = 0.0f;
        anim_preview_.open = true;
        anim_preview_.playing = true;
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::Button("Preview animation")) {
    anim_preview_.open = true;
    anim_preview_.playing = true;
    anim_preview_.time = 0.0f;
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Anim loop", &anim_preview_.loop)) {
    if (e.animator) {
      for (scene3d::AnimLayer3D& layer : e.animator->layers) {
        for (scene3d::AnimState3D& st : layer.states) {
          if (st.clip == anim_preview_.clip || st.name == anim_preview_.clip) st.loop = anim_preview_.loop;
        }
      }
    }
    mark("Anim loop");
  }

  float speed = e.animator ? e.animator->speed : anim_preview_.speed;
  ImGui::SetNextItemWidth(140);
  if (ImGui::DragFloat("Playback speed", &speed, 0.01f, 0.0f, 5.0f, "%.2fx")) {
    anim_preview_.speed = speed;
    if (e.animator) e.animator->speed = speed;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) mark("Playback speed");

  bool play = e.animator.has_value();
  if (ImGui::Checkbox("Play on start", &play)) {
    if (play && !e.animator) {
      scene3d::Animator3D an;
      an.speed = anim_preview_.speed;
      an.root_motion = anim_preview_.root_motion;
      an.drive_from_player = false;
      scene3d::AnimLayer3D layer;
      layer.entry = anim_preview_.clip;
      scene3d::AnimState3D st;
      st.name = anim_preview_.clip.empty() ? "clip" : anim_preview_.clip;
      st.clip = anim_preview_.clip;
      st.loop = anim_preview_.loop;
      layer.states = {std::move(st)};
      an.layers = {std::move(layer)};
      e.animator = std::move(an);
    } else if (!play) {
      e.animator.reset();
    }
    mark("Play on start");
  }
  ImGui::SameLine();
  bool root = e.animator ? e.animator->root_motion : anim_preview_.root_motion;
  if (ImGui::Checkbox("Root motion", &root)) {
    anim_preview_.root_motion = root;
    if (e.animator) e.animator->root_motion = root;
    mark("Root motion");
  }
  if (ImGui::Checkbox("In place", &anim_preview_.in_place)) {
    if (anim_preview_.in_place && timeline_.loaded && timeline_.model_rel == entity_model_rel(e)) {
      if (render3d::Clip* c = timeline_find(anim_preview_.clip)) {
        bake_in_place(c, model->skeleton);
        timeline_.dirty = true;
        timeline_save(nullptr);
      }
    }
  }

  if (ImGui::SmallButton("Locomotion template")) {
    const std::string keep = e.animator ? e.animator->model : std::string();
    e.animator = scene3d::default_locomotion_animator();
    e.animator->model = keep;
    mark("Locomotion template");
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("One click: idle, walk and run blended by speed, plus a jump. Same as Reset to default locomotion.");
  }

  if (ImGui::CollapsingHeader("Avatar")) {
    const std::string abs = (fs::path(project_.path) / fs::path(entity_model_rel(e))).string();
    const std::string file = avatar_file_for(abs);
    std::vector<render3d::AvatarOverride> map = load_avatar(file);
    auto bone_of = [&](const char* slot) -> std::string {
      for (const render3d::AvatarOverride& o : map)
        if (o.slot == slot) return o.bone;
      return {};
    };
    ImGui::TextDisabled("Auto-maps Mixamo, Unreal and Rigify. Pick a bone to override.");
    bool wrote = false;
    for (int i = 0; i < static_cast<int>(render3d::HumanSlot::Count); ++i) {
      const char* slot = render3d::human_slot_name(static_cast<render3d::HumanSlot>(i));
      if (!slot) continue;
      const std::string current = bone_of(slot);
      const std::string shown = current.empty() ? std::string("(auto)") : current;
      ImGui::PushID(i);
      if (ImGui::BeginCombo(slot, shown.c_str())) {
        if (ImGui::Selectable("(auto)", current.empty())) {
          map.erase(std::remove_if(map.begin(), map.end(),
                                   [&](const render3d::AvatarOverride& o) { return o.slot == slot; }),
                    map.end());
          wrote = true;
        }
        for (const render3d::Joint& j : model->skeleton.joints) {
          if (ImGui::Selectable(j.name.c_str(), j.name == current)) {
            bool found = false;
            for (render3d::AvatarOverride& o : map) {
              if (o.slot == slot) { o.bone = j.name; found = true; break; }
            }
            if (!found) map.push_back({slot, j.name});
            wrote = true;
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    if (wrote) {
      if (save_avatar(file, map)) note("Saved bone map to " + fs::path(file).filename().string());
      else note("Could not save the bone map", runtime::LogLevel::Warn);
    }
  }
  return replaced;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
