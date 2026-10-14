#include "SmokeAudio.h"

#include "SmokeAudioClips.h"

#include "audio/Audio.h"
#include "audio/SoundStage.h"
#include "render3d/Scene3D.h"
#include "runtime/Sound.h"
#include "runtime/World.h"
#include "scene/SceneJson.h"

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
namespace tsb = ts::tombstone;
namespace rt = ts::tombstone::runtime;
namespace sj = ts::tombstone::scene_json;
namespace s3 = ts::tombstone::scene3d;

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


tsb::Entity2D ent(std::uint64_t id, const std::string& name, float x, float y) {
  tsb::Entity2D e;
  e.id = id;
  e.name = name;
  e.x = x;
  e.y = y;
  e.w = 32.0f;
  e.h = 32.0f;
  return e;
}

// Scene format: 2D v8 / 3D v10 carry the audio source; older files load.
void scene_checks() {
  sj::SceneDoc doc;
  tsb::Entity2D a = ent(1, "Jukebox", 100.0f, 50.0f);
  a.audio = tsb::AudioSourceData{};
  a.audio->clip = "assets/sfx/piano.ogg";
  a.audio->bus = "music";
  a.audio->volume = 0.75f;
  a.audio->pitch = 1.25f;
  a.audio->loop = false;
  a.audio->autoplay = false;
  a.audio->spatial = true;
  a.audio->min_distance = 64.0f;
  a.audio->max_distance = 512.0f;
  a.audio->doppler = true;
  doc.entities = {a, ent(2, "Quiet", 0.0f, 0.0f)};
  const std::string text = sj::write(doc);
  sj::SceneDoc back;
  std::string err;
  check(sj::parse(text, &back, &err) && back.entities == doc.entities && sj::write(back) == text,
        "2D audio round trip: " + err);
  check(sj::kSceneVersion == 8 && text.find("\"version\": 8") != std::string::npos,
        "2D writes v8");
  check(text.find("\"audio\"") != std::string::npos &&
            sj::write([&] {
              sj::SceneDoc q = doc;
              q.entities[0].audio.reset();
              return q;
            }()).find("\"audio\"") == std::string::npos,
        "audio only written when present");
  // A v7 file (no audio anywhere) loads unchanged.
  sj::SceneDoc plain = doc;
  plain.entities[0].audio.reset();
  std::string v7 = sj::write(plain);
  v7.replace(v7.find("\"version\": 8"), 12, "\"version\": 7");
  sj::SceneDoc old;
  check(sj::parse(v7, &old, &err) && old.entities == plain.entities, "2D v7 loads: " + err);
  // Bad values clamp, unknown bus falls back to sfx; a non-object rejects.
  std::string wild = text;
  wild.replace(wild.find("\"music\""), 7, "\"banjo\"");
  wild.replace(wild.find("\"volume\": 0.75"), 14, "\"volume\": 9");
  sj::SceneDoc fixed;
  check(sj::parse(wild, &fixed, &err) && fixed.entities[0].audio->bus == "sfx" &&
            fixed.entities[0].audio->volume == tsb::AudioSourceData::kMaxVolume,
        "2D audio normalised");

  // 3D v10.
  s3::Scene3D s = s3::make_starter_scene();
  s.entities[0].audio = s3::audio_3d();
  s.entities[0].audio->clip = "builtin:tune";
  s.entities[0].audio->bus = "music";
  s.entities[0].audio->doppler = true;
  const std::string t3 = s3::write(s);
  s3::Scene3D b3;
  check(s3::parse(t3, &b3, &err) && b3 == s && s3::write(b3) == t3, "3D audio round trip: " + err);
  check(s3::kScene3DVersion == 10 && t3.find("\"version\": 10") != std::string::npos,
        "3D writes v10");
  check(b3.entities[0].audio->min_distance == 2.0f && b3.entities[0].audio->max_distance == 40.0f,
        "3D metre defaults");
  std::string v9 = s3::write(s3::make_starter_scene());
  v9.replace(v9.find("\"version\": 10"), 13, "\"version\": 9");
  s3::Scene3D o3;
  check(s3::parse(v9, &o3, &err) && !o3.entities[0].audio, "3D v9 loads: " + err);
}

