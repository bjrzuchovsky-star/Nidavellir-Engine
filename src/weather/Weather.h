#pragma once

// ts.weather core: a deterministic weather simulation shared by 2D and 3D.
// Kinds blend over a transition (rain, snow, dust storms, fog, thunder
// storms with lightning), the wind gusts, and an optional time-of-day
// clock links the sky to the hour. No rendering here: the 2D plugin
// (plugins/weather) and the 3D World extension (weather/Weather3D) turn a
// State into particles, light and fog.
//
// Project settings live in weather.json at the project root.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace weather {

enum class Kind : std::uint8_t { Clear, Rain, Snow, Dust, Fog, Storm };
const char* kind_name(Kind k);
bool kind_from(const std::string& name, Kind* out);
const std::vector<std::string>& kind_names();

// How much of each effect is in the air (0..1).
struct Mix {
  float rain = 0, snow = 0, dust = 0, fog = 0, storm = 0;
  float cloud = 0;  // overcast: dims the sun, greys the sky
  float wind = 0;   // preset wind strength (before the project's scale)
};
Mix preset_mix(Kind k);

struct CycleStep {
  Kind kind = Kind::Clear;
  float seconds = 60.0f;
  bool operator==(const CycleStep& o) const = default;
};

struct Config {
  Kind start = Kind::Clear;
  float intensity = 1.0f;      // 0..2 scales every effect
  float transition = 8.0f;     // seconds to blend between kinds
  bool cycle = false;          // walk `steps` in a loop
  std::vector<CycleStep> steps;
  float wind_dir = 30.0f;      // degrees from +x towards +z (3D) / +y (2D)
  float wind_scale = 1.0f;     // multiplies each kind's wind
  float gust = 0.35f;          // 0..1 gustiness
  float lightning_min = 4.0f;  // seconds between strikes in a full storm
  float lightning_max = 11.0f;
  bool thunder = true;         // play "builtin:boom" low after a strike
  bool time_link = false;      // run the time-of-day clock (sky follows the hour)
  float day_length = 600.0f;   // seconds per whole day
  float start_time = 0.35f;    // 0 midnight, 0.25 dawn, 0.5 noon, 0.75 dusk
  std::uint32_t seed = 1;
  bool operator==(const Config& o) const = default;
};
void normalize(Config& c);
std::string to_json(const Config& c);
bool from_json(const std::string& text, Config* out, std::string* error);
inline constexpr const char* kConfigFile = "weather.json";
// Missing file = defaults (true). False only for a broken file.
bool load_config(const std::string& project_dir, Config* out, std::string* error);
bool save_config(const std::string& project_dir, const Config& c, std::string* error);

struct State {
  Kind kind = Kind::Clear;    // settled kind (the one we blend from)
  Kind target = Kind::Clear;  // heading towards
  float blend = 1.0f;         // 0..1 progress to target
  Mix mix;                    // current amounts (intensity applied)
  float wind_dir[2] = {1, 0}; // unit (x, z) in 3D / (x, y) in 2D
  float wind = 0;             // strength now, gusts included (0 calm .. ~1.5)
  float flash = 0;            // lightning brightness 0..1
  std::uint64_t strikes = 0;
  float strike_at = 0.5f;     // 0..1 across the view (where the bolt fell)
  bool struck = false;        // a strike began this step
  double time = 0;            // seconds simulated
  float time_of_day = 0.5f;   // 0..1
  float daylight = 1.0f;      // 0 night .. 1 day (from time_of_day when linked)
};

// 0 night .. 1 day, smooth around dawn (0.25) and dusk (0.75).
float daylight_at(float time_of_day);

class Sim {
 public:
  Sim() { configure(Config{}); }
  void configure(const Config& c);  // resets
  const Config& config() const { return cfg_; }
  void reset();
  // Head to `k` over `fade` seconds (< 0: the configured transition).
  void set(Kind k, float fade = -1.0f);
  void step(double dt);
  void strike();  // lightning now (any weather)
  void set_time_of_day(float t);
  // Override the wind; strength < 0 goes back to the weather's own.
  void set_wind(float dir_deg, float strength);
  void set_intensity(float k) { cfg_.intensity = k < 0 ? 0 : (k > 2 ? 2 : k); }
  const State& state() const { return st_; }
  // Determinism checks.
  double checksum() const;
  // Called after a script's weather.set (network relay: server -> clients).
  std::function<void(Kind kind, float fade)> on_script_set;

 private:
  float rnd();
  void schedule_strike();
  Config cfg_;
  State st_;
  float fade_ = 8.0f;
  std::uint32_t rng_ = 1;
  float next_strike_ = 6.0f;
  float pulse_in_ = -1.0f;  // second flicker of a strike
  int cycle_index_ = 0;
  float cycle_timer_ = 0.0f;
  float wind_override_dir_ = 0.0f;
  float wind_override_ = -1.0f;
  double tod_base_time_ = 0.0;
  float tod_base_ = 0.35f;
};

}  // namespace weather
}  // namespace tombstone
}  // namespace ts
