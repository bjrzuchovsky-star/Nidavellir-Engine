#pragma once

// Sound for the shipped hosts (ts_game, ts_client): one engine on the sound
// card (null backend when there is none, or TS_AUDIO=null), the player's
// saved mixer levels (TombStoneConfig/audio.json beside the editor's), and a
// SoundStage riding the project.

#include "audio/Audio.h"
#include "audio/MixerSettings.h"
#include "audio/SoundStage.h"

#include <iostream>
#include <string>

namespace ts {
namespace audio {

class GameAudio {
 public:
  GameAudio(const std::string& project_dir, bool three_d, const char* tag)
      : stage_(engine_) {
    EngineOptions opt;
    opt.project_dir = project_dir;
    engine_.start(opt);
    MixerSettings s;
    std::string err;
    if (!load_mixer_settings(default_mixer_settings_path(), &s, &err)) {
      std::cerr << "[" << tag << "] audio settings ignored: " << err << '\n';
    }
    apply_mixer_settings(engine_, s);
    stage_.begin_ride(project_dir, three_d);
    std::cout << "[" << tag << "] audio: " << engine_.backend_name() << '\n';
  }
  ~GameAudio() {
    stage_.end_ride(0.0f);
    engine_.shutdown();
  }
  GameAudio(const GameAudio&) = delete;
  GameAudio& operator=(const GameAudio&) = delete;

  Engine& engine() { return engine_; }
  SoundStage& stage() { return stage_; }
  // A fresh ride (scene reloaded): drop old voices, keep the levels.
  void restart(const std::string& project_dir, bool three_d) {
    stage_.end_ride(0.0f);
    stage_.begin_ride(project_dir, three_d);
  }
  void tick(double dt) { engine_.update(dt); }

 private:
  Engine engine_;
  SoundStage stage_;
};

}  // namespace audio
}  // namespace ts
