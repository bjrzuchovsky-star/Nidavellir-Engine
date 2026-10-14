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

#include "render3d/Camera3D.h"
#include "render3d/Scene3D.h"
#include "runtime/Log.h"
#include "runtime/Script.h"

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

struct Input3D {
  float move_right = 0.0f;    // -1 .. 1
  float move_forward = 0.0f;  // -1 .. 1 (away from the camera)
  float turn = 0.0f;          // camera yaw, degrees this tick (+ = left)
  bool jump = false;
  bool action = false;
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

  // Used by the Lua 3D methods.
  Entity3D* live(std::uint64_t id);

 private:
  struct Static {
    std::uint64_t id;
    Aabb box;
  };
  void post(runtime::LogLevel level, const std::string& channel, const std::string& text,
            std::uint64_t entity = 0);
  void rebuild_statics();
  Aabb collider_box(const Entity3D& e) const;
  void move_player(const Input3D& in);
  void update_triggers();
  void interact();
  void follow_camera(bool snap);
  std::uint64_t spawn_copy(const Entity3D& src, float x, float z, bool keep_y, float y);

  scene3d::Scene3D authored_;
  std::vector<Entity3D> ents_;
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
};

// Capsule (segment a-b, radius r) against a box: push-out vector that
// separates them (zero when apart). Exposed for tests.
Vec3 capsule_box_push(const Vec3& a, const Vec3& b, float r, const Aabb& box);

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
