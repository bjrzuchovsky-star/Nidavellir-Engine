#pragma once

// Authored scene data: what scene.json stores and what both the editor
// (Workspace2D) and the runtime (runtime::World) start from. Plain structs,
// no UI and no graphics API.

#include "scene/TileMap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {

// Local player slots. Slot 0 is the keyboard rider; 1-3 are reserved for
// gamepads now and networked riders later.
inline constexpr int kMaxPlayers = 4;

// Moves the entity from a player's input (scene.json "player").
struct PlayerControllerData {
  static constexpr float kMinSpeed = 1.0f;
  static constexpr float kMaxSpeed = 5000.0f;

  int slot = 0;          // 0..kMaxPlayers-1
  float speed = 160.0f;  // world px per second at full stick

  bool operator==(const PlayerControllerData& o) const = default;
};

// Follow camera (scene.json "camera"). The camera looks at the centre of
// `target` (0 = stay put at the entity's own centre).
struct Camera2DData {
  std::uint64_t target = 0;  // entity id to follow
  float smoothing = 0.15f;   // catch-up time in seconds, 0 = locked on
  float zoom = 1.0f;         // screen px per world px
  // Optional world-space box the view may not leave.
  bool use_bounds = false;
  float bounds_x = 0.0f;
  float bounds_y = 0.0f;
  float bounds_w = 1024.0f;
  float bounds_h = 768.0f;

  bool operator==(const Camera2DData& o) const = default;
};

// Where player `slot` starts when play begins (scene.json "spawn").
struct SpawnPointData {
  int slot = 0;

  bool operator==(const SpawnPointData& o) const = default;
};

// Axis-aligned collision box (scene.json v4 "collider"), relative to the
// entity's top-left corner. Solid colliders block; triggers only report
// overlaps. Static bodies never move; dynamic ones (players, pushable
// things) are moved and separated by the runtime.
struct ColliderData {
  static constexpr float kMinSize = 1.0f;
  static constexpr float kMaxSize = 16384.0f;

  float offset_x = 0.0f;
  float offset_y = 0.0f;
  float w = 32.0f;
  float h = 32.0f;
  bool trigger = false;  // false = solid
  bool dynamic = false;  // false = static

  bool operator==(const ColliderData& o) const = default;
};

// Plays clips from an animation set (scene.json v5 "animator"). The set
// is a project-relative .anim.json next to a sprite sheet (see
// scene/Animation.h); several entities can share one. Players pick their
// clips from movement at runtime (idle / walk + _down/_up/_left/_right).
struct AnimatorData {
  static constexpr float kMaxSpeed = 16.0f;

  std::string set;           // project-relative, e.g. "assets/rider.anim.json"
  std::string clip;          // current clip; the ride starts on it ("" = default)
  std::string default_clip;  // start / fallback clip ("" = the set's default)
  float speed = 1.0f;        // playback rate, 0..kMaxSpeed
  bool playing = true;       // false holds the current frame

  bool operator==(const AnimatorData& o) const = default;
};

// Plays a sound from the entity (scene.json v8 2D "audio", v10 3D). clip is
// a project-relative wav / ogg / mp3 / flac or "builtin:<name>"; bus is
// master/music/sfx/ui/voice. spatial sources pan and fade by distance from
// the listener (camera / HMD), within min..max_distance (pixels in 2D,
// metres in 3D). autoplay starts it when the ride starts (loop keeps it
// going until the entity is destroyed or hidden).
struct AudioSourceData {
  static constexpr float kMaxVolume = 2.0f;

  std::string clip;
  std::string bus = "sfx";
  float volume = 1.0f;
  float pitch = 1.0f;
  bool loop = true;
  bool autoplay = true;
  bool spatial = true;
  float min_distance = 160.0f;
  float max_distance = 900.0f;
  bool doppler = false;

  bool operator==(const AudioSourceData& o) const = default;
};
// Clamp volume / pitch / distances; unknown bus -> "sfx".
void normalize_audio(AudioSourceData& a);

// A value scripts read and write: per-entity script properties (scene.json
// v6) and the runtime's per-entity key/value state. Only the field that
// matches `type` is meaningful (the others stay at their defaults).
struct ScriptValue {
  enum class Type : std::uint8_t { Bool, Number, Text };

  Type type = Type::Number;
  bool flag = false;
  double number = 0.0;
  std::string text;