// Sound events: wire format, server -> client, Lua, SoundStage.
void event_checks() {
  rt::SoundEvent ev;
  ev.kind = rt::SoundEvent::Kind::Play;
  ev.clip = "builtin:gunshot";
  ev.bus = "sfx";
  ev.volume = 0.8f;
  ev.pitch = 1.1f;
  ev.variation = 0.1f;
  ev.positional = true;
  ev.x = 10.0f;
  ev.y = -4.0f;
  ev.loop = true;
  ev.entity = 42;
  rt::SoundEvent pl;
  pl.kind = rt::SoundEvent::Kind::Playlist;
  pl.list = {"a.ogg", "b.mp3"};
  pl.fade = 2.0f;
  for (const rt::SoundEvent& e : {ev, pl}) {
    rt::SoundEvent back;
    check(rt::decode_sound(rt::encode_sound(e), &back) && back == e, "sound wire round trip");
  }
  std::vector<std::uint8_t> bytes = rt::encode_sound(ev);
  bytes.pop_back();
  rt::SoundEvent junk;
  check(!rt::decode_sound(bytes, &junk), "truncated sound rejected");
  bytes = rt::encode_sound(ev);
  bytes[1] = 99;
  check(!rt::decode_sound(bytes, &junk), "bad kind rejected");

  // Server world posts on ts.audio; a client world queues what arrives; a
  // local world posts nothing; a client cannot make others hear things.
  rt::World server;
  server.set_net_role(rt::NetRole::Server);
  server.play_sound(ev);
  std::vector<rt::NetPost> posts = server.take_net_posts();
  check(posts.size() == 1 && posts[0].channel == rt::kSoundChannel && posts[0].to_slot == -1,
        "server posts the sound");
  rt::World client;
  client.set_net_role(rt::NetRole::Client);
  check(!posts.empty() && client.net_deliver(posts[0].channel, -1, posts[0].bytes),
        "client takes ts.audio");
  std::vector<rt::SoundEvent> heard = client.take_sounds();
  check(heard.size() == 1 && heard[0].clip == ev.clip && heard[0].x == ev.x, "client hears it");
  rt::World local;
  local.play_sound(ev);
  check(local.take_net_posts().empty() && local.take_sounds().size() == 1, "local queues only");
  check(!server.net_deliver(rt::kSoundChannel, 1, posts.empty() ? bytes : posts[0].bytes),
        "server ignores ts.audio from riders");
  for (int i = 0; i < 300; ++i) local.play_sound(ev);
  check(local.pending_sounds().size() == rt::World::kMaxSounds, "queue capped");

  // Lua: audio.* reaches the queue with the right fields.
  rt::World w;
  w.script_library().put("scripts/band.lua",
                         "function on_start()\n"
                         "  audio.play('builtin:click', {bus = 'ui', volume = 0.5})\n"
                         "  audio.play('builtin:step', {at = self, variation = 0.2})\n"
                         "  audio.music('builtin:tune', 2)\n"
                         "  audio.playlist({'builtin:tune', 'builtin:chime'})\n"
                         "  audio.set_volume('music', 0.4)\n"
                         "  audio.stop_music(0.5)\n"
                         "  audio.stop()\n"
                         "  local ok = pcall(audio.play, 'x', {bus = 'banjo'})\n"
                         "  log('bad bus refused', not ok)\n"
                         "end\n");
  tsb::Entity2D band = ent(5, "Band", 200.0f, 100.0f);
  band.script = tsb::ScriptData{};
  band.script->path = "scripts/band.lua";
  std::string err;
  check(w.build({band}, "", &err), "lua world builds: " + err);
  w.step(rt::InputFrame{});
  std::vector<rt::SoundEvent> got = w.take_sounds();
  using K = rt::SoundEvent::Kind;
  check(got.size() == 7, "lua queued 7 sounds, got " + std::to_string(got.size()));
  if (got.size() == 7) {
    check(got[0].bus == "ui" && got[0].volume == 0.5f && !got[0].positional, "audio.play ui");
    check(got[1].positional && got[1].x == 216.0f && got[1].y == 116.0f && got[1].entity == 5 &&
              got[1].variation == 0.2f,
          "audio.play at entity");
    check(got[2].kind == K::Music && got[2].fade == 2.0f, "audio.music");
    check(got[3].kind == K::Playlist && got[3].list.size() == 2, "audio.playlist");
    check(got[4].kind == K::Volume && got[4].bus == "music" && got[4].volume == 0.4f,
          "audio.set_volume");
    check(got[5].kind == K::StopMusic && got[6].kind == K::Stop && got[6].entity == 5,
          "audio.stop_music / stop");
  }
  bool refused = false;
  for (const rt::LogEntry& l : w.take_logs()) refused |= l.text.find("bad bus refused true") != std::string::npos;
  check(refused, "unknown bus refused");

  // SoundStage on the null backend: events play, sources autoplay, follow
  // and stop with their entity; the ride's volume changes are undone.
  Engine e;
  e.start({false, 48000, ""});
  e.set_bus_volume(Bus::Music, 0.9f);
  SoundStage stage(e);
  stage.begin_ride("", false);
  rt::World sw;
  tsb::Entity2D loop = ent(7, "Campfire", 300.0f, 0.0f);
  loop.audio = tsb::AudioSourceData{};
  loop.audio->clip = "builtin:chime";
  tsb::Entity2D once = ent(8, "Bell", 0.0f, 0.0f);
  once.audio = tsb::AudioSourceData{};
  once.audio->clip = "builtin:click";
  once.audio->loop = false;
  tsb::Entity2D off = ent(9, "Silent", 0.0f, 0.0f);
  off.audio = tsb::AudioSourceData{};
  off.audio->clip = "builtin:click";
  off.audio->autoplay = false;
  check(sw.build({loop, once, off}, "", &err), "stage world builds: " + err);
  stage.update_2d(sw, 0.0f, 0.0f);
  check(stage.sources_playing() == 2, "two autoplay sources");
  e.update(0.1);
  stage.update_2d(sw, 0.0f, 0.0f);
  check(stage.sources_playing() == 1, "one-shot autoplay done, loop goes on");
  e.update(0.1);
  stage.update_2d(sw, 0.0f, 0.0f);
  check(stage.sources_playing() == 1, "one-shot does not restart");
  sw.script_set_visible(7, false);
  stage.update_2d(sw, 0.0f, 0.0f);
  check(stage.sources_playing() == 0, "hidden source stops");
  sw.play_sound(ev);  // positional loop from entity 42
  rt::SoundEvent vol;
  vol.kind = rt::SoundEvent::Kind::Volume;
  vol.bus = "music";
  vol.volume = 0.1f;
  sw.play_sound(vol);
  stage.update_2d(sw, 0.0f, 0.0f);
  check(stage.owned_voices(42) == 1 && near(e.bus_volume(Bus::Music), 0.1f), "events applied");
  rt::SoundEvent stop;
  stop.kind = rt::SoundEvent::Kind::Stop;
  stop.entity = 42;
  stop.fade = 0.0f;
  stage.handle(stop);
  check(stage.owned_voices(42) == 0, "stop by entity");
  stage.end_ride(0.0f);
  check(near(e.bus_volume(Bus::Music), 0.9f), "ride volume restored");
  e.update(0.1);
  check(e.voice_count() == 0, "ride end stops its voices");
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
  scene_checks();
  event_checks();
  fs::remove_all(dir, ec);
  if (g_fail != 0) {
    std::cerr << "audio smoke: " << g_fail << " check(s) failed\n";
    return 1;
  }
  std::cout << "audio smoke OK (decode wav/ogg/mp3/flac, " << builtin_names().size()
            << " builtins, bus levels, pan/attenuation/doppler, crossfade, playlist, scene v8/v10, "
               "sound events + Lua + server->client, SoundStage)\n";
  return 0;
}
