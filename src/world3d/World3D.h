#pragma once

// 3D Play mode / ts_game runtime for "dimension": "3d" projects.
//
//   * Fixed 60 Hz tick over a copy of the authored Scene3D (the caller's
//     scene is never touched).
//   * One rider (the entity with a `player` component): a capsule moved
//     with WASD / stick relative to the camera yaw, gravity, jump, slides
//     along static colliders (boxes and capsules, as world AABBs).
//   * Follow camera behind the rider (yaw from input, smoothed).
//   * Trigger colliders report enter / exit; the action button reaches the
//     nearest scripted entity within kInteractReach (on_interact).
//   * Lua gameplay scripts through the same sandboxed ScriptHost as 2D.
//     Handle methods keep their ground-plane meaning (pos / set_pos /
//     move_by work on x and z); 3D extras: pos3, set_pos3, move3, yaw,
//     set_yaw, spin, scale3, set_scale3.
//   * VR mode (set_vr(true) before start; World3D_VR.cpp): the entity with
//     a vr_rig component is the player rig (tracking-space floor origin).
//     Head + hands come from set_vr_input every frame (ts_xr: OpenXR or
//     the desktop simulator). Left stick smooth locomotion, right stick
//     snap / smooth turn and teleport arc (push forward, release to go),
//     grip grabs `grabbable` entities (throw on release), laser + trigger
//     reach scripts (on_point / on_unpoint / on_vr_trigger, else
//     on_interact), menu button opens the comfort menu. Lua: the `vr`
//     table (vr.active, vr.head, vr.hand, vr.held, vr.rig, vr.teleport,
//     vr.haptic) and on_grab(hand) / on_release(hand, speed).

#include "render3d/Camera3D.h"
#include "render3d/Scene3D.h"
#include "runtime/Log.h"
#include "runtime/Script.h"
#include "world3d/VrTypes.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace world3d {

using render3d::Aabb;
using render3d::Vec3;
using scene3d::Entity3D;
using render3d::Transform;

struct Input3D {
  float move_right = 0.0f;    // -1 .. 1
  float move_forward = 0.0f;  // -1 .. 1 (away from the camera)
  float turn = 0.0f;          // camera yaw, degrees this tick (+ = left)
  bool jump = false;
  bool action = false;
};

// One hand in the world (VR mode).
struct VrHandState {
  VrPose grip, aim;           // world space
  float trigger = 0.0f, squeeze = 0.0f;
  bool trigger_down = false, squeeze_down = false;
  std::uint64_t held = 0;     // grabbed entity
  Vec3 hold_offset;           // in hand space
  render3d::Quat hold_rotation;
  std::uint64_t pointed = 0;  // entity under the laser
  int menu_button = -1;       // comfort-menu button under the laser
  bool laser_hit = false;
  Vec3 laser_end;
  std::deque<Vec3> trail;     // recent grip positions (throw velocity)
};

struct VrTeleport {
  bool aiming = false;
  bool valid = false;
  Vec3 target;
  std::vector<Vec3> arc;
};

// The comfort menu: a row of boxes floating in front of the player.
struct VrMenuButton {
  enum class Kind : std::uint8_t { Vignette, SnapTurn, Locomotion, Seated };
  Kind kind;
  Aabb box;            // world space
  bool on = false;     // lit state (vignette on, seated...)
  std::string label;   // for 2D mirrors / logs
};

struct Toast3D {
  std::string text;
  double seconds = 2.0;
  std::uint64_t from = 0;
};

class World3D final : public runtime::ScriptWorld {
 public:
  static constexpr int kTickRate = 60;
  static constexpr double kTickSeconds = 1.0 / kTickRate;
  static constexpr float kGravity = 18.0f;        // m/s^2
  static constexpr float kInteractReach = 2.5f;   // m
  static constexpr float kKillY = -50.0f;         // fell off the world
  static constexpr int kMaxCatchUpTicks = 8;

  using LogSink = std::function<void(const runtime::LogEntry&)>;

  World3D();
  ~World3D() override;
  World3D(const World3D&) = delete;
  World3D& operator=(const World3D&) = delete;

  void set_log_sink(LogSink sink) { sink_ = std::move(sink); }
  // Pin a script source (tests, unsaved editor buffers).
  runtime::ScriptLibrary& scripts() { return lib_; }

  bool start(const scene3d::Scene3D& scene, const std::string& project_dir,
             std::string* error = nullptr);
  void stop();
  bool running() const { return running_; }
  bool paused() const { return paused_; }
  void set_paused(bool p) { paused_ = p; }

