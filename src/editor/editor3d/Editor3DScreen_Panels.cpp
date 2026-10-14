// Inspector + Environment panels. Widgets edit a copy that is applied live;
// one undo step is pushed when a widget is released after an edit.
#include "editor/editor3d/Editor3DScreen.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

using render3d::Vec3;

namespace {

struct Tracker {
  bool activated = false;
  const char* commit = nullptr;
  void after(const char* label) {
    if (ImGui::IsItemActivated()) activated = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = label;
  }
};

bool edit_text(const char* label, std::string* s) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s", s->c_str());
  if (ImGui::InputText(label, buf, sizeof(buf))) {
    *s = buf;
    return true;
  }
  return false;
}

}  // namespace

void Editor3DScreen::draw_inspector() {
  const scene3d::Entity3D* cur = find(primary());
  if (!cur) {
    ImGui::TextDisabled("Select something in the viewport or Hierarchy.");
    return;
  }
  scene3d::Entity3D e = *cur;
  Tracker t;

  ImGui::PushID(static_cast<int>(e.id));
  edit_text("Name", &e.name);
  t.after("Rename");
  ImGui::Checkbox("Visible", &e.visible);
  t.after("Visibility");
  ImGui::SameLine();
  ImGui::Checkbox("Locked", &e.locked);
  t.after("Lock");
  ImGui::SameLine();
  ImGui::Checkbox("Hidden (editor)", &e.editor_hidden);
  t.after("Hide");
  bool unparent = false;
  if (e.parent != 0) {
    const scene3d::Entity3D* par = find(e.parent);
    ImGui::TextDisabled("Parent: %s", par ? par->name.c_str() : "?");
    ImGui::SameLine();
    unparent = ImGui::SmallButton("Unparent");
  }

  theme::SectionHeader("Transform");
  ImGui::DragFloat3("Position", &e.transform.position.x, 0.05f);
  t.after("Move");
  Vec3 euler = e.transform.rotation.to_euler_deg();
  if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f))
    e.transform.rotation = render3d::Quat::from_euler_deg(euler);
  t.after("Rotate");
  ImGui::DragFloat3("Scale", &e.transform.scale.x, 0.02f, 0.01f, 1000.0f);
  t.after("Scale");

  auto component = [&](const char* name, bool present, auto add, auto remove) {
    ImGui::Spacing();
    if (!present) {
      if (ImGui::SmallButton((std::string("+ ") + name).c_str())) {
        add();
        t.activated = true;
        t.commit = "Add component";
      }
      return false;
    }
    const bool open = ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 20);
    if (ImGui::SmallButton((std::string("x##rm") + name).c_str())) {
      remove();
      t.activated = true;
      t.commit = "Remove component";
      return false;
    }
    return open;
  };

  if (component("Mesh", e.mesh.has_value(), [&] { e.mesh = scene3d::MeshRef{}; },
                [&] { e.mesh.reset(); })) {
    std::string src = e.mesh->source;
    if (ImGui::BeginCombo("Source", src.c_str())) {
      for (const AssetEntry& a : assets_) {
        if (!a.model) continue;
        if (ImGui::Selectable(a.rel.c_str(), a.rel == src)) {
          e.mesh->source = a.rel;
          t.activated = true;
          t.commit = "Mesh";
        }
      }
      ImGui::EndCombo();
    }
    ImGui::Checkbox("Casts shadows", &e.mesh->cast_shadows);
    t.after("Shadows");
  }
  if (component("Material", e.material.has_value(), [&] { e.material = scene3d::Material3D{}; },
                [&] { e.material.reset(); })) {
    scene3d::Material3D& m = *e.material;
    ImGui::ColorEdit4("Albedo", m.albedo);
    t.after("Albedo");
    ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f);
    t.after("Metallic");
    ImGui::SliderFloat("Roughness", &m.roughness, 0.04f, 1.0f);
    t.after("Roughness");
    ImGui::ColorEdit3("Emissive", m.emissive, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    t.after("Emissive");
    auto tex = [&](const char* label, std::string* path) {
      if (ImGui::BeginCombo(label, path->empty() ? "(none)" : path->c_str())) {
        if (ImGui::Selectable("(none)", path->empty())) {
          path->clear();
          t.activated = true;
          t.commit = "Texture";
        }
        for (const AssetEntry& a : assets_) {
          if (!a.image) continue;
          if (ImGui::Selectable(a.rel.c_str(), a.rel == *path)) {
            *path = a.rel;
            t.activated = true;
            t.commit = "Texture";
          }
        }
        ImGui::EndCombo();
      }
    };
    tex("Albedo map", &m.albedo_texture);
    tex("Normal map", &m.normal_texture);
    tex("Metal/rough map", &m.metal_rough_texture);
    ImGui::Checkbox("Override model materials", &m.override_model);
    t.after("Override");
  }
  if (component("Light", e.light.has_value(), [&] { e.light = scene3d::Light3D{}; },
                [&] { e.light.reset(); })) {
    scene3d::Light3D& l = *e.light;
    int type = l.type == scene3d::Light3D::Type::Point ? 0 : 1;
    if (ImGui::Combo("Type", &type, "Point\0Directional (sun)\0")) {
      l.type = type == 0 ? scene3d::Light3D::Type::Point : scene3d::Light3D::Type::Directional;
      t.activated = true;
      t.commit = "Light type";
    }
    ImGui::ColorEdit3("Color", l.color);
    t.after("Light color");
    ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0.0f, 100.0f);
    t.after("Intensity");
    if (l.type == scene3d::Light3D::Type::Point) {
      ImGui::DragFloat("Range", &l.range, 0.1f, 0.1f, 200.0f);
      t.after("Range");
    }
  }
  if (component("Collider", e.collider.has_value(), [&] { e.collider = scene3d::Collider3D{}; },
                [&] { e.collider.reset(); })) {
    scene3d::Collider3D& c = *e.collider;
    int shape = c.shape == scene3d::Collider3D::Shape::Box ? 0 : 1;
    if (ImGui::Combo("Shape", &shape, "Box\0Capsule\0")) {
      c.shape = shape == 0 ? scene3d::Collider3D::Shape::Box : scene3d::Collider3D::Shape::Capsule;
      t.activated = true;
      t.commit = "Collider shape";
    }
    ImGui::DragFloat3(shape == 0 ? "Size" : "Radius/Height/-", &c.size.x, 0.02f, 0.01f, 500.0f);
    t.after("Collider size");
    ImGui::DragFloat3("Offset", &c.offset.x, 0.02f);
    t.after("Collider offset");
    ImGui::Checkbox("Trigger", &c.trigger);
    t.after("Trigger");
  }
  if (component("Player (rider)", e.player.has_value(), [&] { e.player = scene3d::Player3D{}; },
                [&] { e.player.reset(); })) {
    scene3d::Player3D& p = *e.player;
    ImGui::DragFloat("Speed", &p.speed, 0.05f, 0.1f, 50.0f);
    t.after("Speed");
    ImGui::DragFloat("Jump", &p.jump_speed, 0.05f, 0.0f, 50.0f);
    t.after("Jump");
    ImGui::DragFloat("Cam distance", &p.camera_distance, 0.05f, 1.0f, 50.0f);
    t.after("Camera");
    ImGui::DragFloat("Cam height", &p.camera_height, 0.05f, 0.0f, 50.0f);
    t.after("Camera");
  }
  if (component("VR Rig", e.vr_rig.has_value(), [&] { e.vr_rig = scene3d::VrRig3D{}; },
                [&] { e.vr_rig.reset(); })) {
    scene3d::VrRig3D& r = *e.vr_rig;
    int loco = static_cast<int>(r.locomotion);
    if (ImGui::Combo("Locomotion", &loco, "Teleport only\0Smooth only\0Teleport + smooth\0")) {
      r.locomotion = static_cast<scene3d::VrRig3D::Locomotion>(loco);
      t.activated = true;
      t.commit = "Locomotion";
    }
    ImGui::DragFloat("Move speed", &r.move_speed, 0.05f, 0.1f, 20.0f, "%.1f m/s");
    t.after("Move speed");
    ImGui::DragFloat("Snap turn", &r.snap_turn_deg, 1.0f, 0.0f, 180.0f, "%.0f deg (0 = smooth)");
    t.after("Snap turn");
    ImGui::DragFloat("Smooth turn", &r.smooth_turn_speed, 1.0f, 10.0f, 720.0f, "%.0f deg/s");
    t.after("Smooth turn");
    ImGui::DragFloat("Teleport range", &r.teleport_range, 0.1f, 1.0f, 50.0f, "%.1f m");
    t.after("Teleport range");
    ImGui::Checkbox("Comfort vignette", &r.vignette);
    t.after("Vignette");
    ImGui::Checkbox("Seated", &r.seated);
    t.after("Seated");
    ImGui::DragFloat("Seated lift", &r.seated_offset, 0.01f, 0.0f, 1.5f, "%.2f m");
    t.after("Seated lift");
  }
  if (component("Grabbable", e.grabbable.has_value(), [&] { e.grabbable = scene3d::Grabbable3D{}; },
                [&] { e.grabbable.reset(); })) {
    scene3d::Grabbable3D& g = *e.grabbable;
    ImGui::Checkbox("Throwable", &g.throwable);
    t.after("Throwable");
    ImGui::Checkbox("Snap to hand", &g.snap_to_hand);
    t.after("Snap to hand");
    ImGui::DragFloat("Throw scale", &g.throw_scale, 0.01f, 0.0f, 5.0f, "%.2fx");
    t.after("Throw scale");
  }
  if (component("Script", e.script.has_value(), [&] { e.script = ScriptData{}; },
                [&] { e.script.reset(); })) {
    if (ImGui::BeginCombo("Lua", e.script->path.empty() ? "(none)" : e.script->path.c_str())) {
      for (const std::string& s : scripts_) {
        if (ImGui::Selectable(s.c_str(), s == e.script->path)) {
          e.script->path = s;
          t.activated = true;
          t.commit = "Script";
        }
      }
      ImGui::EndCombo();
    }
    ImGui::TextDisabled("%zu prop override(s)", e.script->props.size());
  }
  ImGui::PopID();

  if (t.activated && !inspector_before_) inspector_before_ = scene_;
  if (scene3d::Entity3D* live = scene_.find(e.id)) {
    if (!(*live == e)) {
      const render3d::Transform prev = live->transform;
      *live = e;
      carry_descendants(e.id, prev);  // children ride along
    }
  }
  if (t.commit && inspector_before_) {
    const scene3d::Scene3D before = std::move(*inspector_before_);
    inspector_before_.reset();
    if (!(before == scene_)) push_undo(std::string(t.commit) + " " + e.name, before);
  }
  if (unparent && !inspector_before_) set_parent(e.id, 0);
}

