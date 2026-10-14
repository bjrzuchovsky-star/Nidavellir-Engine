#pragma once

// Runtime world: the playable copy of a scene. Built from the same
// Entity2D list the editor saves to scene.json, advanced in fixed ticks by
// abstract input, and drawn by whoever asks for a draw list (ImGui in the
// editor, OpenGL in ts_game). No UI or graphics API in here.

#include "runtime/Collision.h"
#include "runtime/Input.h"
#include "runtime/Log.h"
#include "runtime/Script.h"
#include "runtime/WorldExtensions.h"
#include "scene/Animation.h"
#include "scene/SceneData.h"
#include "scene/TileMap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

// Simulation rate. Rendering runs at whatever the display does and
// interpolates between the last two ticks.
inline constexpr int kTickRate = 60;
inline constexpr double kTickSeconds = 1.0 / kTickRate;

// Which way a rider looks (top-down). Picks the idle_ / walk_ clip.
enum class Facing : std::uint8_t { Down, Up, Left, Right };
// "down" / "up" / "left" / "right" (clip name suffixes).
const char* to_string(Facing facing);

// Where an animator is in its set. Rebuilt with the world.
struct AnimState {
  int set = -1;          // index into the world's sets (-1 = no animation)
  std::string clip;      // clip playing now
  double time = 0.0;     // seconds into it (speed-scaled)
  int frame = 0;         // frame index within the clip
  bool finished = false; // a once clip resting on its last frame
  bool flip_x = false;   // mirrored stand-in (left from a right clip)
  bool hold = false;     // play_clip(..., hold): auto-pick keeps out
  bool walking = false;  // players: move input this tick
};

// Runtime state for one entity. `data` starts as the authored entity and
// is the live copy (x/y move); the rest is state the editor never sees.
struct Actor {
  Entity2D data;
  float prev_x = 0.0f;  // position before the latest tick (interpolation)
  float prev_y = 0.0f;
  float vx = 0.0f;      // world px / s actually moved in the latest tick
  float vy = 0.0f;      // (after walls; 0 when stopped flush)
  int facing = 1;       // +1 right, -1 left (last horizontal heading)
  Facing dir = Facing::Down;  // four-way heading; kept while idle
  float in_x = 0.0f;    // move intent of the latest tick (-1..1)
  float in_y = 0.0f;
  AnimState anim;
  // Script-driven state (World_Script.cpp).
  bool hidden = false;   // hide(): skipped by the draw list
  bool dead = false;     // destroy(): removed after the script phase
  bool spawned = false;  // made by spawn() / duplicate() during the ride
  std::optional<ColliderData> stashed;  // set_collider(false) parks it here
  std::map<std::string, ScriptValue> state;  // e:get / e:set
};

// What the camera shows: world point at the view centre plus scale
// (screen px per world px).
struct CameraView {
  float x = 0.0f;
  float y = 0.0f;
  float zoom = 1.0f;
};

struct WorldRect {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
};

// One textured or solid quad in world space. image is a project-relative
// path (null = solid colour) valid until the world changes.
struct DrawQuad {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const std::string* image = nullptr;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 1.0f;
  float v1 = 1.0f;
  bool missing = false;  // sprite image unavailable: solid stand-in
  std::uint64_t entity = 0;
  float rotation = 0.0f;  // degrees clockwise about the quad centre
};

// Something walked into or out of a trigger collider. Recorded per tick;
// the trigger's script hears it (on_trigger_enter / on_trigger_exit) and
// Telegraph logs it.
struct TriggerEvent {
  enum class Kind { Enter, Exit };
  Kind kind = Kind::Enter;
  std::uint64_t trigger = 0;  // entity carrying the trigger collider
  std::uint64_t other = 0;    // body that rode in / out
  std::uint64_t tick = 0;     // the tick it happened on (1 = first tick)

  bool operator==(const TriggerEvent& o) const = default;
};

// Image dimensions for a project-relative path; false when the file is
// missing or does not decode. Lets the draw list compute UVs without
// knowing about textures.
using ImageSizeFn =
    std::function<bool(const std::string& rel_path, int* width, int* height)>;

