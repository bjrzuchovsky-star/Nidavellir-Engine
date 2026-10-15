#include "plugins/weather/WeatherPlugin.h"

#include "fx/Light2D.h"
#include "fx/Particles.h"
#include "plugin/Catalog.h"
#include "runtime/World.h"
#include "weather/WeatherLua.h"

#include "lua.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts {
namespace tombstone {
namespace plugins {

using weather::Kind;
using weather::State;

namespace {
WeatherPlugin::EditorHook& hook_ref() {
  static WeatherPlugin::EditorHook h;
  return h;
}
WeatherPlugin* g_active = nullptr;

constexpr const char* kChannel = "weather";
constexpr float kWindPx = 140.0f;  // px/s^2 particle push per unit of wind

std::uint32_t h32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}
float hu(std::uint32_t i, std::uint32_t salt) {
  return static_cast<float>(h32(i * 0x9E3779B9u + salt) >> 8) / 16777216.0f;
}
float wrap(float v, float span) { return v - std::floor(v / span) * span; }

const std::string& dot_image() {
  static const std::string s = fx::kDotImage;
  return s;
}

runtime::DrawQuad quad(float cx, float cy, float w, float h, const float rgba[4]) {
  runtime::DrawQuad q;
  q.x0 = cx - w * 0.5f;
  q.x1 = cx + w * 0.5f;
  q.y0 = cy - h * 0.5f;
  q.y1 = cy + h * 0.5f;
  for (int i = 0; i < 4; ++i) q.rgba[i] = rgba[i];
  return q;
}
}  // namespace

const plugin::PluginInfo& WeatherPlugin::static_info() {
  static const plugin::PluginInfo info = [] {
    plugin::PluginInfo i;
    i.id = "ts.weather";
    i.name = "Weather";
    i.description =
        "Rain, snow, dust storms, fog and thunder storms with lightning; gusting wind that sways 3D "
        "foliage and pushes particles; an optional time-of-day link for the sky. 2D: particles, "
        "lighting and a lightning light; 3D / VR: fog, overcast sky and billboards. weather.json + "
        "the `weather` Lua table.";
    i.author = "TombStone";
    i.version = plugin::Version{1, 0, 0};
    i.category = plugin::PluginCategory::Gameplay;
    i.project_types = plugin::kType2D | plugin::kType3D | plugin::kTypeVR;
    i.default_enabled = false;
    return i;
  }();
  return info;
}

void WeatherPlugin::set_editor_hook(EditorHook hook) { hook_ref() = std::move(hook); }
WeatherPlugin* WeatherPlugin::active() { return g_active; }

WeatherPlugin::~WeatherPlugin() {
  if (g_active == this) g_active = nullptr;
}

void WeatherPlugin::set_config(const weather::Config& c) {
  cfg_ = c;
  weather::normalize(cfg_);
  if (!playing_) sim_.configure(cfg_);
}

bool WeatherPlugin::save_config(std::string* error) const {
  if (dir_.empty()) {
    if (error) *error = "no project";
    return false;
  }
  return weather::save_config(dir_, cfg_, error);
}

bool WeatherPlugin::on_load(const plugin::PluginContext& ctx, std::string* error) {
  dir_ = ctx.project_dir;
  host_ = ctx.host;
  std::string err;
  if (!dir_.empty() && !weather::load_config(dir_, &cfg_, &err)) {
    // A broken weather.json should not stop the project: defaults + note.
    cfg_ = weather::Config{};
    if (error) *error = err;
  }
  sim_.configure(cfg_);
  g_active = this;
  return true;
}

void WeatherPlugin::on_register(plugin::Registrar& reg) {
  reg.add_component("weather", "Project weather (weather.json): rain, snow, dust, fog, storms, wind, time of day");
  reg.add_system("weather.sim", [this](runtime::World& w, double dt) {
    sim_.step(dt);
    const State& s = sim_.state();
    w.particles_mut().set_wind(s.wind_dir[0] * s.wind * kWindPx, s.wind_dir[1] * s.wind * kWindPx * 0.25f, 0.0f);
    if (s.target != announced_) {
      announced_ = s.target;
      w.script_broadcast("on_weather", weather::kind_name(s.target), static_cast<double>(s.mix.storm));
    }
    if (s.strikes != strikes_seen_) {
      strikes_seen_ = s.strikes;
      w.script_broadcast("on_lightning", "", static_cast<double>(s.strikes));
    }
  });
  reg.add_lua_api("weather", [this](lua_State* L) { weather::push_weather_lua(L, &sim_); });
  reg.add_net_channel(kChannel, [this](runtime::World& w, int /*from*/, const std::vector<std::uint8_t>& b) {
    // Server -> clients: [kind, fade(float)].
    if (w.net_role() != runtime::NetRole::Client || b.size() != 5) return;
    float fade = 0;
    std::memcpy(&fade, b.data() + 1, 4);
    if (b[0] < weather::kind_names().size()) sim_.set(static_cast<Kind>(b[0]), fade);
  });
  reg.add_draw_layer("weather.air", true,
                     [this](const runtime::World&, const runtime::WorldRect& view, float, std::vector<runtime::DrawQuad>* out) {
                       draw_world(sim_.state(), view, out);
                     });
  reg.add_hud_layer("weather.tint",
                    [this](const runtime::World&, const runtime::WorldRect& view, float, std::vector<runtime::DrawQuad>* out) {
                      draw_hud(sim_.state(), view, out);
                    });
  reg.add_light_source("weather.lightning",
                       [this](const runtime::World&, const runtime::WorldRect& view, std::vector<fx::LightInstance>* out) {
                         const State& s = sim_.state();
                         if (s.flash < 0.05f) return;
                         fx::LightInstance L;
                         const float w = view.x1 - view.x0, h = view.y1 - view.y0;
                         L.x = view.x0 + s.strike_at * w;
                         L.y = view.y0 - h * 0.15f;
                         L.data.color[0] = 0.85f;
                         L.data.color[1] = 0.9f;
                         L.data.color[2] = 1.0f;
                         L.data.intensity = std::min(4.0f, s.flash * 3.5f);
                         L.data.radius = std::max(w, h) * 1.4f;
                         L.data.shadows = true;
                         L.data.softness = 0.2f;
                         out->push_back(L);
                       });
  reg.add_ambient("weather.sky", [this](const runtime::World& w, float out[3]) {
    const State& s = sim_.state();
    const bool scene_on = w.lighting().enabled;
    if (!cfg_.time_link && s.mix.cloud < 0.02f && s.flash <= 0.0f) return false;
    float scene[3] = {1, 1, 1};
    if (scene_on) w.lighting().ambient_at(w.time_seconds(), scene);
    ambient(s, cfg_, scene, scene_on && w.lighting().day_night, out);
    return true;
  });
  if (hook_ref() && host_.rfind("editor", 0) == 0) hook_ref()(*this, reg);
}

void WeatherPlugin::on_play_begin(runtime::World& world) {
  fx::ensure_builtin_images();
  sim_.configure(cfg_);
  playing_ = true;
  world_ = &world;
  strikes_seen_ = 0;
  announced_ = sim_.state().target;
  sim_.on_script_set = [this](Kind k, float fade) {
    if (!world_ || world_->net_role() != runtime::NetRole::Server) return;
    std::vector<std::uint8_t> b(5);
    b[0] = static_cast<std::uint8_t>(k);
    std::memcpy(b.data() + 1, &fade, 4);
    world_->net_post(kChannel, -1, std::move(b));
  };
}

void WeatherPlugin::on_play_end() {
  playing_ = false;
  world_ = nullptr;
  sim_.on_script_set = nullptr;
  sim_.configure(cfg_);
}

void WeatherPlugin::ambient(const State& s, const weather::Config& c, const float scene[3], bool scene_cycles,
                            float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = scene[i];
  if (c.time_link && !scene_cycles) {
    const float night[3] = {0.12f, 0.13f, 0.25f};
    for (int i = 0; i < 3; ++i) out[i] = night[i] + (out[i] - night[i]) * s.daylight;
  }
  const float dim = 1.0f - 0.35f * s.mix.cloud - 0.1f * std::min(1.0f, s.mix.dust);
  for (int i = 0; i < 3; ++i) out[i] *= dim;
  if (s.mix.dust > 0) {
    out[0] *= 1.0f + 0.08f * s.mix.dust;
    out[2] *= 1.0f - 0.15f * s.mix.dust;
  }
  for (int i = 0; i < 3; ++i) out[i] = std::clamp(out[i] + s.flash * (i == 2 ? 1.4f : 1.2f), 0.0f, 3.0f);
}

void WeatherPlugin::draw_world(const State& s, const runtime::WorldRect& view, std::vector<runtime::DrawQuad>* out) {
  const weather::Mix& m = s.mix;
  if (m.rain + m.snow + m.dust + m.fog < 0.01f) return;
  const float vw = view.x1 - view.x0, vh = view.y1 - view.y0;
  if (vw <= 0 || vh <= 0) return;
  const float area = std::clamp(vw * vh / (1280.0f * 720.0f), 0.1f, 6.0f);
  const float t = static_cast<float>(s.time);
  const float wx = s.wind_dir[0] * s.wind;
  const float pad = 64.0f;
  const float sw = vw + pad * 2, sh = vh + pad * 2;
  const float lit = std::min(1.0f, 0.3f + 0.7f * s.daylight + s.flash);
  auto at = [&](std::uint32_t i, std::uint32_t salt, float vx, float vy, float* x, float* y) {
    const float x0 = hu(i, salt) * sw, y0 = hu(i, salt + 1) * sh;
    *x = wrap(x0 + vx * t - (view.x0 - pad), sw) + view.x0 - pad;
    *y = wrap(y0 + vy * t - (view.y0 - pad), sh) + view.y0 - pad;
  };
  // Fog banks: huge soft dots drifting with the wind (under the rest).
  const int fog = static_cast<int>(std::min(1.5f, m.fog) * 14.0f * area);
  for (int i = 0; i < fog; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    float x, y;
    at(u, 71, wx * 18.0f + 6.0f, 0.0f, &x, &y);
    const float sz = (260.0f + 260.0f * hu(u, 73)) * std::sqrt(area);
    const float c[4] = {0.82f * lit, 0.84f * lit, 0.88f * lit, 0.10f * std::min(1.0f, m.fog)};
    runtime::DrawQuad q = quad(x, y, sz * 1.6f, sz, c);
    q.image = &dot_image();
    out->push_back(q);
  }
  // Dust: tan puffs racing along the wind.
  const int dust = static_cast<int>(std::min(1.5f, m.dust) * 120.0f * area);
  for (int i = 0; i < dust; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    float x, y;
    const float sp = 500.0f + 400.0f * hu(u, 41);
    at(u, 43, wx * sp + 80.0f, 20.0f * std::sin(static_cast<float>(i)), &x, &y);
    const float sz = 30.0f + 110.0f * hu(u, 45);
    const float c[4] = {0.78f * lit, 0.62f * lit, 0.40f * lit, 0.13f};
    runtime::DrawQuad q = quad(x, y, sz * 1.5f, sz, c);
    q.image = &dot_image();
    q.rotation = hu(u, 47) * 360.0f;
    out->push_back(q);
  }
  // Rain: thin streaks leaning with the wind.
  const int rain = static_cast<int>(std::min(1.5f, m.rain) * 420.0f * area);
  const float fall = 950.0f;
  const float vx = wx * 380.0f;
  const float lean = -std::atan2(vx, fall) * 57.29578f;
  for (int i = 0; i < rain; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    float x, y;
    const float sp = 0.8f + 0.4f * hu(u, 3);
    at(u, 5, vx * sp, fall * sp, &x, &y);
    const float c[4] = {0.70f * lit, 0.76f * lit, 0.90f * lit, 0.45f};
    runtime::DrawQuad q = quad(x, y, 1.6f, 16.0f + 10.0f * hu(u, 9), c);
    q.rotation = lean;
    out->push_back(q);
  }
  // Snow: flakes that wobble down.
  const int snow = static_cast<int>(std::min(1.5f, m.snow) * 300.0f * area);
  for (int i = 0; i < snow; ++i) {
    const auto u = static_cast<std::uint32_t>(i);
    float x, y;
    at(u, 13, wx * 160.0f, 55.0f + 45.0f * hu(u, 15), &x, &y);
    x += 14.0f * std::sin(t * 1.4f + hu(u, 17) * 6.28f);
    const float sz = 3.0f + 4.0f * hu(u, 19);
    const float c[4] = {0.96f * lit, 0.97f * lit, 1.0f * lit, 0.9f};
    runtime::DrawQuad q = quad(x, y, sz, sz, c);
    q.image = &dot_image();
    out->push_back(q);
  }
}

void WeatherPlugin::draw_hud(const State& s, const runtime::WorldRect& view, std::vector<runtime::DrawQuad>* out) {
  const weather::Mix& m = s.mix;
  // Haze over everything (fog grey / dust tan), then the lightning flash.
  const float haze = std::min(0.55f, m.fog * 0.32f + m.dust * 0.3f + m.rain * 0.05f + m.snow * 0.08f);
  if (haze > 0.005f) {
    const float dw = m.dust / std::max(0.001f, m.dust + m.fog + m.rain + m.snow);
    const float day = 0.25f + 0.75f * s.daylight;
    runtime::DrawQuad q;
    q.x0 = view.x0;
    q.y0 = view.y0;
    q.x1 = view.x1;
    q.y1 = view.y1;
    q.rgba[0] = (0.70f + (0.80f - 0.70f) * dw) * day;
    q.rgba[1] = (0.72f + (0.60f - 0.72f) * dw) * day;
    q.rgba[2] = (0.76f + (0.38f - 0.76f) * dw) * day;
    q.rgba[3] = haze;
    out->push_back(q);
  }
  if (s.flash > 0.01f) {
    runtime::DrawQuad q;
    q.x0 = view.x0;
    q.y0 = view.y0;
    q.x1 = view.x1;
    q.y1 = view.y1;
    q.rgba[0] = 0.9f;
    q.rgba[1] = 0.93f;
    q.rgba[2] = 1.0f;
    q.rgba[3] = std::min(0.65f, s.flash * 0.6f);
    q.blend = 1;
    out->push_back(q);
  }
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(weather, ::ts::tombstone::plugins::WeatherPlugin)