void Editor3DScreen::draw_environment() {
  render3d::Environment env = scene_.environment;
  Tracker t;
  ImGui::ColorEdit3("Sky top", env.sky_top);
  t.after("Sky");
  ImGui::ColorEdit3("Horizon", env.sky_horizon);
  t.after("Sky");
  ImGui::ColorEdit3("Ground bounce", env.ground);
  t.after("Ground");
  ImGui::SliderFloat("Ambient", &env.ambient, 0.0f, 2.0f);
  t.after("Ambient");
  theme::SectionHeader("Sun");
  ImGui::DragFloat3("Direction", &env.sun_direction.x, 0.01f, -1.0f, 1.0f);
  t.after("Sun direction");
  ImGui::ColorEdit3("Sun color", env.sun_color);
  t.after("Sun color");
  ImGui::DragFloat("Sun intensity", &env.sun_intensity, 0.02f, 0.0f, 20.0f);
  t.after("Sun intensity");
  ImGui::Checkbox("Shadows", &env.shadows);
  t.after("Shadows");
  ImGui::DragFloat("Shadow distance", &env.shadow_distance, 0.5f, 5.0f, 500.0f);
  t.after("Shadow distance");

  if (t.activated && !inspector_before_) inspector_before_ = scene_;
  scene_.environment = env;
  if (t.commit && inspector_before_) {
    const scene3d::Scene3D before = std::move(*inspector_before_);
    inspector_before_.reset();
    if (!(before == scene_)) push_undo(t.commit, before);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
