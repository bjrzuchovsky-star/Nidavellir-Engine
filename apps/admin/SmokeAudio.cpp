#include "SmokeAudio.h"

#include "SmokeAudioClips.h"

#include "audio/Audio.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace ts::audio;
namespace fs = std::filesystem;

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "audio smoke FAILED: " << what << "\n";
    ++g_fail;
  }
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// Constant-level mono clip (mixing levels are then exact).
ClipPtr dc_clip(float level, int frames, int rate) {
  auto c = std::make_shared<Clip>();
  c->channels = 1;
  c->rate = rate;
  c->samples.assign(static_cast<std::size_t>(frames), level);
  return c;
}

// Mix one block, return the left / right sample at `at`.
std::pair<float, float> mix_at(Engine& e, int frames, int at) {
  std::vector<float> out(static_cast<std::size_t>(frames) * 2);
  e.mix(out.data(), frames);
  return {out[static_cast<std::size_t>(at) * 2], out[static_cast<std::size_t>(at) * 2 + 1]};
}

// Rough pitch check: zero crossings per second of the first channel.
float crossings_hz(const Clip& c) {
  int n = 0;
  const std::size_t frames = c.frames();
  for (std::size_t i = 1; i < frames; ++i) {
    const float a = c.samples[(i - 1) * static_cast<std::size_t>(c.channels)];
    const float b = c.samples[i * static_cast<std::size_t>(c.channels)];
    if ((a < 0.0f) != (b < 0.0f)) ++n;
  }
  return static_cast<float>(n) / 2.0f / static_cast<float>(c.seconds());
}

void decode_checks(const fs::path& dir) {
  const int rate = 48000;
  struct Case {
    const char* name;
    const unsigned char* data;
    std::size_t size;
  } cases[] = {
      {"ogg", smoke_audio_clips::kOgg, sizeof(smoke_audio_clips::kOgg)},
      {"mp3", smoke_audio_clips::kMp3, sizeof(smoke_audio_clips::kMp3)},
      {"flac", smoke_audio_clips::kFlac, sizeof(smoke_audio_clips::kFlac)},
  };
  for (const Case& c : cases) {
    Clip clip;
    std::string err;
    const bool ok = decode_memory(c.data, c.size, rate, &clip, &err);
    check(ok, std::string("decode ") + c.name + ": " + err);
    if (!ok) continue;
    check(clip.channels == 1 && clip.rate == rate, std::string(c.name) + " format");
    // 0.1 s (encoders may pad a few ms).
    check(clip.seconds() > 0.08 && clip.seconds() < 0.25,
          std::string(c.name) + " length " + std::to_string(clip.seconds()));
    const float hz = crossings_hz(clip);
    check(hz > 400.0f && hz < 480.0f, std::string(c.name) + " pitch " + std::to_string(hz));
  }
  // wav: write a builtin out and read it back from disk.
  ClipPtr chime = synth_builtin("builtin:chime", 22050);
  check(chime && chime->frames() > 0, "builtin chime");
  const fs::path wav = dir / "chime.wav";
  check(chime && write_wav(wav.string(), *chime), "write wav");
  Clip back;
  std::string err;
  check(decode_file(wav.string(), 44100, &back, &err), "decode wav: " + err);
  check(chime && near(static_cast<float>(back.seconds()),
                      static_cast<float>(chime->seconds()), 0.01f),
        "wav length after resample");
  check(is_audio_file("a/B.OGG") && is_audio_file("x.flac") && !is_audio_file("x.png"),
        "extension filter");
  Clip junk;
  const unsigned char garbage[16] = {1, 2, 3};
  check(!decode_memory(garbage, sizeof(garbage), rate, &junk), "garbage rejected");
  for (const std::string& n : builtin_names()) {
    ClipPtr b = synth_builtin(n, 48000);
    float pk = 0.0f;
    if (b) {
      for (float s : b->samples) pk = std::max(pk, std::fabs(s));
    }
    check(b && b->frames() > 0 && pk > 0.3f && pk <= 1.0f, "builtin " + n);
  }
}

