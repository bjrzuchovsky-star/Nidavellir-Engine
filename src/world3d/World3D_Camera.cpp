// Scene cameras for 3D Play (docs/camera.md). The view comes from the
// active camera entity with the highest priority (ties: scene order);
// scripts can switch with camera.set_active. Rigs: Fixed (the entity's own
// transform), Orbit (third-person boom behind a target, mouse look, scroll
// zoom, collision) and FirstPerson (the target's eyes). Scenes from before
// v18 keep the old rules (legacy_cameras) until the editor migrates them.
//
// Lua (graphs / C++ / C# through the plugin bridge, table "camera"):
//   camera.active() -> id (0 = no camera entity: the built-in follow view)
//   camera.set_active(e | id | name) -> ok      camera.reset()
//   camera.set_priority(e, p) -> ok              camera.count() -> cameras
//   camera.set_fov(e, degrees) -> ok

#include "world3d/World3D.h"

#include "render3d/Attach3D.h"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ts {
namespace tombstone {
namespace world3d {

using render3d::Vec3;
using runtime::LogLevel;

std::uint64_t World3D::pick_camera() const {
  if (forced_cam_ != 0) {
    const Entity3D* f = find(forced_cam_);
    if (f && f->camera) return forced_cam_;
  }
  const Entity3D* best = nullptr;
  for (const Entity3D& e : ents_) {
    if (!e.camera || !e.camera->active) continue;
    if (!best || e.camera->priority > best->camera->priority) best = &e;
  }
  return best ? best->id : 0;
}

bool World3D::set_active_camera(std::uint64_t id) {
  if (id == 0) {
    forced_cam_ = 0;
    if (running_ && !vr_) update_camera(true);
    return true;
  }
  const Entity3D* e = find(id);
  if (!e || !e->camera) return false;
  forced_cam_ = id;
  if (running_ && !vr_) update_camera(true);
  return true;
}

bool World3D::set_camera_priority(std::uint64_t id, int priority) {
  for (Entity3D& e : ents_) {
    if (e.id == id && e.camera) {
      e.camera->priority = priority;
      return true;
    }
  }
  return false;
}

bool World3D::set_camera_fov(std::uint64_t id, float fov) {
  for (Entity3D& e : ents_) {
    if (e.id == id && e.camera) {
      e.camera->fov = std::clamp(fov, 10.0f, 150.0f);
      return true;
    }
  }
  return false;
}

std::size_t World3D::camera_count() const {
  return static_cast<std::size_t>(
      std::count_if(ents_.begin(), ents_.end(), [](const Entity3D& e) { return e.camera.has_value(); }));
}

render3d::Environment World3D::view_environment() const {
  render3d::Environment e = env_;
  if (view_.use_clear) {
    for (int i = 0; i < 3; ++i) {
      e.sky_top[i] = e.sky_horizon[i] = e.ground[i] = view_.clear[i];
    }
  }
  return e;
}

// Where the orbit boom may go: the old static-box pull-in, then the
// physics plugin's sphere cast (no-op without one).
Vec3 World3D::boom_collide(const Vec3& from, Vec3 want, float radius, std::uint64_t ignore) const {
  const render3d::Ray ray{from, render3d::normalize(want - from)};
  const float full = render3d::length(want - from);
  // Static boxes (always), then the physics sphere cast over the full boom
  // when a physics backend is loaded; the nearer of the two wins.
  float dist = full;
  for (const Static& s : statics_) {
    float t = 0.0f;
    if (render3d::ray_aabb(ray, s.box, &t) && t > 0.3f && t < full) dist = std::min(dist, std::max(0.8f, t - 0.3f));
  }
  if (const physics3d::World* pw = phys_.get(); pw && full > 1e-4f) {
    physics3d::RayHit hit;
    if (pw->sphere_cast(from, std::max(0.01f, radius), ray.dir, full, &hit, 0xff, body_of(ignore))) {
      dist = std::min(dist, std::max(0.3f, hit.distance));
      ++boom_hits_;
    }
  }
  if (dist < full) want = from + ray.dir * dist;
  return want;
}

void World3D::update_camera(bool snap) {
  if (vr_) return;
  if (legacy_cams_) {
    follow_camera(snap);
    return;
  }
  const std::uint64_t id = pick_camera();
  if (id != active_cam_) {
    snap = true;
    if (active_cam_ != 0 || id != 0) {
      const Entity3D* c = find(id);
      post(LogLevel::Info, "play", "Camera: " + (c ? c->name : std::string("built-in follow view")));
    }
    // Orbit cameras start at their own boom length.
    if (const Entity3D* c = find(id); c && c->camera && c->camera->rig == scene3d::PlayCamera3D::Rig::Orbit)
      cam_dist_ = c->camera->distance;
    active_cam_ = id;
  }
  Entity3D* cam = nullptr;
  for (Entity3D& e : ents_)
    if (e.id == id) cam = &e;
  if (!cam || !cam->camera) {
    view_ = {};
    camera_.orthographic = false;
    camera_.z_near = 0.1f;
    camera_.z_far = 1000.0f;
    follow_camera(snap);  // no camera entity: the built-in rider view
    return;
  }
  const scene3d::PlayCamera3D& pc = *cam->camera;
  view_.use_clear = pc.use_clear;
  std::copy(pc.clear_color, pc.clear_color + 4, view_.clear);
  std::copy(pc.viewport, pc.viewport + 4, view_.viewport);
  camera_.fov_y_degrees = pc.fov;
  camera_.z_near = pc.near_clip;
  camera_.z_far = pc.far_clip;
  camera_.orthographic = pc.projection == scene3d::PlayCamera3D::Projection::Orthographic;
  camera_.ortho_size = pc.ortho_size;
  camera_.up = {0.0f, 1.0f, 0.0f};
  const std::uint64_t follow_id = pc.follow != 0 ? pc.follow : player_;
  const Entity3D* tgt = pc.rig == scene3d::PlayCamera3D::Rig::Fixed ? nullptr : find(follow_id);
  if (!tgt) {
    // Fixed (or a missing target): the entity itself, looking down -Z.
    camera_.position = cam->transform.position;
    camera_.target = cam->transform.position + cam->transform.rotation.rotate(Vec3{0.0f, 0.0f, -1.0f});
    camera_.up = cam->transform.rotation.rotate(Vec3{0.0f, 1.0f, 0.0f});
    return;
  }
  const float yaw = render3d::radians(cam_yaw_);
  Transform now = cam->transform;
  if (pc.rig == scene3d::PlayCamera3D::Rig::FirstPerson) {
    Vec3 eye;
    if (cam->parent == tgt->id) {
      eye = cam->transform.position;  // carried by its parent
    } else {
      const scene3d::Player3D cfg = tgt->player ? *tgt->player : scene3d::Player3D{};
      float h = 2.0f;
      Vec3 off;
      if (tgt->collider) {
        h = tgt->collider->size.y;
        off = tgt->collider->offset;
      }
      eye = tgt->transform.position + off + Vec3{0.0f, cfg.eye_height - 0.5f * h, 0.0f};
    }
    const float pitch = render3d::radians(cam_pitch_);
    const Vec3 look{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    camera_.position = eye;
    camera_.target = eye + look;
    now.position = eye;
    now.rotation = scene3d::look_rotation(look);
  } else {
    // Orbit: the old follow camera with this camera's boom.
    const float elev = render3d::radians(-cam_pitch_);
    const float dist = cam_dist_;
    const Vec3 target = tgt->transform.position + Vec3{0, 1.0f, 0};
    Vec3 want = target + Vec3{std::sin(yaw) * std::cos(elev) * dist,
                              std::sin(elev) * dist + (pc.height - 1.0f),
                              std::cos(yaw) * std::cos(elev) * dist};
    if (pc.collision) want = boom_collide(target, want, pc.collision_radius, tgt->id);
    if (snap) {
      camera_.position = want;
      camera_.target = target;
    } else {
      camera_.position = render3d::lerp(camera_.position, want, 0.18f);
      camera_.target = render3d::lerp(camera_.target, target, 0.3f);
    }
    now.position = camera_.position;
    now.rotation = scene3d::look_rotation(camera_.target - camera_.position);
  }
  // The camera entity is where the view is (scripts, children ride along).
  if (!(now == cam->transform)) scene3d::move_with_children(ents_, cam->id, now);
}

// Follow constraints and bone attachments, after parents carried children.
void World3D::apply_attachments() {
  if (!has_attach_) return;
  for (Entity3D& e : ents_) {
    if (e.follow && e.follow->target != 0) {
      const Entity3D* t = find(e.follow->target);
      if (t && t->id != e.id) {
        const Transform now = scene3d::follow_result(e, t->transform);
        if (!(now == e.transform)) scene3d::move_with_children(ents_, e.id, now);
      }
    }
  }
  for (Entity3D& e : ents_) {
    if (e.parent_bone.empty() || e.parent == 0) continue;
    const Entity3D* p = find(e.parent);
    render3d::AnimatorRuntime* a = animator(e.parent);
    if (!p || !a || !a->model()) continue;
    render3d::Mat4 bw;
    if (!scene3d::bone_world(*p, *a->model(), e.parent_bone, &a->pose(), &bw)) continue;
    const Transform now = scene3d::bone_follow(e, bw);
    if (!(now == e.transform)) scene3d::move_with_children(ents_, e.id, now);
  }
}

namespace {

constexpr const char* kEntityMeta = "TombStone.Entity";  // see ScriptApi.cpp

World3D& W(lua_State* L) { return *static_cast<World3D*>(lua_touserdata(L, lua_upvalueindex(1))); }

std::uint64_t cam_arg(lua_State* L, int idx) {
  if (const auto* u = static_cast<std::uint64_t*>(luaL_testudata(L, idx, kEntityMeta))) return *u;
  if (lua_type(L, idx) == LUA_TSTRING) {
    const std::string name = lua_tostring(L, idx);
    for (const scene3d::Entity3D& e : W(L).entities())
      if (e.camera && e.name == name) return e.id;
    return 0;
  }
  const lua_Number n = luaL_checknumber(L, idx);
  return n > 0 ? static_cast<std::uint64_t>(n) : 0;
}

int l_active(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(W(L).active_camera()));
  return 1;
}
int l_set_active(lua_State* L) {
  const std::uint64_t id = cam_arg(L, 1);
  lua_pushboolean(L, id != 0 && W(L).set_active_camera(id));
  return 1;
}
int l_reset(lua_State* L) {
  W(L).set_active_camera(0);
  return 0;
}
int l_set_priority(lua_State* L) {
  lua_pushboolean(L, W(L).set_camera_priority(cam_arg(L, 1), static_cast<int>(luaL_checknumber(L, 2))));
  return 1;
}
int l_set_fov(lua_State* L) {
  lua_pushboolean(L, W(L).set_camera_fov(cam_arg(L, 1), static_cast<float>(luaL_checknumber(L, 2))));
  return 1;
}
int l_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(W(L).camera_count()));
  return 1;
}

}  // namespace

void World3D::open_camera_lua(lua_State* L, int sandbox) {
  const luaL_Reg fns[] = {{"active", l_active},       {"set_active", l_set_active},
                          {"reset", l_reset},         {"set_priority", l_set_priority},
                          {"set_fov", l_set_fov},     {"count", l_count},
                          {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, fns, 1);
  lua_setfield(L, sandbox, "camera");
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