  static ScriptValue of_bool(bool v);
  static ScriptValue of_number(double v);
  static ScriptValue of_text(std::string v);

  bool operator==(const ScriptValue& o) const = default;
};
// "bool" / "number" / "text".
const char* to_string(ScriptValue::Type type);
// The value as a short display string ("24", "true", "The gate creaks").
std::string describe(const ScriptValue& value);

// One named script property override on an entity.
struct ScriptProp {
  std::string name;
  ScriptValue value;

  bool operator==(const ScriptProp& o) const = default;
};

// Runs a gameplay script (scene.json v6 "script"). The file lives under
// <project>/scripts/ and declares its properties with defaults; `props`
// holds this entity's overrides, kept sorted by name.
struct ScriptData {
  static constexpr std::size_t kMaxProps = 64;
  static constexpr std::size_t kMaxTextLength = 1024;

  std::string path;  // project-relative, e.g. "scripts/gate.lua"
  std::vector<ScriptProp> props;

  const ScriptProp* find(const std::string& name) const;
  // Add or replace an override (keeps the list sorted).
  void set(const std::string& name, ScriptValue value);
  // Drop an override; false when there was none.
  bool erase(const std::string& name);

  bool operator==(const ScriptData& o) const = default;
};

// One entity in a flat scene under the conceptual root. Transform is the
// x/y/w/h rect (y grows down); everything else is an optional component.
struct Entity2D {
  std::uint64_t id = 0;
  std::string name;
  float x = 0.0f;
  float y = 0.0f;
  float w = 64.0f;
  float h = 64.0f;
  float color[4] = {0.35f, 0.65f, 0.95f, 1.0f};  // RGBA tint
  int layer = 0;                                  // z-order (higher draws later)
  // A TileMap entity's w/h follow its grid; a sprite draws a textured quad
  // tinted by `color` (falls back to the rect).
  std::optional<TileMapData> tilemap;
  std::optional<SpriteData> sprite;
  // Gameplay components (scene.json v3).
  std::optional<PlayerControllerData> player;
  std::optional<Camera2DData> camera;
  std::optional<SpawnPointData> spawn;
  // Collision (scene.json v4).
  std::optional<ColliderData> collider;
  // Sprite animation (scene.json v5).
  std::optional<AnimatorData> animator;
  // Gameplay script (scene.json v6).
  std::optional<ScriptData> script;
  // Editor tools (scene.json v7). rotation turns the drawn quad about its
  // centre (colliders stay axis-aligned). parent: the entity this one hangs
  // under in the Hierarchy; positions stay in world space and follow the
  // parent's moves (editor drags, and the runtime each tick). locked /
  // hidden only affect the edit viewport (no picking / not drawn).
  float rotation = 0.0f;       // degrees, clockwise on screen
  std::uint64_t parent = 0;    // 0 = scene root
  bool locked = false;
  bool hidden = false;
  // Sound source (scene.json v8).
  std::optional<AudioSourceData> audio;

  float center_x() const { return x + w * 0.5f; }
  float center_y() const { return y + h * 0.5f; }
  bool has_gameplay() const { return player || camera || spawn; }
};

bool operator==(const Entity2D& a, const Entity2D& b);
inline bool operator!=(const Entity2D& a, const Entity2D& b) {
  return !(a == b);
}

// Keep w/h equal to the TileMap grid extent (no-op without a tilemap).
void sync_tilemap_extent(Entity2D& e);

// A collider covering the whole entity rect: dynamic for players, static
// otherwise. What "Add Collider" and the v3 -> v4 upgrade hand out.
ColliderData default_collider(const Entity2D& e);

// Clamp component fields to their valid ranges (slots, speed, zoom...).
void normalize_components(Entity2D& e);
// Sort props by name, drop blank / duplicate names and non-finite numbers,
// cap counts and text lengths.
void normalize_script(ScriptData& s);

// Lookup helpers over a flat entity list.
const Entity2D* find_entity(const std::vector<Entity2D>& entities,
                            std::uint64_t id);
const Entity2D* find_entity_named(const std::vector<Entity2D>& entities,
                                  const std::string& name);

// Indices sorted by layer ascending, stable by id (the shared draw order).
std::vector<std::size_t> draw_order(const std::vector<Entity2D>& entities);

}  // namespace tombstone
}  // namespace ts
