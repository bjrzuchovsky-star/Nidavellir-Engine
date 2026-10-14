#include "audio/MixerSettings.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace audio {

namespace json = tombstone::json;
namespace fs = std::filesystem;

std::string default_mixer_settings_path() {
  return (fs::path("TombStoneConfig") / "audio.json").string();
}

std::string mixer_settings_to_json(const MixerSettings& s) {
  json::Value root = json::Value::object();
  root.set("version", json::Value::number(1));
  json::Value buses = json::Value::object();
  for (int b = 0; b < kBusCount; ++b) {
    json::Value v = json::Value::object();
    v.set("volume", json::Value::number(s.volume[static_cast<std::size_t>(b)]));
    v.set("muted", json::Value::boolean(s.muted[static_cast<std::size_t>(b)]));
    buses.set(bus_name(static_cast<Bus>(b)), std::move(v));
  }
  root.set("buses", std::move(buses));
  return json::write(root);
}

bool mixer_settings_from_json(const std::string& text, MixerSettings* out,
                              std::string* error) {
  json::Value root;
  std::string err;
  if (!json::parse(text, &root, &err) || !root.is_object()) {
    if (error) *error = err.empty() ? "not a JSON object" : err;
    return false;
  }
  MixerSettings s;
  const json::Value& buses = root["buses"];
  for (int b = 0; b < kBusCount; ++b) {
    const json::Value& v = buses[bus_name(static_cast<Bus>(b))];
    if (!v.is_object()) continue;
    const float vol = v.flt("volume", 1.0f);
    s.volume[static_cast<std::size_t>(b)] = vol == vol ? std::clamp(vol, 0.0f, 2.0f) : 1.0f;
    s.muted[static_cast<std::size_t>(b)] = v.flag("muted", false);
  }
  *out = s;
  return true;
}

bool load_mixer_settings(const std::string& path, MixerSettings* out, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *out = MixerSettings{};
    return true;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return mixer_settings_from_json(ss.str(), out, error);
}

bool save_mixer_settings(const std::string& path, const MixerSettings& s,
                         std::string* error) {
  std::error_code ec;
  const fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  f << mixer_settings_to_json(s) << "\n";
  return static_cast<bool>(f);
}

void apply_mixer_settings(Engine& engine, const MixerSettings& s) {
  for (int b = 0; b < kBusCount; ++b) {
    engine.set_bus_volume(static_cast<Bus>(b), s.volume[static_cast<std::size_t>(b)]);
    engine.set_bus_muted(static_cast<Bus>(b), s.muted[static_cast<std::size_t>(b)]);
  }
}

MixerSettings capture_mixer_settings(const Engine& engine) {
  MixerSettings s;
  for (int b = 0; b < kBusCount; ++b) {
    s.volume[static_cast<std::size_t>(b)] = engine.bus_volume(static_cast<Bus>(b));
    s.muted[static_cast<std::size_t>(b)] = engine.bus_muted(static_cast<Bus>(b));
  }
  return s;
}

}  // namespace audio
}  // namespace ts
