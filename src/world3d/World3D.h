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

#include "fx/Particles.h"
#include "render3d/Animator.h"
#include "physics3d/Physics3D.h"
#include "render3d/Renderer3D.h"
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

class World3D;

// Optional systems a host attaches to a World3D before start() (ts.ai's
// navmesh agents). on_start runs after the statics are built and before
// scripts start; on_tick after the scripts each tick (before animation);
// open_lua adds globals to every script sandbox; debug_lines feeds the
// editor's overlay.
class World3DExtension {
 public:
  struct Line {
    Vec3 a, b;
    float rgba[4] = {1, 1, 1, 1};
  };
  virtual ~World3DExtension() = default;
  virtual const char* name() const = 0;
  virtual void on_start(World3D& /*world*/) {}
  virtual void on_tick(World3D& /*world*/, double /*dt*/) {}
  virtual void on_stop(World3D& /*world*/) {}
  virtual void on_sound(World3D& /*world*/, const runtime::SoundEvent& /*ev*/) {}
  virtual void open_lua(World3D& /*world*/, lua_State* /*L*/, int /*sandbox*/) {}
  virtual void debug_lines(const World3D& /*world*/, std::vector<Line>* /*out*/) const {}
};

struct Input3D {
  float move_right = 0.0f;    // -1 .. 1
  float move_forward = 0.0f;  // -1 .. 1 (away from the camera)
  float turn = 0.0f;          // camera yaw, degrees this tick (+ = left)
  bool jump = false;
  bool action = false;
  // Desktop physics-gun: hold `grab` to pick up the dynamic body in front of
  // the rider and carry it (it pushes and knocks things over); `throw_held`
  // flings it along the camera.
  bool grab = false;
  bool throw_held = false;
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
  bool pulling = false;       // force grab still flying in to the hand
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
  // Extensions (see World3DExtension); kept across start / stop.
  void add_extension(std::shared_ptr<World3DExtension> ext);
  World3DExtension* extension(const std::string& name) const;
  void clear_extensions();
  void debug_lines(std::vector<World3DExtension::Line>* out) const;
  // Pin a script source (tests, unsaved editor buffers).
  runtime::ScriptLibrary& scripts() { return lib_; }

  bool start(const scene3d::Scene3D& scene, const std::string& project_dir,
             std::string* error = nullptr);
  void stop();
  bool running() const { return running_; }
  const std::string& project_dir() const { return project_dir_; }
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
  void script_ui(runtime::UiCommand command) override;
  void script_fx(runtime::FxEvent event) override;
  // Particles (metres, y up): entity "particles" emitters plus bursts from
  // Lua particles.* / gameplay (VR grab sparkle). fx() takes x, y, z.
  void fx(runtime::FxEvent event);
  const fx::ParticleWorld& particles() const { return particles_; }
  void billboards(std::vector<render3d::Renderer3D::Billboard>* out) const;
  // In-game UI: Lua ui.* requests for the host, widget events back.
  std::vector<runtime::UiCommand> take_ui_commands();
  void ui_event(const std::string& widget, bool change, double value, const std::string& text);
  // Plugins see widget events before the scripts (by owner; one each). A
  // tap returning true consumes the event.
  using UiTap = std::function<bool(const std::string& widget, bool change, double value,
                                   const std::string& text)>;
  void set_ui_tap(const std::string& owner, UiTap tap);
  // Call hook(text, number) on every running script that defines it.
  void script_broadcast(const char* hook, const std::string& text, double number);
  // Story plugins: the rider's position (saves) without touching physics
  // internals; false when there is no rider.
  bool rider_position(Vec3* out) const;
  bool set_rider_position(const Vec3& p);

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
  // Grabbables in flight after a throw / drop (awake, moving, not held).
  std::size_t vr_bodies() const;

  // ---- Physics (World3D_Physics.cpp; Jolt via ts_physics3d) ----------------
  // Every collider is a body: static by default, dynamic for grabbables and
  // rigid_body components (kinematic follows the entity transform). The
  // rider is a Jolt character. Held things are driven by velocity, so they
  // collide, push and knock other bodies over (impacts: on_impact(other,
  // speed), "builtin:impact" sound, dust, a buzz in the holding hand).
  physics3d::World* physics() { return phys_.get(); }
  const physics3d::World* physics() const { return phys_.get(); }
  physics3d::BodyId body_of(std::uint64_t id) const;
  std::uint64_t entity_of(physics3d::BodyId body) const;
  std::size_t physics_bodies() const { return phys_ ? phys_->body_count() : 0; }
  std::size_t physics_joints() const { return phys_ ? phys_->joint_count() : 0; }
  struct ImpactStats {
    std::uint64_t impacts = 0;    // contacts above the impact speed
    std::uint64_t held_hits = 0;  // ... where one side was held / carried
    float max_speed = 0.0f;
    std::uint64_t last_a = 0, last_b = 0;
  };
  const ImpactStats& impact_stats() const { return impact_stats_; }
  // Desktop carry (physics gun).
  std::uint64_t carried() const { return carry_.id; }
  // Lua / graph / C++ / C# physics calls.
  bool phys_apply(std::uint64_t id, const char* what, const Vec3& v);
  bool phys_set_velocity(std::uint64_t id, const Vec3& v);
  Vec3 phys_velocity(std::uint64_t id) const;
  bool phys_raycast(const Vec3& from, const Vec3& dir, float dist, physics3d::RayHit* hit) const;
  // Join two entities (0 = the world); returns the joint handle or 0.
  physics3d::JointId phys_joint(const scene3d::Joint3D& desc, std::uint64_t a, std::uint64_t b,
                                std::string* error = nullptr);
  void phys_unjoint(physics3d::JointId id);
  static constexpr float kImpactSpeed = 1.2f;  // m/s for an impact event