  // Real time in; 0..kMaxCatchUpTicks ticks out. Buttons seen on a frame
  // that ran no tick ride along to the next one.
  int update(double real_seconds, const Input3D& input);
  void step(const Input3D& input);  // exactly one tick
  void run_ticks(int n, const Input3D& input);
  int reload_scripts();

  std::uint64_t tick() const { return tick_; }
  const std::vector<Entity3D>& entities() const { return ents_; }
  const scene3d::Scene3D& authored() const { return authored_; }
  const Entity3D* find(std::uint64_t id) const;
  std::uint64_t player_id() const { return player_; }
  bool grounded() const { return grounded_; }
  Vec3 player_velocity() const { return vel_; }
  const render3d::Camera3D& camera() const { return camera_; }
  float camera_yaw() const { return cam_yaw_; }
  std::size_t static_colliders() const { return statics_.size(); }
  std::size_t contacts_last_tick() const { return contacts_; }
  std::size_t script_count() const;
  std::uint64_t trigger_events() const { return trigger_events_; }
  const std::deque<runtime::LogEntry>& log() const { return log_; }
  std::vector<Toast3D> take_toasts();
  const ScriptValue* state(std::uint64_t id, const std::string& key) const;

  // ScriptWorld.
  std::uint64_t script_tick() const override;
  const Entity2D* script_entity(std::uint64_t id) const override;
  std::uint64_t script_find(const std::string& name) const override;
  std::uint64_t script_player(int slot) const override;
  void script_move(std::uint64_t id, float x, float y) override;
  bool script_visible(std::uint64_t id) const override;
  void script_set_visible(std::uint64_t id, bool on) override;
  bool script_collider_on(std::uint64_t id) const override;
  void script_set_collider(std::uint64_t id, bool on) override;
  bool script_play(std::uint64_t id, const std::string& clip, bool hold) override;
  void script_release(std::uint64_t id) override;
  std::uint64_t script_spawn(const std::string& tmpl, float x, float y) override;
  std::uint64_t script_duplicate(std::uint64_t id, float x, float y) override;
  void script_destroy(std::uint64_t id) override;
  bool script_spawned(std::uint64_t id) const override;
  const ScriptValue* script_get(std::uint64_t id, const std::string& key) const override;
  void script_set(std::uint64_t id, const std::string& key, const ScriptValue* value) override;
  void script_toast(int slot, const std::string& text, double seconds,
                    std::uint64_t from) override;
  void script_log(runtime::LogEntry entry) override;
  void script_open_extensions(lua_State* L, int sandbox) override;
  void script_sound(runtime::SoundEvent event) override;

  // Sound (see runtime/Sound.h): queued for the host, metres. A positional
  // event naming an entity plays from its position.
  static constexpr std::size_t kMaxSounds = 256;
  void play_sound(runtime::SoundEvent event);
  std::vector<runtime::SoundEvent> take_sounds();
  // Where the ears are: the VR head in VR, else the play camera.
  void listener_pose(Vec3* position, Vec3* forward, Vec3* up) const;

  // Used by the Lua 3D methods.
  Entity3D* live(std::uint64_t id);

  // ---- VR mode (World3D_VR.cpp) -------------------------------------------
  static constexpr float kGrabReach = 0.12f;     // m from the grip to a box
  static constexpr float kLaserReach = 15.0f;    // m
  static constexpr float kForceGrabReach = 4.0f; // m, laser + grip pulls it in
  void set_vr(bool on) { vr_ = on; }
  bool vr() const { return vr_; }
  // Latest tracking-space input; used by every tick until the next call.
  // Button edges seen here are kept for the next tick.
  void set_vr_input(const VrInput& in);
  const VrInput& vr_input() const { return vr_in_; }
  std::uint64_t vr_rig_id() const { return rig_id_; }
  const scene3d::VrRig3D& vr_settings() const { return rig_cfg_; }
  void set_vr_settings(const scene3d::VrRig3D& cfg) { rig_cfg_ = cfg; }
  Vec3 rig_position() const { return rig_pos_; }
  float rig_yaw() const { return rig_yaw_; }  // degrees
  // Tracking space to world (rig position + yaw, seated lift).
  render3d::Mat4 rig_matrix() const;
  VrPose to_world(const VrPose& tracked) const;
  VrPose head() const { return head_; }  // world
  const VrHandState& hand(int h) const { return hands_[h & 1]; }
  const VrTeleport& teleport() const { return tele_; }
  bool comfort_menu_open() const { return menu_open_; }
  const std::vector<VrMenuButton>& comfort_menu() const { return menu_; }
  void set_comfort_menu(bool open);
  float vignette() const { return vignette_; }  // 0..1 now
  std::vector<VrHaptic> take_haptics();
  void vr_haptic(int hand, float amplitude, float seconds);
  // Move the rig so the head stands over `p` (feet at p.y).
  void vr_teleport_to(const Vec3& p);
  struct VrStats {
    std::uint64_t grabs = 0, releases = 0, throws = 0, teleports = 0, snap_turns = 0,
                  laser_triggers = 0, menu_presses = 0, landings = 0;
    float smooth_moved = 0.0f;
  };
  const VrStats& vr_stats() const { return vr_stats_; }
  // Entities in flight after a throw / drop.
  std::size_t vr_bodies() const { return bodies_.size(); }