// Absolute (or cwd-relative) path for a project-relative asset path.
std::string resolve_asset(const std::string& project_dir,
                          const std::string& rel_path);

// A short HUD message from a script ("Picked up 10 gold").
struct Toast {
  int slot = -1;             // rider slot it shows for (-1 = everyone)
  std::string text;
  std::uint64_t until = 0;   // last tick it shows on
  std::uint64_t entity = 0;  // whose script sent it
};

// One actor in a network snapshot: what a client needs to draw it and
// predict against it.
struct NetActor {
  std::uint64_t id = 0;
  float x = 0.0f;
  float y = 0.0f;
  float in_x = 0.0f;  // rider move intent (animation heading)
  float in_y = 0.0f;
  std::int8_t facing = 1;
  Facing dir = Facing::Down;
  bool hidden = false;
  bool collider_on = true;  // false: a script parked its collider
  int slot = -1;            // rider slot (-1 = not a rider)
  bool spawned = false;     // made during the ride: `name` is its template
  std::string name;

  bool operator==(const NetActor& o) const = default;
};

// Where a world runs: alone (editor Play, ts_game), as the authoritative
// server of a posse, or as a client's mirror that is never stepped.
enum class NetRole { Local, Server, Client };

// A plugin message waiting for the network layer (World::net_post).
struct NetPost {
  std::string channel;
  int to_slot = -1;  // server: rider slot, -1 = everybody; client: ignored
  std::vector<std::uint8_t> bytes;
};

class World final : public ScriptWorld {
 public:
  World() = default;
  ~World() override;
  // The script host keeps a reference to its world: no copies, no moves.
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Build from authored entities (copied). Each PlayerController whose
  // slot has a SpawnPoint starts centred on it. project_dir anchors the
  // project-relative image paths. False (with a reason) for an empty scene.
  bool build(const std::vector<Entity2D>& entities,
             const std::string& project_dir = std::string(),
             std::string* error_out = nullptr);
  // Same, with the scene's tile solidity (which tile ids block riders).
  bool build(const std::vector<Entity2D>& entities,
             const TileSolidity& solidity, const std::string& project_dir,
             std::string* error_out = nullptr);
  // Read <project_dir>/scene.json and build from it.
  bool load_project(const std::string& project_dir,
                    std::string* error_out = nullptr);
  void clear();
  bool empty() const { return actors_.empty(); }
  const std::string& project_dir() const { return project_dir_; }

  // Exactly one fixed tick (kTickSeconds) with this input.
  void step(const InputFrame& input);
  std::uint64_t tick() const { return tick_; }
  double time_seconds() const { return static_cast<double>(tick_) * kTickSeconds; }

  // Input of the tick being simulated (the latest one between ticks).
  const InputFrame& last_input() const { return input_; }

  const std::vector<Actor>& actors() const { return actors_; }
  Actor* find(std::uint64_t id);
  const Actor* find(std::uint64_t id) const;
  // First actor driven by `slot`, or null.
  const Actor* player(int slot) const;
  int player_count() const;

  // Screen size in pixels; the camera keeps its bounds with it.
  void set_view_size(float width, float height);
  // Camera between the previous and latest tick (alpha 0..1).
  CameraView camera(float alpha = 1.0f) const;
  // Entity carrying the active Camera2D (0 = implicit follow camera).
  std::uint64_t camera_entity() const { return cam_.entity; }
  std::uint64_t camera_target() const { return cam_.target; }
  // World rect a camera shows on a width x height pixel view.
  static WorldRect view_rect(const CameraView& cam, float width, float height);

