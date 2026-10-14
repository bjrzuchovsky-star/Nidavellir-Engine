#include "editor/audio/EditorAudio.h"

#include "editor/settings/Settings.h"

#include <filesystem>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

EditorAudio& EditorAudio::get() {
  static EditorAudio instance;
  return instance;
}

EditorAudio::EditorAudio()
    : settings_path_((std::filesystem::path(SettingsStore::default_config_dir()) /
                      "audio.json")
                         .string()) {}

void EditorAudio::ensure_started() {
  if (engine_.started()) return;
  audio::EngineOptions opt;
  opt.device = device_;
  engine_.start(opt);
  audio::MixerSettings s;
  std::string err;
  if (!audio::load_mixer_settings(settings_path_, &s, &err)) {
    std::cerr << "audio: ignoring " + settings_path_ + ": " + err << "\n";
  }
  audio::apply_mixer_settings(engine_, s);
}

void EditorAudio::start(bool device) {
  if (engine_.started() && (!device || engine_.has_device() || device_)) return;
  if (engine_.started()) engine_.shutdown();
  device_ = device;
  ensure_started();
  std::cerr << "audio: " + engine_.backend_name() << "\n";
}

void EditorAudio::shutdown() {
  if (save_in_ >= 0.0) save_now();
  preview_voice_ = 0;
  engine_.shutdown();
}

audio::Engine& EditorAudio::engine() {
  ensure_started();
  return engine_;
}

void EditorAudio::tick(double dt) {
  if (!engine_.started()) return;
  engine_.update(dt);
  if (save_in_ >= 0.0) {
    save_in_ -= dt;
    if (save_in_ < 0.0) save_now();
  }
  if (preview_voice_ && !engine_.playing(preview_voice_)) {
    preview_voice_ = 0;
    preview_ref_.clear();
  }
}

void EditorAudio::set_bus_volume(audio::Bus bus, float volume) {
  engine().set_bus_volume(bus, volume);
  save_in_ = 0.5;
}

void EditorAudio::set_bus_muted(audio::Bus bus, bool muted) {
  engine().set_bus_muted(bus, muted);
  save_in_ = 0.5;
}

void EditorAudio::reset_mixer() {
  audio::apply_mixer_settings(engine(), audio::MixerSettings{});
  save_in_ = 0.0;
}

void EditorAudio::save_now() {
  save_in_ = -1.0;
  std::string err;
  if (!audio::save_mixer_settings(settings_path_,
                                  audio::capture_mixer_settings(engine()), &err)) {
    std::cerr << "audio: " + err << "\n";
  }
}

void EditorAudio::set_settings_path(const std::string& path) {
  settings_path_ = path;
  if (!engine_.started()) return;
  audio::MixerSettings s;
  audio::load_mixer_settings(settings_path_, &s);
  audio::apply_mixer_settings(engine_, s);
}

void EditorAudio::preview(const std::string& project_dir, const std::string& ref) {
  stop_preview();
  audio::Engine& e = engine();
  if (!project_dir.empty() && e.project_dir() != project_dir) e.set_project_dir(project_dir);
  audio::PlayParams p;
  p.bus = audio::Bus::Ui;
  std::string err;
  audio::ClipPtr clip = e.load(ref, &err);
  if (!clip) {
    std::cerr << "audio: preview " + ref + ": " + err << "\n";
    return;
  }
  preview_voice_ = e.play(clip, p, ref);
  preview_ref_ = preview_voice_ ? ref : std::string();
}

void EditorAudio::stop_preview() {
  if (preview_voice_) engine_.stop(preview_voice_, 0.05f);
  preview_voice_ = 0;
  preview_ref_.clear();
}

bool EditorAudio::previewing(const std::string& ref) const {
  return preview_voice_ != 0 && preview_ref_ == ref;
}

void EditorAudio::click() {
  audio::PlayParams p;
  p.bus = audio::Bus::Ui;
  p.volume = 0.6f;
  p.pitch_variation = 0.05f;
  engine().play("builtin:click", p);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
