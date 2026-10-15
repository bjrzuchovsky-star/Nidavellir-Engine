#include "weather/Weather3D.h"
#include "weather/WeatherLua.h"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace weather {

using render3d::Environment;
using render3d::Vec3;

namespace {
constexpr float kPi = 3.14159265358979f;
void mix3(float* d, const float* a, const float* b, float t) {
  for (int i = 0; i < 3; ++i) d[i] = a[i] + (b[i] - a[i]) * t;
}
std::uint32_t h32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}
float hu(std::uint32_t i, std::uint32_t salt) { return static_cast<float>(h32(i * 0x9E3779B9u + salt) >> 8) / 16777216.0f; }
float wrap(float v, float span) { return v - std::floor(v / span) * span; }
}  // namespace

void apply_environment(const State& s, const Config& c, const Environment& base, Environment* out) {
  Environment e = base;
  const Mix& m = s.mix;
  float day = 1.0f;
  if (c.time_link) {
    day = s.daylight;
    // Sun path: rises in the east (+x) at 0.25, overhead at noon, sets west.
    const float a = (s.time_of_day - 0.25f) * 2.0f * kPi;
    const float el = std::sin(a);
    const Vec3 to_sun = render3d::normalize(Vec3{std::cos(a), std::max(el, 0.06f), 0.35f});
    e.sun_direction = to_sun * -1.0f;
    const float night_top[3] = {0.015f, 0.02f, 0.06f}, night_hor[3] = {0.04f, 0.05f, 0.10f};
    const float noon_hor[3] = {0.62f, 0.72f, 0.86f};
    // Warm horizon near dawn / dusk (the base sky), bluer at noon.
    const float high = std::clamp(el * 1.6f, 0.0f, 1.0f);
    float hor[3];
    mix3(hor, base.sky_horizon, noon_hor, high * 0.6f);
    mix3(e.sky_top, night_top, base.sky_top, day);
    mix3(e.sky_horizon, night_hor, hor, day);
    const float sunk = std::clamp(el * 4.0f, 0.0f, 1.0f);
    e.sun_intensity = base.sun_intensity * sunk;
    const float warm[3] = {1.0f, 0.55f, 0.30f};
    mix3(e.sun_color, warm, base.sun_color, std::clamp(el * 2.5f, 0.0f, 1.0f));
    e.ambient = base.ambient * (0.22f + 0.78f * day);
  }
  // Overcast.
  const float grey[3] = {0.46f * (0.15f + 0.85f * day), 0.48f * (0.15f + 0.85f * day), 0.52f * (0.15f + 0.85f * day)};
  mix3(e.sky_top, e.sky_top, grey, m.cloud * 0.85f);
  mix3(e.sky_horizon, e.sky_horizon, grey, m.cloud * 0.7f);
  e.sun_intensity *= 1.0f - 0.8f * m.cloud;
  e.ambient *= 1.0f - 0.15f * m.cloud;
  // Fog / dust haze.
  const float haze = m.fog * 0.05f + m.dust * 0.035f + m.rain * 0.006f + m.snow * 0.014f;
  if (haze > 0.0005f) {
    float col[3];
    const float mist[3] = {0.66f, 0.69f, 0.73f}, dust[3] = {0.74f, 0.58f, 0.38f};
    const float dw = m.dust / std::max(0.001f, m.dust + m.fog + m.rain + m.snow);
    mix3(col, mist, dust, dw);
    for (float& v : col) v *= 0.12f + 0.88f * day;
    if (e.fog_density > 0) mix3(e.fog_color, e.fog_color, col, std::min(1.0f, haze * 40.0f));
    else for (int i = 0; i < 3; ++i) e.fog_color[i] = col[i];
    e.fog_density = std::max(e.fog_density, haze);
    if (e.fog_falloff <= 0) {
      e.fog_height = base.fog_height;
      e.fog_falloff = m.dust > m.fog ? 0.02f : 0.04f;
    }
    mix3(e.sky_horizon, e.sky_horizon, col, std::min(0.8f, haze * 18.0f));
  }
  // Lightning.
  if (s.flash > 0) {
    const float f = s.flash;
    e.ambient += f * 1.8f;
    const float bolt[3] = {0.85f, 0.88f, 1.0f};
    mix3(e.sky_top, e.sky_top, bolt, f * 0.7f);
    mix3(e.sky_horizon, e.sky_horizon, bolt, f * 0.7f);
    if (e.fog_density > 0) mix3(e.fog_color, e.fog_color, bolt, f * 0.5f);
  }
  // Wind.
  e.wind_dir[0] = s.wind_dir[0];
  e.wind_dir[1] = s.wind_dir[1];
  e.wind_strength = s.wind;
  e.wind_gust = c.gust * 0.6f;
  *out = e;
}

namespace {
Weather3D* g_latest = nullptr;
}

// ---- Extension ---------------------------------------------------------------

void Weather3D::on_start(world3d::World3D& w) {
  g_latest = this;
  Config c = cfg_;
  if (!have_cfg_) {
    std::string err;
    if (!load_config(w.project_dir(), &c, &err)) {
      runtime::LogEntry le;
      le.level = runtime::LogLevel::Warn;
      le.channel = "weather";
      le.text = err + " (using defaults)";
      w.script_log(le);
      c = Config{};
    }
  }
  sim_.configure(c);
  base_ = w.environment();
  announced_ = sim_.state().target;
  strikes_seen_ = 0;
  thunder_in_ = -1;
  apply_environment(sim_.state(), sim_.config(), base_, &w.environment_mut());
}