  // --- Collision (World_Collision.cpp) ----------------------------------------
  // Riders move one axis at a time (X, resolve, Y, resolve) against solid
  // tiles and static solid colliders, in substeps of at most half the
  // thinnest obstacle, so they slide along walls and never tunnel. Dynamic
  // bodies that end a tick overlapping are pushed apart.
  const TileSolidity& tile_solidity() const { return solidity_; }
  // Trigger enter / exit events of the latest tick (cleared every tick).
  const std::vector<TriggerEvent>& trigger_events() const { return events_; }
  const std::optional<TriggerEvent>& last_trigger_event() const {
    return last_event_;
  }
  std::uint64_t trigger_event_total() const { return events_total_; }
  bool inside_trigger(std::uint64_t trigger, std::uint64_t other) const;
  // "Player rode into Gate" (names from the scene).
  std::string describe(const TriggerEvent& event) const;
  // Collision overlay boxes visible in `view` (interpolated like the draw
  // list); triggers with somebody inside are marked active.
  void build_overlay(const WorldRect& view, float alpha,
                     std::vector<OverlayBox>* out) const;
  // Largest single move before resolution splits it (tests / overlay).
  float substep_limit() const { return step_limit_; }

  // --- Animation (World_Anim.cpp) ---------------------------------------------
  // Animators advance on the fixed tick. Riders pick idle_<dir> / walk_<dir>
  // from their move intent (falling back to fewer directions, mirroring
  // right for left, then the default clip) and keep facing while idle.
  // Sets come from the library, which outlives build() and re-reads a
  // changed .anim.json on the next build.
  AnimLibrary& anim_library() { return anim_lib_; }
  const AnimLibrary::Entry* anim_set(const Actor& a) const;
  // Play `clip` on entity `id` (restart = from frame 0 even if already on
  // it). hold keeps a rider's auto-pick off it until release_clip(). False
  // when the entity has no animator or the set has no such clip.
  bool play_clip(std::uint64_t id, const std::string& clip,
                 bool restart = true, bool hold = false);
  void release_clip(std::uint64_t id);
  // Clip a rider auto-picks for a heading (also used by the editor preview).
  // Sets *flip_x when a right clip stands in for left (or vice versa).
  static std::string pick_clip(const AnimSet& set, Facing dir, int facing,
                               bool walking, const std::string& fallback,
                               bool* flip_x);

  // --- Scripts (World_Script.cpp) -----------------------------------------------
  // Entities with a script component run it inside step(), after riders
  // move and triggers resolve: on_start (first tick), trigger hooks,
  // on_interact (action button pressed within kInteractReach px), timers,
  // then on_tick. Spawns join at once (on_start next tick); destroys land
  // at the end of the script phase.
  static constexpr float kInteractReach = 24.0f;
  static constexpr std::size_t kMaxActors = 10000;
  static constexpr std::size_t kMaxPendingLogs = 4096;
  // Sources outlive build() like anim_library(); tests put() into it.
  ScriptLibrary& script_library() { return script_lib_; }
  ScriptHost* scripts() { return scripts_.get(); }
  // Re-run script files changed on disk (Play hot reload). Files reloaded.
  int reload_scripts();
  // Telegraph lines since the last take (scripts, toasts, trigger events).
  std::vector<LogEntry> take_logs();
  const std::vector<LogEntry>& pending_logs() const { return logs_; }
  std::uint64_t log_total() const { return log_total_; }
  const std::vector<Toast>& toasts() const { return toasts_; }
  // Toasts on one rider's HUD now: its own plus everyone's, oldest first.
  std::vector<const Toast*> toasts_for(int slot) const;
  // What the rider's action button would poke: the nearest scripted entity
  // with on_interact within kInteractReach px of its collider (0 = none).
  std::uint64_t interact_target(std::uint64_t rider);

  // ScriptWorld: what scripts reach (see Script.h).
  std::uint64_t script_tick() const override;
  const Entity2D* script_entity(std::uint64_t id) const override;
  std::uint64_t script_find(const std::string& name) const override;
  std::uint64_t script_player(int slot) const override;
  void script_move(std::uint64_t id, float x, float y) override;
  bool script_visible(std::uint64_t id) const override;
  void script_set_visible(std::uint64_t id, bool on) override;
  bool script_collider_on(std::uint64_t id) const override;
  void script_set_collider(std::uint64_t id, bool on) override;
  bool script_play(std::uint64_t id, const std::string& clip,
                   bool hold) override;
  void script_release(std::uint64_t id) override;
  std::uint64_t script_spawn(const std::string& tmpl, float x,
                             float y) override;
  std::uint64_t script_duplicate(std::uint64_t id, float x, float y) override;
  void script_destroy(std::uint64_t id) override;
  bool script_spawned(std::uint64_t id) const override;
  const ScriptValue* script_get(std::uint64_t id,
                                const std::string& key) const override;
  void script_set(std::uint64_t id, const std::string& key,
                  const ScriptValue* value) override;
  void script_toast(int slot, const std::string& text, double seconds,
                    std::uint64_t from) override;
  void script_log(LogEntry entry) override;
  void script_open_extensions(lua_State* L, int sandbox) override;
  void script_sound(SoundEvent event) override;

