// Inspector + Environment panels. Widgets edit a copy that is applied live;
// one undo step is pushed when a widget is released after an edit.
#include "render3d/ColliderFit.h"
#include "editor/fx/FxInspector.h"
#include "editor/editor3d/Editor3DScreen.h"

#include "editor/audio/EditorAudio.h"
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

  if (e.prefab) {
    draw_prefab_inspector(e);
    // An Apply / Revert above may have replaced the entity.
    if (const scene3d::Entity3D* again = find(primary())) e = *again; else return;
  }
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
    using S = scene3d::ColliderShape3D;
    scene3d::Collider3D& c = *e.collider;
    const scene3d::Collider3D c_before = c;
    bool fitted_now = false;
    int shape = static_cast<int>(c.shape);
    if (ImGui::Combo("Shape", &shape, "Box\0Capsule\0Sphere\0Convex hull (model)\0Mesh (model, static)\0")) {
      c.shape = static_cast<S>(shape);
      t.activated = true;
      t.commit = "Collider shape";
    }
    if (c.shape == S::Box) {
      ImGui::DragFloat3("Size", &c.size.x, 0.02f, 0.01f, 500.0f);
      t.after("Collider size");
    } else if (c.shape == S::Capsule || c.shape == S::Sphere) {
      ImGui::DragFloat("Radius", &c.size.x, 0.01f, 0.01f, 100.0f, "%.2f m");
      t.after("Collider radius");
      if (c.shape == S::Capsule) {
        ImGui::DragFloat("Height", &c.size.y, 0.02f, 0.02f, 200.0f, "%.2f m");
        t.after("Collider height");
      }
    } else {
      ImGui::TextDisabled(c.shape == S::Mesh ? "Triangles of the model (static / kinematic only)"
                                             : "Convex hull of the model's vertices");
    }
    ImGui::DragFloat3("Offset", &c.offset.x, 0.02f);
    t.after("Collider offset");
    if (e.mesh && !e.mesh->is_primitive()) {
      // Fit to the model: what is drawn (rest pose, import scale), not the
      // raw file vertices. Fitted colliders follow model reimports.
      static const char* fits[] = {"auto", "box", "capsule", "convex", "mesh"};
      static const char* labels = "Auto\0Tight box\0Capsule\0Convex hull\0Mesh\0";
      int fi = 0;
      for (int k = 0; k < 5; ++k) {
        if (c.fit == fits[k]) fi = k;
      }
      ImGui::SetNextItemWidth(120);
      ImGui::Combo("##fitmode", &fi, labels);
      ImGui::SameLine();
      if (ImGui::Button("Fit to mesh") && view_) {
        if (const render3d::Model* m = view_->model(e.mesh->source);
            m && render3d::fit_collider(*m, render3d::collider_fit_from(fits[fi]), &c)) {
          t.activated = true;
          t.commit = "Fit collider to mesh";
          fitted_now = true;
        }
      }
      ImGui::SameLine();
      ImGui::TextDisabled(c.fit.empty() ? "(manual)" : "(fitted: %s)", c.fit.c_str());
    }
    // Hand edits make it a manual collider (reimports leave it alone).
    if (!fitted_now && !c.fit.empty() && c.fit == c_before.fit &&
        (c.shape != c_before.shape || !(c.size == c_before.size) || !(c.offset == c_before.offset))) {
      c.fit.clear();
    }
    ImGui::Checkbox("Trigger", &c.trigger);
    t.after("Trigger");
    // Layer: -1 = by role (static / default / held / player ...).
    const scene3d::PhysicsSettings3D& ps = scene_.physics;
    const std::string cur_layer = c.layer < 0 ? "By role" : ps.layer_names[c.layer & 7];
    if (ImGui::BeginCombo("Layer", cur_layer.c_str())) {
      if (ImGui::Selectable("By role", c.layer < 0)) {
        c.layer = -1;
        t.activated = true;
        t.commit = "Collider layer";
      }
      for (int i = 0; i < 8; ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(ps.layer_names[i].c_str(), c.layer == i)) {
          c.layer = i;
          t.activated = true;
          t.commit = "Collider layer";
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    // Compound: extra primitives (offset / rotation local).
    ImGui::TextDisabled("Compound parts: %d", static_cast<int>(c.parts.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Part")) {
      c.parts.push_back(scene3d::ColliderPart3D{});
      t.activated = true;
      t.commit = "Add collider part";
    }
    for (std::size_t i = 0; i < c.parts.size(); ++i) {
      ImGui::PushID(static_cast<int>(1000 + i));
      scene3d::ColliderPart3D& p = c.parts[i];
      int ps2 = std::min(static_cast<int>(p.shape), 2);
      ImGui::SetNextItemWidth(90);
      if (ImGui::Combo("##shape", &ps2, "Box\0Capsule\0Sphere\0")) {
        p.shape = static_cast<S>(ps2);
        t.activated = true;
        t.commit = "Part shape";
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        c.parts.erase(c.parts.begin() + static_cast<std::ptrdiff_t>(i));
        t.activated = true;
        t.commit = "Remove collider part";
        ImGui::PopID();
        break;
      }
      ImGui::DragFloat3("Part size", &p.size.x, 0.02f, 0.01f, 100.0f);
      t.after("Part size");
      ImGui::DragFloat3("Part offset", &p.offset.x, 0.02f);
      t.after("Part offset");
      ImGui::DragFloat3("Part rotation", &p.rotation_deg.x, 0.5f, -360.0f, 360.0f, "%.0f deg");
      t.after("Part rotation");
      ImGui::PopID();
    }
  }
  if (component("Rigid Body", e.rigid_body.has_value(), [&] {
        e.rigid_body = scene3d::RigidBody3D{};
        if (!e.collider) e.collider = scene3d::Collider3D{};
      }, [&] { e.rigid_body.reset(); })) {
    scene3d::RigidBody3D& b = *e.rigid_body;
    int m = static_cast<int>(b.motion);
    if (ImGui::Combo("Motion", &m, "Static\0Kinematic (script / animation)\0Dynamic (simulated)\0")) {
      b.motion = static_cast<scene3d::RigidBody3D::Motion>(m);
      t.activated = true;
      t.commit = "Body motion";
    }
    ImGui::DragFloat("Mass", &b.mass, 0.05f, 0.0f, 100000.0f, b.mass > 0 ? "%.2f kg" : "auto (volume)");
    t.after("Mass");
    ImGui::SliderFloat("Friction", &b.friction, 0.0f, 2.0f);
    t.after("Friction");
    ImGui::SliderFloat("Restitution", &b.restitution, 0.0f, 1.0f);
    t.after("Restitution");
    ImGui::DragFloat("Linear damping", &b.linear_damping, 0.01f, 0.0f, 10.0f);
    t.after("Linear damping");
    ImGui::DragFloat("Angular damping", &b.angular_damping, 0.01f, 0.0f, 10.0f);
    t.after("Angular damping");
    ImGui::DragFloat("Gravity scale", &b.gravity_scale, 0.01f, -5.0f, 5.0f);
    t.after("Gravity scale");
    ImGui::Checkbox("CCD (fast, small)", &b.ccd);
    t.after("CCD");
    ImGui::SameLine();
    ImGui::Checkbox("Impact events", &b.impact_events);
    t.after("Impact events");
    if (!e.collider) ImGui::TextColored(theme::Warning(), "Needs a Collider for its shape.");
  }
  if (component("Joint", e.joint.has_value(), [&] { e.joint = scene3d::Joint3D{}; },
                [&] { e.joint.reset(); })) {
    scene3d::Joint3D& j = *e.joint;
    int ty = static_cast<int>(j.type);
    if (ImGui::Combo("Type", &ty, "Fixed\0Hinge\0Slider\0Distance (rope)\0Ball\0")) {
      j.type = static_cast<scene3d::Joint3D::Type>(ty);
      t.activated = true;
      t.commit = "Joint type";
    }
    const scene3d::Entity3D* tgt = j.target ? find(j.target) : nullptr;
    if (ImGui::BeginCombo("Target", tgt ? tgt->name.c_str() : "(the world)")) {
      if (ImGui::Selectable("(the world)", j.target == 0)) {
        j.target = 0;
        t.activated = true;
        t.commit = "Joint target";
      }
      for (const scene3d::Entity3D& o : scene_.entities) {
        if (o.id == e.id || !o.collider) continue;
        ImGui::PushID(static_cast<int>(o.id));
        if (ImGui::Selectable(o.name.c_str(), o.id == j.target)) {
          j.target = o.id;
          t.activated = true;
          t.commit = "Joint target";
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::DragFloat3("Anchor (local)", &j.anchor.x, 0.01f);
    t.after("Joint anchor");
    using JT = scene3d::Joint3D::Type;
    if (j.type == JT::Hinge || j.type == JT::Slider) {
      ImGui::DragFloat3("Axis (local)", &j.axis.x, 0.01f, -1.0f, 1.0f);
      t.after("Joint axis");
    }
    if (j.type != JT::Fixed && j.type != JT::Ball) {
      ImGui::Checkbox("Limits", &j.limits);
      t.after("Joint limits");
      if (j.limits) {
        const char* unit = j.type == JT::Hinge ? "%.1f deg" : "%.2f m";
        ImGui::DragFloat("Min", &j.min, 0.1f, -1000.0f, 1000.0f, unit);
        t.after("Joint min");
        ImGui::DragFloat("Max", &j.max, 0.1f, -1000.0f, 1000.0f, unit);
        t.after("Joint max");
      }
    }
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
    int view = p.view == scene3d::Player3D::View::FirstPerson ? 1 : 0;
    if (ImGui::Combo("View", &view, "Third person (orbit camera)\0First person\0")) {
      p.view = view == 1 ? scene3d::Player3D::View::FirstPerson
                         : scene3d::Player3D::View::ThirdPerson;
      t.activated = true;
      t.commit = "Player view";
    }
    if (p.view == scene3d::Player3D::View::FirstPerson) {
      ImGui::DragFloat("Eye height", &p.eye_height, 0.01f, 0.1f, 10.0f, "%.2f m");
      t.after("Eye height");
    }
    using Look = scene3d::Player3D::Look;
    int look = p.look == Look::Keys ? 1 : 0;
    if (ImGui::Combo("Look", &look, "Mouse\0Keys (Q / Z, Page Up / Down)\0")) {
      p.look = look == 1 ? Look::Keys : Look::Mouse;
      t.activated = true;
      t.commit = "Player look";
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(p.view == scene3d::Player3D::View::FirstPerson
                            ? "Mouse: turn the body and tilt the eyes with the mouse.\n"
                              "Keys: Q / Z turn, Page Up / Down look, right-drag."
                            : "Mouse: orbit the camera with the mouse, scroll to zoom.\n"
                              "Keys: Q / Z or right-drag orbit.");
    }
    if (p.look == Look::Mouse) {
      ImGui::DragFloat("Sensitivity", &p.mouse_sensitivity, 0.002f,
                       scene3d::Player3D::kMinSensitivity, scene3d::Player3D::kMaxSensitivity,
                       "%.3f deg / count", ImGuiSliderFlags_AlwaysClamp);
      t.after("Mouse sensitivity");
      ImGui::Checkbox("Invert Y", &p.invert_y);
      t.after("Invert Y");
      ImGui::SliderFloat("Smoothing", &p.smoothing, 0.0f, scene3d::Player3D::kMaxSmoothing,
                         p.smoothing <= 0.0f ? "off" : "%.2f", ImGuiSliderFlags_AlwaysClamp);
      t.after("Look smoothing");
      ImGui::Checkbox("Capture cursor on Play", &p.capture_cursor);
      t.after("Capture cursor");
      ImGui::TextDisabled("Esc frees the cursor; click the view to look again.");
    }
  }
  if (component("Camera", e.camera.has_value(), [&] { e.camera = scene3d::PlayCamera3D{}; },
                [&] { e.camera.reset(); })) {
    ImGui::DragFloat("FOV", &e.camera->fov, 0.2f, 10.0f, 150.0f, "%.0f deg");
    t.after("Camera FOV");
    ImGui::TextDisabled("Play looks through it when there is no rider.");
  }
  if (component("Animator", e.animator.has_value(),
                [&] { e.animator = scene3d::default_locomotion_animator(); },
                [&] { e.animator.reset(); })) {
    const char* step = nullptr;
    draw_animator_fields(*e.animator, entity_model(e), [&](const char* l) { t.after(l); }, &step);
    if (step) {
      t.activated = true;
      t.commit = step;
    }
    if (ImGui::SmallButton("Preview clips...")) anim_preview_.open = true;
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
    edit_text("Hand model", &r.hand_model);
    t.after("Hand model");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Skinned glTF (right hand; mirrored for the left). Empty = controllers");
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
  if (component("Audio Source", e.audio.has_value(), [&] { e.audio = scene3d::audio_3d(); },
                [&] { e.audio.reset(); })) {
    const char* step = "Audio Source";
    const AudioEdit r = draw_audio_source_fields(
        *e.audio, sounds_, project_.path, true, [&](const char* l) { t.after(l); }, &step);
    if (r == AudioEdit::Commit || r == AudioEdit::Remove) {
      if (r == AudioEdit::Remove) e.audio.reset();
      t.activated = true;
      t.commit = step;
    }
    if (e.audio) normalize_audio(*e.audio);
  }
  if (component("Particles", e.particles.has_value(),
                [&] { e.particles = fx::preset("campfire", fx::Space::ThreeD); },
                [&] { e.particles.reset(); })) {
    const FxEdit r = draw_particle_fields(*e.particles, fx::Space::ThreeD);
    if (r == FxEdit::Commit || r == FxEdit::Remove) {
      if (r == FxEdit::Remove) e.particles.reset();
      t.activated = true;
      t.commit = "Particles";
    } else if (r == FxEdit::Live) {
      t.activated = true;
    }
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

void Editor3DScreen::draw_physics_settings() {
  scene3d::PhysicsSettings3D ps = scene_.physics;
  Tracker t;
  ImGui::DragFloat3("Gravity", &ps.gravity.x, 0.05f, -100.0f, 100.0f, "%.2f");
  t.after("Gravity");
  ImGui::SliderInt("Collision steps", &ps.collision_steps, 1, 8);
  t.after("Collision steps");
  ImGui::Checkbox("Debug draw (colliders + joints while playing)", &ps.debug_draw);
  t.after("Physics debug draw");
  theme::SectionHeader("Collision layers");
  for (int i = 0; i < 8; ++i) {
    ImGui::PushID(i);
    char label[16];
    std::snprintf(label, sizeof(label), "%d", i);
    edit_text(label, &ps.layer_names[i]);
    t.after("Layer name");
    ImGui::PopID();
  }
  theme::SectionHeader("Who collides with whom");
  if (ImGui::BeginTable("##matrix", 9, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    for (int j = 0; j < 8; ++j) {
      ImGui::TableNextColumn();
      ImGui::Text("%d", j);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ps.layer_names[j].c_str());
    }
    for (int i = 0; i < 8; ++i) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(ps.layer_names[i].c_str());
      for (int j = 0; j < 8; ++j) {
        ImGui::TableNextColumn();
        if (j < i) continue;  // symmetric: upper triangle only
        ImGui::PushID(i * 8 + j);
        bool on = (ps.collide[i] >> j) & 1u;
        if (ImGui::Checkbox("##c", &on)) {
          const auto bi = static_cast<std::uint8_t>(1u << i), bj = static_cast<std::uint8_t>(1u << j);
          ps.collide[i] = on ? (ps.collide[i] | bj) : (ps.collide[i] & ~bj);
          ps.collide[j] = on ? (ps.collide[j] | bi) : (ps.collide[j] & ~bi);
          t.activated = true;
          t.commit = "Collision matrix";
        }
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }
  if (t.activated && !inspector_before_) inspector_before_ = scene_;
  scene_.physics = ps;
  if (t.commit && inspector_before_) {
    const scene3d::Scene3D before = std::move(*inspector_before_);
    inspector_before_.reset();
    if (!(before == scene_)) push_undo(t.commit, before);
  }
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
