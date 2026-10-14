#pragma once

// SoundStage: plays a ride's sound on a ts_audio Engine. Hosts (editor Play
// / posse, ts_game, ts_client) own one per ride and each frame
//   * hand it the world's queued SoundEvents (World::take_sounds()),
//   * sync the audio-source components (autoplay / loop, positions),
//   * set the listener (2D camera, 3D camera, or the VR head).
// When the ride ends it stops what it started, fades the music and puts the
// bus volumes scripts changed back. Works the same on the null backend.

#include "audio/Audio.h"
#include "runtime/Sound.h"
#include "scene/SceneData.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {
class World;
struct CameraView;
}  // namespace runtime
}  // namespace tombstone

namespace audio {

// An entity's audio source this frame (alive = in the world and shown).
struct SourceState {
  std::uint64_t id = 0;
  const tombstone::AudioSourceData* data = nullptr;
  Vec3f position;
  Vec3f velocity;
};

class SoundStage {
 public:
  explicit SoundStage(Engine& engine);
  ~SoundStage();
  SoundStage(const SoundStage&) = delete;
  SoundStage& operator=(const SoundStage&) = delete;

  // `three_d`: world units are metres (default distances 2..40) instead of
  // pixels (200..1400).
  void begin_ride(const std::string& project_dir, bool three_d);
  void end_ride(float music_fade = 0.5f);
  bool riding() const { return riding_; }

  void handle(const tombstone::runtime::SoundEvent& event);
  void handle_all(const std::vector<tombstone::runtime::SoundEvent>& events);
  // Starts autoplay sources once, keeps loops going and positioned, stops
  // the ones whose entity is gone (missing from `sources`).
  void sync_sources(const std::vector<SourceState>& sources);
  void set_listener(const Listener& listener) { engine_.set_listener(listener); }

  // 2D convenience: drain + sources + listener at the camera centre.
  void update_2d(tombstone::runtime::World& world, float cam_x, float cam_y);

  std::uint64_t events_handled() const { return events_; }
  std::size_t sources_playing() const;
  // Voices started for an entity's events (cleared as they end).
  std::size_t owned_voices(std::uint64_t entity) const;

 private:
  PlayParams params_for(const tombstone::AudioSourceData& a) const;
  void prune();

  Engine& engine_;
  bool riding_ = false;
  bool three_d_ = false;
  std::map<std::uint64_t, VoiceId> sources_;
  std::set<std::uint64_t> finished_;  // one-shot autoplays already played
  std::multimap<std::uint64_t, VoiceId> owned_;
  std::array<float, kBusCount> saved_volume_{};
  std::uint64_t events_ = 0;
};

}  // namespace audio
}  // namespace ts