void spatial_checks() {
  const PanGains c = pan_gains(0.0f);
  check(near(c.left, 0.70711f) && near(c.right, 0.70711f), "centre pan -3 dB");
  const PanGains l = pan_gains(-1.0f);
  check(near(l.left, 1.0f) && near(l.right, 0.0f), "hard left");
  const PanGains r = pan_gains(1.0f);
  check(near(r.left, 0.0f) && near(r.right, 1.0f), "hard right");
  const PanGains h = pan_gains(0.5f);
  check(near(h.left * h.left + h.right * h.right, 1.0f), "equal power");
  check(near(attenuation(0.5f, 1.0f, 11.0f), 1.0f), "inside min");
  check(near(attenuation(6.0f, 1.0f, 11.0f), 0.5f), "half way");
  check(near(attenuation(20.0f, 1.0f, 11.0f), 0.0f), "past max");
  check(near(doppler_factor(0.0f, 0.0f, 343.0f), 1.0f), "doppler still");
  check(doppler_factor(0.0f, 34.3f, 343.0f) > 1.1f, "doppler approaching up");
  check(doppler_factor(0.0f, -34.3f, 343.0f) < 0.95f, "doppler receding down");

  // 3D: listener at origin looking -z; a source on +x is right.
  Listener lis;
  Spatial s = spatialize(lis, {5.0f, 0.0f, 0.0f}, {}, 1.0f, 30.0f, false);
  check(near(s.pan, 1.0f) && s.gain > 0.8f, "3D right");
  s = spatialize(lis, {0.0f, 0.0f, -5.0f}, {}, 1.0f, 30.0f, false);
  check(near(s.pan, 0.0f), "3D ahead centred");
  // Turned around (looking +z), +x is now on the left.
  lis.forward = {0.0f, 0.0f, 1.0f};
  s = spatialize(lis, {5.0f, 0.0f, 0.0f}, {}, 1.0f, 30.0f, false);
  check(near(s.pan, -1.0f), "3D turned: left");
  // Moving towards the listener raises pitch.
  s = spatialize(Listener(), {0.0f, 0.0f, -10.0f}, {0.0f, 0.0f, 30.0f}, 1.0f, 30.0f, true);
  check(s.pitch > 1.05f, "3D doppler");

  // 2D: camera at (100, 100); a source 300 px right pans right, left pans
  // left, one on the camera is centred, far away is silent.
  const Listener l2 = listener_2d(100.0f, 100.0f);
  const Spatial a = spatialize(l2, {400.0f, 100.0f, 0.0f}, {}, 200.0f, 1200.0f, false);
  const Spatial b = spatialize(l2, {-200.0f, 100.0f, 0.0f}, {}, 200.0f, 1200.0f, false);
  const Spatial m = spatialize(l2, {100.0f, 100.0f, 0.0f}, {}, 200.0f, 1200.0f, false);
  const Spatial f = spatialize(l2, {3000.0f, 100.0f, 0.0f}, {}, 200.0f, 1200.0f, false);
  check(a.pan > 0.8f && b.pan < -0.8f && near(m.pan, 0.0f), "2D pan by x");
  check(near(m.gain, 1.0f) && a.gain < 1.0f && a.gain > 0.0f && near(f.gain, 0.0f),
        "2D attenuation");
  const Spatial up = spatialize(l2, {100.0f, -100.0f, 0.0f}, {}, 200.0f, 1200.0f, false);
  check(near(up.pan, 0.0f), "2D vertical offset is centred");
}

void mixing_checks() {
  Engine e;
  e.start({false, 48000, ""});
  check(!e.has_device() && e.backend_name() == "null", "null backend");
  const int R = e.rate();
  ClipPtr dc = dc_clip(0.5f, R, R);

  // One centred mono voice on sfx: 0.5 * 0.7071 per side.
  VoiceId v = e.play(dc, PlayParams{}, "dc");
  check(v != 0 && e.voice_count() == 1, "voice started");
  auto lr = mix_at(e, 256, 100);
  check(near(lr.first, 0.35355f) && near(lr.second, 0.35355f), "centre level");
  // Bus volume halves it, master halves again.
  e.set_bus_volume(Bus::Sfx, 0.5f);
  mix_at(e, 256, 0);  // gains ramp over one block
  lr = mix_at(e, 256, 100);
  check(near(lr.first, 0.17678f), "sfx bus 50 %");
  e.set_bus_volume(Bus::Master, 0.5f);
  lr = mix_at(e, 256, 100);
  check(near(lr.first, 0.08839f), "master 50 %");
  e.set_bus_muted(Bus::Sfx, true);
  lr = mix_at(e, 256, 100);
  check(near(lr.first, 0.0f), "sfx muted");
  e.set_bus_muted(Bus::Sfx, false);
  e.set_bus_volume(Bus::Sfx, 1.0f);
  e.set_bus_volume(Bus::Master, 1.0f);
  // A second voice on ui adds, hard right.
  PlayParams ui;
  ui.bus = Bus::Ui;
  ui.pan = 1.0f;
  ui.loop = true;
  VoiceId u = e.play(dc, ui, "dc");
  mix_at(e, 256, 0);
  lr = mix_at(e, 256, 100);
  check(near(lr.first, 0.35355f) && near(lr.second, 0.85355f), "two buses add");
  check(e.bus_peak(Bus::Ui) > 0.49f && e.bus_peak(Bus::Ui) < 0.01f, "ui meter + reset");
  e.set_bus_muted(Bus::Master, true);
  lr = mix_at(e, 256, 100);
  check(near(lr.first, 0.0f) && near(lr.second, 0.0f), "master mute");
  e.set_bus_muted(Bus::Master, false);
  e.stop(u);
  e.stop(v, 0.01f);  // 480 frames of fade
  mix_at(e, 1024, 0);
  check(e.voice_count() == 0, "stop + fade end voices");

  // One-shot ends on its own; pitch 2 ends in half the time.
  ClipPtr second = dc_clip(0.25f, R / 10, R);  // 0.1 s
  PlayParams p2;
  p2.pitch = 2.0f;
  VoiceId fast = e.play(second, p2, "fast");
  VoiceId slow = e.play(second, PlayParams{}, "slow");
  e.update(0.06);
  check(!e.playing(fast) && e.playing(slow), "pitch 2 halves length");
  e.update(0.06);
  check(!e.playing(slow), "one-shot ends");

  // Variation stays within its range and differs between plays.
  e.set_seed(7);
  float lo = 10.0f, hi = 0.0f;
  for (int i = 0; i < 20; ++i) {
    PlayParams pv;
    pv.volume = 1.0f;
    pv.volume_variation = 0.2f;
    pv.pitch_variation = 0.1f;
    const VoiceId id = e.play(dc, pv, "var");
    e.mix(std::vector<float>(512).data(), 256);
    for (const VoiceInfo& vi : e.voices()) {
      if (vi.id == id) {
        lo = std::min(lo, vi.gain);
        hi = std::max(hi, vi.gain);
      }
    }
    e.stop(id);
  }
  check(lo >= 0.8f * 0.7071f - 1e-3f && hi <= 1.2f * 0.7071f + 1e-3f && hi - lo > 0.05f,
        "volume variation range");

  // Spatial voice follows the listener.
  PlayParams sp;
  sp.spatial = true;
  sp.loop = true;
  sp.position = {400.0f, 100.0f, 0.0f};
  sp.min_distance = 200.0f;
  sp.max_distance = 1200.0f;
  e.set_listener(listener_2d(100.0f, 100.0f));
  const VoiceId s = e.play(dc, sp, "sp");
  mix_at(e, 256, 0);
  lr = mix_at(e, 256, 100);
  check(lr.second > lr.first * 3.0f, "spatial voice pans right");
  e.set_listener(listener_2d(700.0f, 100.0f));
  mix_at(e, 256, 0);
  lr = mix_at(e, 256, 100);
  check(lr.first > lr.second * 3.0f, "listener moved: now left");
  e.stop(s);

  // Voice cap: loops keep their place, one-shots get stolen.
  for (std::size_t i = 0; i < Engine::kMaxVoices + 8; ++i) e.play(dc, PlayParams{}, "cap");
  check(e.voice_count() == Engine::kMaxVoices, "voice cap");
  e.stop_all();
  e.shutdown();
}

