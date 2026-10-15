#include "weather/Weather.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace weather {

namespace fs = std::filesystem;
using json::Value;

namespace {
constexpr float kPi = 3.14159265358979f;
float smooth(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }
}  // namespace

const std::vector<std::string>& kind_names() {
  static const std::vector<std::string> n = {"clear", "rain", "snow", "dust", "fog", "storm"};
  return n;
}

const char* kind_name(Kind k) {
  const auto i = static_cast<std::size_t>(k);
  return i < kind_names().size() ? kind_names()[i].c_str() : "clear";
}

bool kind_from(const std::string& name, Kind* out) {
  const auto& n = kind_names();
  for (std::size_t i = 0; i < n.size(); ++i) {
    if (n[i] == name) {
      *out = static_cast<Kind>(i);
      return true;
    }
  }
  if (name == "sand" || name == "sandstorm" || name == "dust_storm") {
    *out = Kind::Dust;
    return true;
  }
  if (name == "thunderstorm" || name == "thunder") {
    *out = Kind::Storm;
    return true;
  }
  return false;
}

Mix preset_mix(Kind k) {
  Mix m;
  switch (k) {
    case Kind::Clear: m.wind = 0.15f; break;
    case Kind::Rain: m.rain = 1; m.cloud = 0.7f; m.fog = 0.15f; m.wind = 0.35f; break;
    case Kind::Snow: m.snow = 1; m.cloud = 0.6f; m.fog = 0.3f; m.wind = 0.25f; break;
    case Kind::Dust: m.dust = 1; m.cloud = 0.25f; m.fog = 0.2f; m.wind = 0.95f; break;
    case Kind::Fog: m.fog = 1; m.cloud = 0.45f; m.wind = 0.05f; break;
    case Kind::Storm: m.rain = 1; m.storm = 1; m.cloud = 1; m.fog = 0.25f; m.wind = 0.8f; break;
  }
  return m;
}

float daylight_at(float t) {
  t = t - std::floor(t);
  // Up 0.2..0.3 (dawn), down 0.7..0.8 (dusk).
  if (t < 0.5f) return smooth((t - 0.2f) / 0.1f);
  return 1.0f - smooth((t - 0.7f) / 0.1f);
}

void normalize(Config& c) {
  c.intensity = std::clamp(c.intensity, 0.0f, 2.0f);
  c.transition = std::clamp(c.transition, 0.0f, 600.0f);
  for (CycleStep& s : c.steps) s.seconds = std::clamp(s.seconds, 1.0f, 36000.0f);
  if (c.steps.size() > 64) c.steps.resize(64);
  c.wind_scale = std::clamp(c.wind_scale, 0.0f, 4.0f);
  c.gust = std::clamp(c.gust, 0.0f, 1.0f);
  c.lightning_min = std::clamp(c.lightning_min, 0.3f, 600.0f);
  c.lightning_max = std::clamp(c.lightning_max, c.lightning_min, 600.0f);
  c.day_length = std::clamp(c.day_length, 10.0f, 86400.0f);
  c.start_time = c.start_time - std::floor(c.start_time);
  if (!std::isfinite(c.wind_dir)) c.wind_dir = 0;
}

std::string to_json(const Config& c) {
  Value v = Value::object();
  v.set("version", Value::number(1));
  v.set("start", Value::string(kind_name(c.start)));
  v.set("intensity", Value::number(c.intensity));
  v.set("transition", Value::number(c.transition));
  v.set("cycle", Value::boolean(c.cycle));
  Value steps = Value::array();
  for (const CycleStep& s : c.steps) {
    Value sv = Value::object();
    sv.set("kind", Value::string(kind_name(s.kind)));
    sv.set("seconds", Value::number(s.seconds));
    steps.push(std::move(sv));
  }
  v.set("steps", std::move(steps));
  v.set("wind_dir", Value::number(c.wind_dir));
  v.set("wind_scale", Value::number(c.wind_scale));
  v.set("gust", Value::number(c.gust));
  v.set("lightning_min", Value::number(c.lightning_min));
  v.set("lightning_max", Value::number(c.lightning_max));
  v.set("thunder", Value::boolean(c.thunder));
  v.set("time_link", Value::boolean(c.time_link));
  v.set("day_length", Value::number(c.day_length));
  v.set("start_time", Value::number(c.start_time));
  v.set("seed", Value::number(c.seed));
  return json::write(v);
}

