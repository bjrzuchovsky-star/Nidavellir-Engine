#pragma once
// Particles: emitter settings shared by the 2D and 3D scenes (the
// "particles" component), presets, and a small deterministic simulator.
// Units are the scene's: pixels with y down in 2D, metres with y up in 3D
// (preset() converts). The simulator only knows vectors, so one codepath
// drives ts_game, ts_client, the 3D/VR runtime and the editor previews.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace fx {

enum class EmitShape { Point, Circle, Box, Line };
enum class Blend { Alpha, Additive };
enum class Space { TwoD, ThreeD };

struct EmitterDesc {
  static constexpr int kMaxParticles = 2048;

  std::string preset;          // name it was made from (informational)
  bool enabled = true;         // off: no new particles (live ones finish)
  bool continuous = true;      // emit `rate` per second while running
  float rate = 20.0f;          // particles per second (continuous)
  int burst = 0;               // particles at once when the emitter starts
  float duration = 0.0f;       // seconds of emission; 0 = forever
  int max_particles = 256;
  // Where particles appear, around the emitter origin.
  EmitShape shape = EmitShape::Point;
  float radius = 0.0f;         // circle / sphere radius
  float extent[3] = {0, 0, 0}; // box half size; line runs along x
  // Launch: direction +- spread degrees, speed range.
  float direction[3] = {0, -1, 0};
  float spread = 25.0f;
  float speed_min = 40.0f;
  float speed_max = 80.0f;
  float gravity[3] = {0, 0, 0};
  float drag = 0.0f;           // velocity loss per second (0..20)
  float life_min = 0.6f;
  float life_max = 1.2f;
  // Over life: 3-key curves (start, mid at mid_t, end).
  float mid_t = 0.5f;
  float size[3] = {6.0f, 8.0f, 10.0f};
  float color_start[4] = {1, 1, 1, 1};
  float color_mid[4] = {1, 1, 1, 0.8f};
  float color_end[4] = {1, 1, 1, 0};
  float spin = 0.0f;           // max deg/s either way
  // Look: "" = built-in soft dot; or a project image cut in frames_x by
  // frames_y cells played over life (frame_fps 0) or at frame_fps.
  std::string texture;
  int frames_x = 1;
  int frames_y = 1;
  float frame_fps = 0.0f;
  Blend blend = Blend::Alpha;
  bool local_space = false;    // particles ride along with the emitter
  std::uint32_t seed = 0;      // 0: derived from the entity id

  bool operator==(const EmitterDesc& o) const = default;
};

// Clamp counts, ranges and curves into sane bounds.
void normalize(EmitterDesc& d);

// The presets: dust, gun_smoke, muzzle_flash, sparks, campfire, rain,
// sand_storm, thruster, explosion, sparkle.
const std::vector<std::string>& preset_names();
std::optional<EmitterDesc> preset(const std::string& name, Space space);

// Scene file fields: only non-default values, key -> JSON text (strings
// already quoted). from_fields reads values back with a lookup that
// returns the raw JSON text (numbers / booleans) or the unquoted string.
struct Field {
  std::string key;
  std::string json;   // ready-to-write JSON value
};
std::vector<Field> to_fields(const EmitterDesc& d);
using FieldLookup = std::function<std::optional<std::string>(const std::string& key)>;
EmitterDesc from_fields(const FieldLookup& get);

// Built-in soft round dot (64x64 RGBA) published as kDotImage.
inline constexpr const char* kDotImage = "mem:fx/dot";
void ensure_builtin_images();

struct Sprite {
  float pos[3] = {0, 0, 0};
  float size = 1.0f;
  float rotation = 0.0f;     // degrees
  float rgba[4] = {1, 1, 1, 1};
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  const std::string* image = nullptr;  // kDotImage or the desc texture
  bool additive = false;
};

// Deterministic xorshift.
struct Rng {
  std::uint32_t s = 0x9E3779B9u;
  explicit Rng(std::uint32_t seed = 1) : s(seed ? seed : 0x9E3779B9u) {}
  std::uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float unit() { return static_cast<float>(next() >> 8) / 16777216.0f; }
  float range(float a, float b) { return a + (b - a) * unit(); }
};

class Emitter {
 public:
  Emitter() = default;
  Emitter(const EmitterDesc& desc, std::uint32_t seed, Space space = Space::TwoD);
  void set_desc(const EmitterDesc& desc);  // keeps live particles
  const EmitterDesc& desc() const { return desc_; }
  void restart();
  void stop() { stopped_ = true; }
  bool stopped() const { return stopped_; }
  // Advance dt seconds with the emitter at origin.
  void update(float dt, const float origin[3]);
  void emit(int count, const float origin[3]);
  int alive() const { return static_cast<int>(parts_.size()); }
  bool finished() const;  // stopped / out of duration and nothing alive
  void sprites(std::vector<Sprite>* out) const;
  // Sum of positions (determinism checks).
  double checksum() const;
  // Extra acceleration on every particle (wind), units / s^2.
  void set_wind(const float a[3]) {
    for (int i = 0; i < 3; ++i) wind_[i] = a[i];
  }

 private:
  struct Particle {
    float p[3], v[3];
    float age, life, scale, rot, spin;
    std::uint16_t frame0;
  };
  void spawn(const float origin[3]);

  EmitterDesc desc_;
  std::vector<Particle> parts_;
  Rng rng_;
  Space space_ = Space::TwoD;
  float origin_[3] = {0, 0, 0};
  double time_ = 0.0;
  float accum_ = 0.0f;
  float wind_[3] = {0, 0, 0};
  bool started_ = false;
  bool stopped_ = false;
};

// Every emitter of a running world: per-entity component emitters plus
// one-shot bursts (Lua, gameplay, network events) that retire when done.
class ParticleWorld {
 public:
  explicit ParticleWorld(Space space = Space::TwoD) : space_(space) {}
  Space space() const { return space_; }
  void clear();
  // A component emitter (created on first sight; desc changes apply live).
  void sync(std::uint64_t entity, const EmitterDesc& desc, const float pos[3]);
  void drop(std::uint64_t entity);
  // Entities not synced since the last prune() go away.
  void prune();
  void stop(std::uint64_t entity);   // stop emitting (live ones finish)
  void start(std::uint64_t entity);  // restart a stopped emitter
  bool has(std::uint64_t entity) const { return entities_.count(entity) != 0; }
  // One-shot: count particles (0 = the desc's burst / rate) at pos.
  void burst(const EmitterDesc& desc, const float pos[3], int count = 0);
  // Extra particles from an entity emitter.
  bool emit(std::uint64_t entity, int count);
  void update(float dt);
  void sprites(std::vector<Sprite>* out) const;
  int alive() const;
  int emitters() const { return static_cast<int>(entities_.size() + shots_.size()); }
  // Wind / weather push on every particle (units / s^2; 0 = still air).
  void set_wind(float x, float y, float z) {
    wind_[0] = x;
    wind_[1] = y;
    wind_[2] = z;
  }
  const float* wind() const { return wind_; }
  double checksum() const;

 private:
  struct Entry {
    Emitter em;
    float pos[3] = {0, 0, 0};
    bool seen = true;
  };
  Space space_;
  std::unordered_map<std::uint64_t, Entry> entities_;
  std::vector<std::uint64_t> order_;  // deterministic iteration
  std::vector<Entry> shots_;
  std::uint32_t shot_seed_ = 0x51ED27u;
  float wind_[3] = {0, 0, 0};
};

}  // namespace fx
}  // namespace ts