  // --- Sound (World_Plugins.cpp) -----------------------------------------------
  // Queue a sound for the host to play (see runtime/Sound.h). On a server
  // the event also goes out on the "ts.audio" channel (to event.slot, -1 =
  // every rider); a client queues what arrives on it. Keeps the newest
  // kMaxSounds when nobody drains the queue (a headless server).
  static constexpr std::size_t kMaxSounds = 256;
  void play_sound(SoundEvent event);
  std::vector<SoundEvent> take_sounds();
  const std::vector<SoundEvent>& pending_sounds() const { return sounds_; }
  std::uint64_t sound_total() const { return sound_total_; }

  // --- Plugins (WorldExtensions.h) -------------------------------------------
  // Systems / Lua tables / components plugins add. Outlives build() like the
  // libraries; set it before build() so scripts see the Lua tables. A system
  // that throws is switched off for the rest of the ride (logged once).
  void set_extensions(std::shared_ptr<const WorldExtensions> ext);
  const WorldExtensions* extensions() const { return ext_.get(); }
  // Systems still running this ride (a throwing one drops out).
  std::size_t extension_systems_live() const;

  // --- Plugin network channels (World_Plugins.cpp) ---------------------------
  // The host (GameServer / GameClient) says what this world is; plugins
  // read it to decide whether to simulate or mirror.
  void set_net_role(NetRole role) { net_role_ = role; }
  NetRole net_role() const { return net_role_; }
  // Queue a message on a plugin channel. Dropped in a Local world; the
  // queue keeps the newest kMaxNetPosts.
  static constexpr std::size_t kMaxNetPosts = 4096;
  void net_post(std::string channel, int to_slot, std::vector<std::uint8_t> bytes);
  std::vector<NetPost> take_net_posts();
  // Hand an arrived message to the channel's owner; false when no loaded
  // plugin owns `channel` (or it threw, logged).
  bool net_deliver(const std::string& channel, int from_slot,
                   const std::vector<std::uint8_t>& bytes);

  // --- Networking (World_Net.cpp) -------------------------------------------
  // Hooks for ts_mp: the authoritative server steps this world as usual and
  // captures NetActor rows for snapshots; a client keeps a world it never
  // steps, applies snapshot rows to it, and predicts its own rider with
  // predict_rider() (movement + walls only, no scripts or triggers).
  //
  // The rider for `slot`, cloning the first rider (slot 0 preferred) onto
  // that slot's SpawnPoint (or beside the template) when the scene has
  // none. `forced_id` (client side) reuses the server's id. 0 = the scene
  // has no rider to clone.
  std::uint64_t ensure_rider(int slot, std::uint64_t forced_id = 0);
  // One tick of `in` for rider `id` exactly as step() moves riders.
  void predict_rider(std::uint64_t id, const PlayerInput& in);
  // Every live actor as a snapshot row (id order of the world).
  void net_capture(std::vector<NetActor>* out) const;
  // Make the world match `rows`: move / flag known actors, create unknown
  // ones (spawned copies by template name, cloned riders by slot), drop
  // actors that are missing. prev = current (no tick interpolation).
  void net_apply(const std::vector<NetActor>& rows);
  // Advance animators one tick without simulating (client side).
  void net_animate();

  // Quads for everything visible in `view`, back to front. Cameras and
  // spawn points are editor markers and do not draw, nor do hidden
  // entities. Animators draw the current frame of their sheet in place of
  // the sprite image.
  void build_draw_list(const WorldRect& view, float alpha,
                       const ImageSizeFn& image_size,
                       std::vector<DrawQuad>* out) const;