bool from_json(const std::string& text, Config* out, std::string* error) {
  Value v;
  std::string err;
  if (!json::parse(text, &v, &err) || !v.is_object()) {
    if (error) *error = "weather.json: " + (err.empty() ? std::string("expected an object") : err);
    return false;
  }
  Config c;
  Kind k;
  if (kind_from(v.str("start", "clear"), &k)) c.start = k;
  c.intensity = v.flt("intensity", c.intensity);
  c.transition = v.flt("transition", c.transition);
  c.cycle = v.flag("cycle", c.cycle);
  for (const Value& sv : v["steps"].items()) {
    CycleStep s;
    if (!kind_from(sv.str("kind", "clear"), &s.kind)) continue;
    s.seconds = sv.flt("seconds", s.seconds);
    c.steps.push_back(s);
  }
  c.wind_dir = v.flt("wind_dir", c.wind_dir);
  c.wind_scale = v.flt("wind_scale", c.wind_scale);
  c.gust = v.flt("gust", c.gust);
  c.lightning_min = v.flt("lightning_min", c.lightning_min);
  c.lightning_max = v.flt("lightning_max", c.lightning_max);
  c.thunder = v.flag("thunder", c.thunder);
  c.time_link = v.flag("time_link", c.time_link);
  c.day_length = v.flt("day_length", c.day_length);
  c.start_time = v.flt("start_time", c.start_time);
  c.seed = static_cast<std::uint32_t>(std::max(0.0, v.num("seed", c.seed)));
  normalize(c);
  *out = c;
  return true;
}

bool load_config(const std::string& dir, Config* out, std::string* error) {
  const fs::path p = fs::path(dir) / kConfigFile;
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    *out = Config{};
    return true;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return from_json(ss.str(), out, error);
}

bool save_config(const std::string& dir, const Config& c, std::string* error) {
  const fs::path p = fs::path(dir) / kConfigFile;
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot write " + p.string();
    return false;
  }
  f << to_json(c);
  return static_cast<bool>(f);
}

// ---- Sim ---------------------------------------------------------------------

void Sim::configure(const Config& c) {
  cfg_ = c;
  normalize(cfg_);
  reset();
}

void Sim::reset() {
  st_ = State{};
  st_.kind = st_.target = cfg_.cycle && !cfg_.steps.empty() ? cfg_.steps[0].kind : cfg_.start;
  st_.blend = 1.0f;
  rng_ = cfg_.seed ? cfg_.seed * 2654435761u : 0x9E3779B9u;
  fade_ = cfg_.transition;
  cycle_index_ = 0;
  cycle_timer_ = 0;
  wind_override_ = -1;
  pulse_in_ = -1;
  tod_base_ = cfg_.start_time;
  tod_base_time_ = 0;
  schedule_strike();
  step(0.0);
}

float Sim::rnd() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<float>(rng_ >> 8) / 16777216.0f;
}

void Sim::schedule_strike() { next_strike_ = lerp(cfg_.lightning_min, cfg_.lightning_max, rnd()); }

void Sim::set(Kind k, float fade) {
  if (k == st_.target && st_.blend >= 1.0f) return;
  // Settle wherever we are now as the new starting point.
  if (st_.blend >= 0.5f) st_.kind = st_.target;
  st_.target = k;
  fade_ = fade < 0 ? cfg_.transition : fade;
  st_.blend = fade_ <= 0.0f ? 1.0f : 0.0f;
  if (st_.blend >= 1.0f) st_.kind = k;
}

void Sim::strike() {
  st_.flash = 1.0f;
  ++st_.strikes;
  st_.strike_at = 0.1f + 0.8f * rnd();
  st_.struck = true;
  pulse_in_ = 0.12f + 0.1f * rnd();
}