  // ---- Skeletal animation (World3D_Anim.cpp) -------------------------------
  // Entities with an `animator` get an AnimatorRuntime on their first tick
  // (or first anim.* call); each tick it advances, feeds the rider's speed /
  // grounded / jump, fires on_anim_event(name), applies root motion and
  // leaves the skinning palette on the live entity for SceneView. Lua:
  // anim.play / crossfade / set_param / param / trigger / state / stop.
  render3d::AnimatorRuntime* animator(std::uint64_t id);
  std::size_t animators() const;
  bool anim_play(std::uint64_t id, const std::string& name, float fade, bool hold);
  // VR skinned hands (vr_rig.hand_model): the posed palette per hand, null
  // when there is no hand model.
  std::shared_ptr<const std::vector<render3d::Mat4>> vr_hand_palette(int hand) const;

  struct AnimSystem;  // World3D_Anim.cpp
  struct AnimSystemDeleter {
    void operator()(AnimSystem* a) const;
  };

 private:
  std::unique_ptr<AnimSystem, AnimSystemDeleter> anim_;
  void anim_step();
  void anim_open_lua(lua_State* L, int sandbox);
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
  void vr_start();
  void vr_step();
  void vr_locomotion(float dt);
  void vr_hands();
  void vr_grab(int h);
  // Grab / throw / comfort-menu sounds (0 entity = a ui click, not placed).
  void vr_sound(const char* clip, std::uint64_t entity, float volume, float pitch);
  void vr_release(int h);
  void vr_laser(int h);
  void vr_update_triggers();
  void vr_layout_menu();
  void vr_press_menu(int button);
  void vr_update_poses();
  float vr_floor_at(const Vec3& p, float max_step) const;
  void vr_event(std::uint64_t target, const char* hook, int hand, double num);
  void vr_follow_held();
  void vr_drive_held(float dt);
  // Physics glue (World3D_Physics.cpp).
  void phys_build();
  bool phys_wants_body(const Entity3D& e) const;
  void phys_add(const Entity3D& e);
  void phys_remove(std::uint64_t id);
  void phys_pre(float dt);
  void phys_step(float dt);
  void phys_post();
  void phys_drive(std::uint64_t id, const Vec3& p, const render3d::Quat& q, float dt,
                  float max_speed);
  void phys_carry(const Input3D& in, float dt);
  void phys_on_hold(std::uint64_t id, bool held);
  bool phys_held(std::uint64_t id) const;
  const render3d::MeshData* phys_mesh(const std::string& source);
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
  std::vector<std::shared_ptr<World3DExtension>> exts_;
  LogSink sink_;
  std::deque<runtime::LogEntry> log_;
  std::vector<Toast3D> toasts_;
  std::vector<runtime::SoundEvent> sounds_;
  std::vector<runtime::UiCommand> ui_commands_;
  std::vector<std::pair<std::string, UiTap>> ui_taps_;
  fx::ParticleWorld particles_{fx::Space::ThreeD};
  void step_fx();
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
  std::unique_ptr<physics3d::World> phys_;
  std::map<std::uint64_t, physics3d::BodyId> body_ids_;
  std::map<physics3d::BodyId, std::uint64_t> body_owner_;
  std::map<std::uint64_t, Transform> synced_;   // last transform the body had
  std::map<std::uint64_t, physics3d::JointId> joint_ids_;
  std::map<std::uint64_t, std::uint64_t> impact_quiet_until_;  // tick
  std::set<std::uint64_t> flying_;              // grabbables in flight
  std::map<std::string, std::unique_ptr<render3d::MeshData>> phys_meshes_;
  ImpactStats impact_stats_;
  struct Carry {
    std::uint64_t id = 0;
    float distance = 2.0f;
    render3d::Quat rel;  // body rotation relative to the camera yaw
    bool grab_down = false, throw_down = false;
  } carry_;
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