 private:
  struct Static {
    std::uint64_t id;
    Aabb box;
  };
  void post(runtime::LogLevel level, const std::string& channel, const std::string& text,
            std::uint64_t entity = 0);
  void rebuild_statics();
  // Children ride with their parents (scene.json v9 "parent"): each moved
  // entity carries its direct children by its own delta this tick, parents
  // first, so whole chains follow.
  void follow_parents(const std::vector<std::pair<std::uint64_t, Transform>>& before);
  Aabb collider_box(const Entity3D& e) const;
  void move_player(const Input3D& in);
  void update_triggers();
  void interact();
  void follow_camera(bool snap);
  std::uint64_t spawn_copy(const Entity3D& src, float x, float z, bool keep_y, float y);
  // VR (World3D_VR.cpp).
  struct Body {
    Vec3 vel;
    std::set<std::uint64_t> inside;  // triggers it overlaps
  };
  void vr_start();
  void vr_step();
  void vr_locomotion(float dt);
  void vr_hands();
  void vr_grab(int h);
  void vr_release(int h);
  void vr_laser(int h);
  void vr_bodies_step(float dt);
  void vr_update_triggers();
  void vr_layout_menu();
  void vr_press_menu(int button);
  void vr_update_poses();
  float vr_floor_at(const Vec3& p, float max_step) const;
  void vr_event(std::uint64_t target, const char* hook, int hand, double num);
  void vr_follow_held();
 public:
  // World bounds used by the laser and grabbing: the collider box, else
  // the unit primitive under the transform.
  Aabb vr_bounds(const Entity3D& e) const;

 private:

  scene3d::Scene3D authored_;
  std::vector<Entity3D> ents_;
  bool has_parents_ = false;  // any entity has a parent (skip the work if not)
  std::set<std::uint64_t> spawned_, collider_off_, dead_;
  std::map<std::uint64_t, std::map<std::string, ScriptValue>> state_;
  mutable std::map<std::uint64_t, Entity2D> proxies_;
  std::vector<Static> statics_;
  std::set<std::uint64_t> inside_;  // triggers the rider is in
  std::string project_dir_;
  runtime::ScriptLibrary lib_;
  std::unique_ptr<runtime::ScriptHost> host_;
  LogSink sink_;
  std::deque<runtime::LogEntry> log_;
  std::vector<Toast3D> toasts_;
  std::vector<runtime::SoundEvent> sounds_;
  render3d::Camera3D camera_;
  std::uint64_t player_ = 0;
  Vec3 spawn_;
  Vec3 vel_;
  bool grounded_ = false;
  bool jump_held_ = false;
  bool action_latched_ = false;
  bool jump_latched_ = false;
  float cam_yaw_ = 0.0f;
  std::size_t contacts_ = 0;
  std::uint64_t trigger_events_ = 0;
  std::uint64_t tick_ = 0;
  bool in_step_ = false;
  bool running_ = false;
  bool paused_ = false;
  double accumulator_ = 0.0;
  // VR.
  bool vr_ = false;
  VrInput vr_in_;
  bool vr_menu_latched_ = false;
  std::uint64_t rig_id_ = 0;
  scene3d::VrRig3D rig_cfg_;
  Vec3 rig_pos_;
  float rig_yaw_ = 0.0f;
  VrPose head_;
  VrHandState hands_[2];
  VrTeleport tele_;
  bool snap_latched_ = false;
  bool menu_held_ = false;
  bool menu_open_ = false;
  Vec3 menu_anchor_;
  float menu_yaw_ = 0.0f;
  std::vector<VrMenuButton> menu_;
  float vignette_ = 0.0f;
  float turn_activity_ = 0.0f;
  std::map<std::uint64_t, Body> bodies_;
  std::map<std::uint64_t, render3d::Transform> grab_home_;
  std::vector<VrHaptic> haptics_;
  VrStats vr_stats_;
};

// Capsule (segment a-b, radius r) against a box: push-out vector that
// separates them (zero when apart). Exposed for tests.
Vec3 capsule_box_push(const Vec3& a, const Vec3& b, float r, const Aabb& box);

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