void Sim::set_time_of_day(float t) {
  tod_base_ = t - std::floor(t);
  tod_base_time_ = st_.time;
  st_.time_of_day = tod_base_;
  st_.daylight = cfg_.time_link ? daylight_at(st_.time_of_day) : 1.0f;
}

void Sim::set_wind(float dir_deg, float strength) {
  wind_override_dir_ = dir_deg;
  wind_override_ = strength;
}

void Sim::step(double dt_d) {
  const float dt = static_cast<float>(std::max(0.0, dt_d));
  st_.time += dt_d;
  st_.struck = false;
  // Cycle.
  if (cfg_.cycle && !cfg_.steps.empty()) {
    cycle_timer_ += dt;
    const CycleStep& cur = cfg_.steps[static_cast<std::size_t>(cycle_index_) % cfg_.steps.size()];
    if (cycle_timer_ >= cur.seconds) {
      cycle_timer_ = 0;
      cycle_index_ = (cycle_index_ + 1) % static_cast<int>(cfg_.steps.size());
      set(cfg_.steps[static_cast<std::size_t>(cycle_index_)].kind);
    }
  }
  // Transition.
  if (st_.blend < 1.0f) {
    st_.blend = fade_ <= 0 ? 1.0f : std::min(1.0f, st_.blend + dt / fade_);
    if (st_.blend >= 1.0f) st_.kind = st_.target;
  }
  const Mix a = preset_mix(st_.kind), b = preset_mix(st_.target);
  const float t = st_.kind == st_.target ? 1.0f : smooth(st_.blend);
  const float k = cfg_.intensity;
  Mix m;
  m.rain = std::min(1.5f, lerp(a.rain, b.rain, t) * k);
  m.snow = std::min(1.5f, lerp(a.snow, b.snow, t) * k);
  m.dust = std::min(1.5f, lerp(a.dust, b.dust, t) * k);
  m.fog = std::min(1.5f, lerp(a.fog, b.fog, t) * k);
  m.storm = std::min(1.5f, lerp(a.storm, b.storm, t) * k);
  m.cloud = std::min(1.0f, lerp(a.cloud, b.cloud, t) * std::min(1.0f, k));
  m.wind = lerp(a.wind, b.wind, t);
  st_.mix = m;
  // Wind with gusts (two slow sines + a little noise, deterministic).
  const float dir = (wind_override_ >= 0 ? wind_override_dir_ : cfg_.wind_dir) * kPi / 180.0f;
  st_.wind_dir[0] = std::cos(dir);
  st_.wind_dir[1] = std::sin(dir);
  const float base = wind_override_ >= 0 ? wind_override_ : m.wind * cfg_.wind_scale;
  const float tt = static_cast<float>(st_.time);
  const float g = 0.5f * std::sin(tt * 0.63f) + 0.3f * std::sin(tt * 1.71f + 1.3f) + 0.2f * std::sin(tt * 4.1f);
  st_.wind = std::max(0.0f, base * (1.0f + cfg_.gust * g));
  // Lightning.
  if (pulse_in_ >= 0) {
    pulse_in_ -= dt;
    if (pulse_in_ < 0) st_.flash = std::max(st_.flash, 0.75f);
  }
  st_.flash *= std::exp(-7.0f * dt);
  if (st_.flash < 0.002f) st_.flash = 0;
  if (m.storm > 0.3f) {
    next_strike_ -= dt * m.storm;
    if (next_strike_ <= 0) {
      strike();
      schedule_strike();
    }
  }
  // Time of day.
  st_.time_of_day = static_cast<float>(tod_base_ + (st_.time - tod_base_time_) / cfg_.day_length);
  st_.time_of_day -= std::floor(st_.time_of_day);
  st_.daylight = cfg_.time_link ? daylight_at(st_.time_of_day) : 1.0f;
}

double Sim::checksum() const {
  return st_.mix.rain * 3 + st_.mix.snow * 5 + st_.mix.dust * 7 + st_.mix.fog * 11 + st_.wind * 13 +
         st_.flash * 17 + static_cast<double>(st_.strikes) * 19 + st_.time_of_day * 23;
}

}  // namespace weather
}  // namespace tombstone
}  // namespace ts
