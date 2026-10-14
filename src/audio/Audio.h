#pragma once

// ts_audio: the engine's sound layer.
//
//   * Clips: wav / ogg / mp3 / flac decoded by miniaudio (+ stb_vorbis) into
//     float PCM at the mixer rate, cached by reference. "builtin:<name>"
//     references are small placeholder sounds synthesized in code (CC0 by
//     construction): click, step, gunshot, mine, boom, grab, throw, chime,
//     tune.
//   * Buses: master, music, sfx, ui, voice; each with a volume, a mute and a
//     peak meter. Every voice and the music decks route through one bus and
//     then master.
//   * Voices: one-shots and loops with volume / pitch (linear-resampled) and
//     optional random variation, equal-power stereo pan, and positional mode
//     (listener + attenuation + optional doppler).
//   * Music: two decks streamed from disk with crossfade, and playlists.
//   * Backends: a miniaudio playback device when one opens, else the null
//     backend (headless smokes, servers, CI): the mixer runs the same, the
//     host just pumps it (update()) and the output goes nowhere.
//
// The mixer itself (mix()) is plain code, callable directly, so tests check
// levels sample-exact without a sound card.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ts {
namespace audio {

enum class Bus : std::uint8_t { Master = 0, Music, Sfx, Ui, Voice, Count };
constexpr int kBusCount = static_cast<int>(Bus::Count);
const char* bus_name(Bus bus);
const char* bus_label(Bus bus);  // "Master" ...
// "master" / "music" / "sfx" / "ui" / "voice" (case-insensitive). False and
// *out untouched when unknown.
bool parse_bus(const std::string& name, Bus* out);

// Decoded PCM: interleaved floats, 1 or 2 channels, at `rate`.
struct Clip {
  int channels = 1;
  int rate = 48000;
  std::vector<float> samples;
  std::size_t frames() const {
    return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
  }
  double seconds() const {
    return rate > 0 ? static_cast<double>(frames()) / rate : 0.0;
  }
};
using ClipPtr = std::shared_ptr<const Clip>;

// Formats by extension: .wav .ogg .mp3 .flac.
bool is_audio_file(const std::string& path);
// Decode a whole file / buffer, resampled to `rate`, keeping its channel
// count (mono stays mono). The format is sniffed from the bytes.
bool decode_memory(const void* data, std::size_t size, int rate, Clip* out,
                   std::string* error = nullptr);
bool decode_file(const std::string& path, int rate, Clip* out,
                 std::string* error = nullptr);
// 16-bit PCM wav writer (tests, the "bake" button).
bool write_wav(const std::string& path, const Clip& clip);

// Built-in placeholder sounds ("builtin:click" ...): synthesized at `rate`,
// deterministic. Null when the name is unknown.
ClipPtr synth_builtin(const std::string& name, int rate);
const std::vector<std::string>& builtin_names();
bool is_builtin(const std::string& ref);

// --- Spatial math (pure; see smoke "pan math") ---------------------------
struct Vec3f {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct PanGains {
  float left = 0.0f, right = 0.0f;
};
// Equal-power pan law: pan -1 (left) .. 0 (centre, both 0.7071) .. +1.
PanGains pan_gains(float pan);
// Distance gain: 1 inside min_distance, falling linearly to 0 at
// max_distance (inverse-rolloff feel without a long tail).
float attenuation(float distance, float min_distance, float max_distance);
// Doppler pitch factor for a source / listener closing at relative speeds
// along the line between them (positive = approaching). Clamped 0.5..2.
float doppler_factor(float listener_toward_source, float source_toward_listener,
                     float speed_of_sound);

struct Listener {
  Vec3f position;
  Vec3f forward{0.0f, 0.0f, -1.0f};
  Vec3f up{0.0f, 1.0f, 0.0f};
  Vec3f velocity;
};
// Listener for a 2D world in pixels: looking "into" the screen at (x, y),
// +x is right; y grows downwards like the world.
Listener listener_2d(float x, float y);
struct Spatial {
  float gain = 1.0f;
  float pan = 0.0f;
  float pitch = 1.0f;
};
Spatial spatialize(const Listener& listener, const Vec3f& source,
                   const Vec3f& source_velocity, float min_distance,
                   float max_distance, bool doppler,
                   float speed_of_sound = 343.0f);

// --- Engine ------------------------------------------------------------
using VoiceId = std::uint32_t;

struct PlayParams {
  Bus bus = Bus::Sfx;
  float volume = 1.0f;
  float pitch = 1.0f;
  float pan = 0.0f;              // non-spatial voices
  float volume_variation = 0.0f; // +- fraction, random per play (0.1 = 10 %)
  float pitch_variation = 0.0f;
  bool loop = false;
  bool spatial = false;
  Vec3f position;
  Vec3f velocity;
  float min_distance = 1.0f;
  float max_distance = 30.0f;
  bool doppler = false;
};

struct EngineOptions {
  bool device = true;  // false = null backend from the start
  int rate = 48000;
  std::string project_dir;  // relative clip paths resolve against it
};

struct VoiceInfo {
  VoiceId id = 0;
  std::string ref;
  Bus bus = Bus::Sfx;
  float gain = 0.0f;
  bool loop = false;
};

class Engine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Opens the playback device (unless options.device is false or the env
  // var TS_AUDIO=null), else falls back to the null backend. Always true.
  bool start(const EngineOptions& options = EngineOptions());
  void shutdown();
  bool started() const { return started_; }
  bool has_device() const;
  std::string backend_name() const;
  int rate() const { return rate_; }

  void set_project_dir(const std::string& dir);
  const std::string& project_dir() const { return project_dir_; }
  // Decoded + cached; null (and *error) when missing / undecodable.
  ClipPtr load(const std::string& ref, std::string* error = nullptr);
  void clear_cache();
  std::string resolve(const std::string& ref) const;

  // Voices. 0 when the clip did not load or the voice cap was hit.
  static constexpr std::size_t kMaxVoices = 64;
  VoiceId play(const std::string& ref, const PlayParams& params = PlayParams());
  VoiceId play(ClipPtr clip, const PlayParams& params,
               const std::string& ref = std::string());
  void stop(VoiceId id, float fade_seconds = 0.0f);
  void stop_bus(Bus bus, float fade_seconds = 0.0f);
  void stop_all();
  bool playing(VoiceId id) const;
  void set_voice_position(VoiceId id, const Vec3f& pos, const Vec3f& vel = Vec3f());
  void set_voice_volume(VoiceId id, float volume);
  std::size_t voice_count() const;
  std::vector<VoiceInfo> voices() const;
  void set_listener(const Listener& listener);
  Listener listener() const;
  void set_seed(std::uint32_t seed);

  // Buses.
  void set_bus_volume(Bus bus, float volume);  // 0..2
  float bus_volume(Bus bus) const;
  void set_bus_muted(Bus bus, bool muted);
  bool bus_muted(Bus bus) const;
  // Peak level (after volume) seen since the last call; decays for meters.
  float bus_peak(Bus bus);

  // Music: streamed from disk (files) or played from a clip (builtins), on
  // two decks crossfading over fade_seconds.
  bool play_music(const std::string& ref, float fade_seconds = 1.0f,
                  bool loop = true);
  void stop_music(float fade_seconds = 1.0f);
  std::string current_music() const;
  // Playlist: tracks advance when one ends (loop off per track), wrapping.
  void set_playlist(const std::vector<std::string>& refs, float fade_seconds = 1.0f,
                    bool shuffle = false);
  const std::vector<std::string>& playlist() const { return playlist_; }
  void next_track(float fade_seconds = 1.0f);
  int playlist_index() const { return playlist_index_; }
  // 0..1 gain of the incoming / outgoing deck (tests, the Mixer).
  float music_deck_gain(int deck) const;

  // Mix `frames` stereo frames (interleaved) into `out` (overwrites). The
  // device callback calls this; tests and the null backend call it directly.
  void mix(float* out, int frames);
  // Main-thread tick: under the null backend mixes dt seconds into scratch
  // so voices advance and end; advances playlists either way.
  void update(double dt_seconds);
  std::uint64_t frames_mixed() const { return frames_mixed_; }

 private:
  struct Voice;
  struct Deck;
  struct Device;
  void mix_locked(float* out, int frames);
  void start_deck_locked(const std::string& ref, float fade, bool loop);
  float rand01_locked();

  mutable std::mutex mutex_;
  bool started_ = false;
  int rate_ = 48000;
  std::string project_dir_;
  std::unique_ptr<Device> device_;
  std::vector<std::pair<std::string, ClipPtr>> cache_;
  std::vector<std::unique_ptr<Voice>> voices_;
  VoiceId next_voice_ = 1;
  Listener listener_;
  std::array<float, kBusCount> volume_{};
  std::array<bool, kBusCount> muted_{};
  std::array<float, kBusCount> peak_{};
  std::array<std::unique_ptr<Deck>, 2> decks_;
  int active_deck_ = 0;
  std::vector<std::string> playlist_;
  int playlist_index_ = -1;
  float playlist_fade_ = 1.0f;
  bool playlist_advance_ = false;
  std::uint32_t rng_ = 0x9E3779B9u;
  std::uint64_t frames_mixed_ = 0;
  std::vector<float> scratch_;
  std::vector<float> bus_buf_;
};

}  // namespace audio
}  // namespace ts