void music_checks() {
  Engine e;
  e.start({false, 48000, ""});
  check(e.play_music("builtin:tune", 0.5f, true), "music builtin");
  check(e.current_music() == "builtin:tune", "current music");
  e.update(0.25);
  const float g1 = e.music_deck_gain(0);
  check(g1 > 0.4f && g1 < 0.6f, "fade in half way " + std::to_string(g1));
  e.update(0.3);
  check(near(e.music_deck_gain(0), 1.0f), "fade in done");
  check(e.bus_peak(Bus::Music) > 0.1f, "music bus has signal");
  // Crossfade: the old deck falls while the new rises.
  check(e.play_music("builtin:chime", 1.0f, true), "music 2");
  e.update(0.5);
  const float in = e.music_deck_gain(0), out = e.music_deck_gain(1);
  check(in > 0.4f && in < 0.6f && out > 0.4f && out < 0.6f,
        "crossfade mid " + std::to_string(in) + " " + std::to_string(out));
  e.update(0.6);
  check(near(e.music_deck_gain(0), 1.0f) && near(e.music_deck_gain(1), 0.0f),
        "crossfade done");
  // Music bus volume scales the music only.
  e.set_bus_volume(Bus::Music, 0.0f);
  e.bus_peak(Bus::Master);
  e.update(0.1);
  check(e.bus_peak(Bus::Master) < 1e-6f, "music bus silenced");
  e.set_bus_volume(Bus::Music, 1.0f);
  // Playlist: short tracks advance on their own and wrap.
  e.set_playlist({"builtin:click", "builtin:missing", "builtin:grab"}, 0.0f);
  check(e.playlist_index() == 0 && e.current_music() == "builtin:click", "playlist start");
  e.update(0.1);  // click is 30 ms
  e.update(0.02);
  check(e.current_music() == "builtin:grab" && e.playlist_index() == 2,
        "playlist skips a bad track: " + e.current_music());
  e.update(0.1);
  e.update(0.02);
  check(e.playlist_index() == 0, "playlist wraps");
  e.stop_music(0.0f);
  check(e.current_music().empty() && e.playlist().empty(), "stop music");
  e.shutdown();
}

}  // namespace

int run_audio_smoke() {
  g_fail = 0;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) /
                       ("tombstone_audio_" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007));
  fs::create_directories(dir, ec);
  decode_checks(dir);
  spatial_checks();
  mixing_checks();
  music_checks();
  fs::remove_all(dir, ec);
  if (g_fail != 0) {
    std::cerr << "audio smoke: " << g_fail << " check(s) failed\n";
    return 1;
  }
  std::cout << "audio smoke OK (decode wav/ogg/mp3/flac, " << builtin_names().size()
            << " builtins, bus levels, pan/attenuation/doppler, crossfade, playlist)\n";
  return 0;
}
