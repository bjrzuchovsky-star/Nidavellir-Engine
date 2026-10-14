#pragma once

// The editor's one audio engine. The Admin window opens the sound card
// (start(true)); smokes and headless paths get the null backend, which
// mixes in update() so voices still advance and end. Bus volumes persist to
// TombStoneConfig/audio.json (saved shortly after a fader stops moving).

#include "audio/Audio.h"
#include "audio/MixerSettings.h"

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
struct AudioSourceData;
}  // namespace tombstone
}  // namespace ts

namespace ts {
namespace tombstone {
namespace editor {

class EditorAudio {
 public:
  static EditorAudio& get();

  // Idempotent; a second call with device=true upgrades a null engine.
  void start(bool device);
  void shutdown();
  audio::Engine& engine();

  // Main loop: engine update + deferred settings save.
  void tick(double dt);

  void set_bus_volume(audio::Bus bus, float volume);
  void set_bus_muted(audio::Bus bus, bool muted);
  void reset_mixer();
  void save_now();
  const std::string& settings_path() const { return settings_path_; }
  void set_settings_path(const std::string& path);

  // Supply Wagon / Inspector audition: one preview at a time (ui bus).
  void preview(const std::string& project_dir, const std::string& ref);
  void stop_preview();
  bool previewing(const std::string& ref) const;
  // Little UI click (toolbar buttons).
  void click();

 private:
  EditorAudio();
  void ensure_started();

  audio::Engine engine_;
  bool device_ = false;
  std::string settings_path_;
  double save_in_ = -1.0;
  audio::VoiceId preview_voice_ = 0;
  std::string preview_ref_;
};

// "Saloon Band" mixer window body (callers own Begin/End).
void draw_mixer_panel();

// Clip picker: "(none)", the built-in placeholders, then the project's
// sounds. True when the user picked something else (*out).
bool audio_clip_combo(const char* label, const std::string& current,
                      const std::vector<std::string>& sounds, std::string* out);

// Inspector body for an Audio Source (2D and 3D share it). Edits `a` in
// place; Live = a drag in flight (apply + coalesce), Commit = one undo step
// labelled *step. after_drag runs right after each drag widget (undo
// coalescing hooks there). three_d switches the distance units to metres.
enum class AudioEdit { None, Live, Commit, Remove };
AudioEdit draw_audio_source_fields(AudioSourceData& a,
                                   const std::vector<std::string>& sounds,
                                   const std::string& project_dir, bool three_d,
                                   const std::function<void(const char*)>& after_drag,
                                   const char** step);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
