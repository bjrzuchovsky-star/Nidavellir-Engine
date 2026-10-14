#pragma once

// Sound requests from the simulation (scripts, plugins, gameplay) to
// whoever plays audio. The world only queues them (World::play_sound); the
// host (editor Play / posse, ts_game, ts_client) drains the queue each frame
// into ts_audio. A server world also posts each one on the "ts.audio"
// channel so every client hears script- and plugin-triggered sounds; a
// client world queues what arrives there. No audio code here: servers and
// headless tools link the runtime without a sound card.

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

struct SoundEvent {
  enum class Kind : std::uint8_t {
    Play = 0,    // one-shot (or loop) on `bus`
    Music,       // crossfade to `clip` over `fade`
    StopMusic,   // fade the music out
    Playlist,    // tracks in `list`, crossfading `fade`
    Volume,      // set `bus` volume to `volume`
    Stop,        // stop sounds `entity` started (0 = all one-shots on bus)
  };
  Kind kind = Kind::Play;
  std::string clip;          // "builtin:click" or a project-relative path
  std::string bus = "sfx";   // master / music / sfx / ui / voice
  float volume = 1.0f;
  float pitch = 1.0f;
  float variation = 0.0f;    // +- fraction on volume and pitch per play
  bool positional = false;   // pan / fade from (x, y, z) to the listener
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float fade = 1.0f;         // music / stop fade seconds
  bool loop = false;
  bool three_d = false;      // world units are metres (3D) not pixels
  std::uint64_t entity = 0;  // who made it (follow-up stops)
  std::vector<std::string> list;
  int slot = -1;             // server: -1 = every rider, else that one

  bool operator==(const SoundEvent& o) const = default;
};

// Wire format for the "ts.audio" channel (little-endian, versioned).
inline constexpr const char* kSoundChannel = "ts.audio";
std::vector<std::uint8_t> encode_sound(const SoundEvent& ev);
bool decode_sound(const std::vector<std::uint8_t>& bytes, SoundEvent* out);

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
