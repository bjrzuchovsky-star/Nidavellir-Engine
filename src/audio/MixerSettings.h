#pragma once

// Saved mixer: per-bus volume + mute in a small JSON file (the editor keeps
// it beside settings.json, ./TombStoneConfig/audio.json; ts_game and
// ts_client read the same file from their working folder).
//
//   {"version": 1, "buses": {"master": {"volume": 1, "muted": false}, ...}}

#include "audio/Audio.h"

#include <array>
#include <string>

namespace ts {
namespace audio {

struct MixerSettings {
  std::array<float, kBusCount> volume{1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<bool, kBusCount> muted{};
  bool operator==(const MixerSettings& o) const = default;
};

std::string default_mixer_settings_path();  // ./TombStoneConfig/audio.json
std::string mixer_settings_to_json(const MixerSettings& s);
// Missing keys keep their defaults; volumes clamp to 0..2.
bool mixer_settings_from_json(const std::string& text, MixerSettings* out,
                              std::string* error = nullptr);
// A missing file is not an error (defaults).
bool load_mixer_settings(const std::string& path, MixerSettings* out,
                         std::string* error = nullptr);
bool save_mixer_settings(const std::string& path, const MixerSettings& s,
                         std::string* error = nullptr);
void apply_mixer_settings(Engine& engine, const MixerSettings& s);
MixerSettings capture_mixer_settings(const Engine& engine);

}  // namespace audio
}  // namespace ts