void Weather3D::on_tick(world3d::World3D& w, double dt) {
  sim_.step(dt);
  const State& s = sim_.state();
  apply_environment(s, sim_.config(), base_, &w.environment_mut());
  if (s.target != announced_) {
    announced_ = s.target;
    w.script_broadcast("on_weather", kind_name(s.target), static_cast<double>(s.mix.storm));
  }
  if (s.strikes != strikes_seen_) {
    strikes_seen_ = s.strikes;
    w.script_broadcast("on_lightning", "", static_cast<double>(s.strikes));
    if (sim_.config().thunder) {
      // Sound travels: the rumble follows the flash.
      thunder_in_ = 0.4f + 1.6f * s.strike_at;
      thunder_volume_ = 0.6f + 0.4f * (1.0f - s.strike_at);
    }
  }
  if (thunder_in_ >= 0) {
    thunder_in_ -= static_cast<float>(dt);
    if (thunder_in_ < 0) {
      runtime::SoundEvent ev;
      ev.clip = "builtin:boom";
      ev.volume = thunder_volume_;
      ev.pitch = 0.45f;
      ev.variation = 0.1f;
      w.play_sound(ev);
    }
  }
}

void Weather3D::on_stop(world3d::World3D& w) {
  (void)w;
  thunder_in_ = -1;
}

void Weather3D::billboards(const world3d::World3D& w, std::vector<render3d::Renderer3D::Billboard>* out) const {
  const State& s = sim_.state();
  const Mix& m = s.mix;
  if (m.rain + m.snow + m.dust < 0.01f) return;
  const Vec3 cam = w.camera().position;
  const float t = static_cast<float>(s.time);
  const float wx = s.wind_dir[0] * s.wind, wz = s.wind_dir[1] * s.wind;
  const float day = 0.25f + 0.75f * s.daylight;
  const float lit = std::min(1.0f, day + s.flash);
  auto place = [&](std::uint32_t i, std::uint32_t salt, float span, float height, float vx, float vy, float vz,
                   Vec3* p) {
    // World-anchored lattice wrapped into a box around the camera.
    const float x0 = hu(i, salt) * span, z0 = hu(i, salt + 1) * span, y0 = hu(i, salt + 2) * height;
    p->x = wrap(x0 + vx * t - (cam.x - span * 0.5f), span) + cam.x - span * 0.5f;
    p->z = wrap(z0 + vz * t - (cam.z - span * 0.5f), span) + cam.z - span * 0.5f;
    p->y = wrap(y0 + vy * t - (cam.y - height * 0.5f), height) + cam.y - height * 0.5f;
  };
  // Rain: streaks along the fall velocity.
  const int rain = static_cast<int>(std::min(1.5f, m.rain) * 1600.0f);
  for (int i = 0; i < rain; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    const float speed = 10.0f + 3.0f * hu(u, 7);
    const float vx = wx * 7.0f, vz = wz * 7.0f;
    render3d::Renderer3D::Billboard b;
    place(u, 11, 34.0f, 18.0f, vx, -speed, vz, &b.position);
    b.size = 0.025f;
    b.stretch = Vec3{vx, -speed, vz} * 0.045f;
    b.rgba[0] = 0.72f * lit;
    b.rgba[1] = 0.78f * lit;
    b.rgba[2] = 0.88f * lit;
    b.rgba[3] = 0.42f;
    out->push_back(b);
  }
  // Snow: slow flakes that wobble.
  const int snow = static_cast<int>(std::min(1.5f, m.snow) * 1400.0f);
  for (int i = 0; i < snow; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    const float fall = 0.8f + 0.7f * hu(u, 21);
    render3d::Renderer3D::Billboard b;
    place(u, 23, 30.0f, 16.0f, wx * 2.5f, -fall, wz * 2.5f, &b.position);
    const float ph = hu(u, 25) * 6.28f;
    b.position.x += 0.35f * std::sin(t * 1.3f + ph);
    b.position.z += 0.35f * std::cos(t * 1.1f + ph);
    b.size = 0.05f + 0.05f * hu(u, 27);
    b.rgba[0] = b.rgba[1] = b.rgba[2] = 0.95f * lit;
    b.rgba[3] = 0.9f;
    out->push_back(b);
  }
  // Dust: big soft puffs racing along the wind near the ground.
  const int dust = static_cast<int>(std::min(1.5f, m.dust) * 700.0f);
  for (int i = 0; i < dust; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    const float sp = 10.0f + 8.0f * hu(u, 31);
    render3d::Renderer3D::Billboard b;
    place(u, 33, 44.0f, 10.0f, wx * sp, 0.3f * std::sin(static_cast<float>(i)), wz * sp, &b.position);
    b.size = 0.6f + 1.6f * hu(u, 35);
    b.rotation = hu(u, 37) * 360.0f + t * 20.0f;
    b.rgba[0] = 0.72f * day;
    b.rgba[1] = 0.56f * day;
    b.rgba[2] = 0.36f * day;
    b.rgba[3] = 0.16f;
    out->push_back(b);
  }
}

void Weather3D::open_lua(world3d::World3D& w, lua_State* L, int sandbox) {
  (void)w;
  push_weather_lua(L, &sim_);
  lua_setfield(L, sandbox, "weather");
}

Weather3D::~Weather3D() {
  if (g_latest == this) g_latest = nullptr;
}

Weather3D* Weather3D::latest() { return g_latest; }

Weather3D* Weather3D::of(const world3d::World3D& w) {
  return dynamic_cast<Weather3D*>(w.extension(kName));
}

std::shared_ptr<Weather3D> attach(world3d::World3D& w, bool enabled) {
  if (!enabled) return nullptr;
  auto x = std::make_shared<Weather3D>();
  w.add_extension(x);
  return x;
}

}  // namespace weather
}  // namespace tombstone
}  // namespace ts