 private:
  struct CameraRig {
    std::uint64_t entity = 0;  // camera entity (0 = implicit)
    std::uint64_t target = 0;  // followed entity (0 = none)
    Camera2DData data;
    float x = 0.0f;
    float y = 0.0f;
    float prev_x = 0.0f;
    float prev_y = 0.0f;
  };

  struct SolidGrid {
    std::size_t actor = 0;  // TileMap actor (tilemaps never move)
    SolidTable table;
  };

  void setup_camera();
  void setup_collision();
  // Move by (dx, dy) with collision when the actor has a solid collider.
  void move_body(Actor& a, float dx, float dy);
  // After moving `delta` along axis (0 = x, 1 = y), back out of anything
  // solid the move entered.
  void resolve_axis(Actor& a, int axis, float delta);
  void separate_dynamics();
  void update_triggers();
  void step_players(const InputFrame& input);
  // One rider, one tick of input (step_players / predict_rider).
  void move_rider(Actor& a, const PlayerInput& in);
  void step_camera();
  void setup_animation();
  void setup_actor_animation(Actor& a);
  void setup_scripts();
  void step_scripts(const InputFrame& input);
  // Drop destroyed actors, re-index collision + draw order after spawns,
  // destroys and collider toggles.
  void apply_changes();
  // Children (Entity2D::parent) ride along with their parent's motion
  // this tick, parents first.
  void follow_parents();
  std::uint64_t add_actor(Entity2D data);
  Actor* live(std::uint64_t id);
  const Actor* live(std::uint64_t id) const;
  void post(LogLevel level, const std::string& channel, std::string text,
            std::uint64_t entity);
  void step_animation();
  // Switch an actor's clip; time carries over between directions of one
  // move (walk_left -> walk_up) and resets otherwise.
  static void set_clip(AnimState& st, const std::string& clip, bool flip_x,
                       bool restart);
  void clamp_camera(float* x, float* y) const;
  bool target_center(float* x, float* y) const;

  std::vector<Actor> actors_;
  std::vector<std::size_t> draw_order_;
  CameraRig cam_;
  float view_w_ = 1280.0f;
  float view_h_ = 720.0f;
  std::uint64_t tick_ = 0;
  std::string project_dir_;

  // Collision state (rebuilt by build()).
  TileSolidity solidity_;
  std::vector<SolidGrid> grids_;
  std::vector<std::size_t> static_solids_;  // actor indices
  std::vector<std::size_t> dynamics_;       // dynamic solid actors
  std::vector<std::size_t> triggers_;       // trigger actors
  float step_limit_ = 1.0e9f;
  // (trigger id, other id) pairs overlapping after the latest tick, sorted.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> inside_;
  std::vector<TriggerEvent> events_;
  std::optional<TriggerEvent> last_event_;
  std::uint64_t events_total_ = 0;

  // Animation sets used by this build (copied out of the library so the
  // world stays self-contained); actors index into it.
  std::vector<AnimLibrary::Entry> anim_sets_;
  std::vector<std::string> anim_paths_;
  AnimLibrary anim_lib_;

  // Scripts. The library is declared first so the host (which reads it)
  // goes first on teardown.
  ScriptLibrary script_lib_;
  std::unique_ptr<ScriptHost> scripts_;
  std::vector<Entity2D> authored_;  // spawn() templates, as built
  std::shared_ptr<const WorldExtensions> ext_;
  std::vector<bool> ext_dead_;  // per ext_->systems entry, this ride
  void step_extensions();
  std::vector<LogEntry> logs_;
  std::uint64_t log_total_ = 0;
  std::vector<Toast> toasts_;
  std::uint64_t next_id_ = 1;
  std::array<std::uint32_t, kMaxPlayers> prev_buttons_{};
  InputFrame input_{};
  NetRole net_role_ = NetRole::Local;
  std::vector<NetPost> net_posts_;
  std::vector<SoundEvent> sounds_;
  std::uint64_t sound_total_ = 0;
  // Plugin blockers' boxes near the body being moved (reused buffer).
  std::vector<Aabb> blocker_boxes_;
  bool in_step_ = false;  // script_tick() = the tick being simulated
  bool dirty_ = false;    // apply_changes() has work
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
