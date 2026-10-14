// ts_audio mixer: buses, voices, music decks, spatial math. See Audio.h.

#include "audio/Audio.h"
#include "audio/Backend.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ts {
namespace audio {

namespace {

constexpr const char* kBusNames[kBusCount] = {"master", "music", "sfx", "ui", "voice"};
constexpr const char* kBusLabels[kBusCount] = {"Master", "Music", "SFX", "UI", "Voice"};

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

Vec3f sub(const Vec3f& a, const Vec3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3f cross(const Vec3f& a, const Vec3f& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const Vec3f& a) { return std::sqrt(dot(a, a)); }

}  // namespace

const char* bus_name(Bus bus) {
  const int i = static_cast<int>(bus);
  return i >= 0 && i < kBusCount ? kBusNames[i] : "sfx";
}
const char* bus_label(Bus bus) {
  const int i = static_cast<int>(bus);
  return i >= 0 && i < kBusCount ? kBusLabels[i] : "SFX";
}
bool parse_bus(const std::string& name, Bus* out) {
  const std::string n = lower(name);
  for (int i = 0; i < kBusCount; ++i) {
    if (n == kBusNames[i]) {
      if (out) *out = static_cast<Bus>(i);
      return true;
    }
  }
  return false;
}

bool is_audio_file(const std::string& path) {
  const std::string ext = lower(std::filesystem::path(path).extension().string());
  return ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac";
}

bool write_wav(const std::string& path, const Clip& clip) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  const std::uint32_t data_bytes = static_cast<std::uint32_t>(clip.samples.size() * 2);
  auto u32 = [&](std::uint32_t v) {
    const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                       static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
    f.write(b, 4);
  };
  auto u16 = [&](std::uint16_t v) {
    const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
    f.write(b, 2);
  };
  f.write("RIFF", 4);
  u32(36 + data_bytes);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(static_cast<std::uint16_t>(clip.channels));
  u32(static_cast<std::uint32_t>(clip.rate));
  u32(static_cast<std::uint32_t>(clip.rate * clip.channels * 2));
  u16(static_cast<std::uint16_t>(clip.channels * 2));
  u16(16);
  f.write("data", 4);
  u32(data_bytes);
  for (float s : clip.samples) {
    const long v = std::lround(clampf(s, -1.0f, 1.0f) * 32767.0f);
    u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
  }
  return static_cast<bool>(f);
}

// --- Spatial math --------------------------------------------------------

PanGains pan_gains(float pan) {
  const float p = clampf(pan, -1.0f, 1.0f);
  const float angle = (p + 1.0f) * 0.25f * 3.14159265f;  // 0..pi/2
  return {std::cos(angle), std::sin(angle)};
}

float attenuation(float distance, float min_distance, float max_distance) {
  if (min_distance < 0.0f) min_distance = 0.0f;
  if (distance <= min_distance) return 1.0f;
  if (max_distance <= min_distance || distance >= max_distance) return 0.0f;
  return 1.0f - (distance - min_distance) / (max_distance - min_distance);
}

float doppler_factor(float listener_toward_source, float source_toward_listener,
                     float speed_of_sound) {
  const float c = speed_of_sound > 1.0f ? speed_of_sound : 343.0f;
  const float den = std::max(c - source_toward_listener, c * 0.1f);
  return clampf((c + listener_toward_source) / den, 0.5f, 2.0f);
}

Listener listener_2d(float x, float y) {
  Listener l;
  // Sitting a little in front of the screen so pan eases across the view
  // instead of snapping hard left/right at the listener's x.
  l.position = {x, y, -160.0f};
  l.forward = {0.0f, 0.0f, 1.0f};
  l.up = {0.0f, -1.0f, 0.0f};
  return l;
}

Spatial spatialize(const Listener& listener, const Vec3f& source,
                   const Vec3f& source_velocity, float min_distance,
                   float max_distance, bool doppler, float speed_of_sound) {
  Spatial s;
  const Vec3f d = sub(source, listener.position);
  const float dist = length(d);
  s.gain = attenuation(dist, min_distance, max_distance);
  if (dist > 1e-4f) {
    Vec3f right = cross(listener.forward, listener.up);
    const float rl = length(right);
    if (rl > 1e-6f) {
      right = {right.x / rl, right.y / rl, right.z / rl};
      s.pan = clampf(dot(d, right) / dist, -1.0f, 1.0f);
    }
    if (doppler) {
      const Vec3f dir{d.x / dist, d.y / dist, d.z / dist};
      s.pitch = doppler_factor(dot(listener.velocity, dir),
                               -dot(source_velocity, dir), speed_of_sound);
    }
  }
  return s;
}

// --- Engine ----------------------------------------------------------------

struct Engine::Voice {
  VoiceId id = 0;
  std::string ref;
  ClipPtr clip;
  PlayParams p;
  float volume = 1.0f;  // after variation
  float pitch = 1.0f;
  double cursor = 0.0;
  float gl = -1.0f, gr = -1.0f;  // last applied gains (-1 = first block)
  float fade = 1.0f;
  float fade_step = 0.0f;  // per frame, negative when stopping
  bool done = false;
};

struct Engine::Deck {
  std::string ref;
  std::unique_ptr<detail::Stream> stream;
  ClipPtr clip;
  double cursor = 0.0;
  bool loop = true;
  bool ended = false;
  float gain = 0.0f;
  float target = 1.0f;
  float step = 1.0f;  // gain change per frame
};

struct Engine::Device {
  std::unique_ptr<detail::DeviceHandle> handle;
};

namespace {
void device_mix(void* user, float* out, int frames) {
  static_cast<Engine*>(user)->mix(out, frames);
}
}  // namespace

Engine::Engine() {
  volume_.fill(1.0f);
  muted_.fill(false);
  peak_.fill(0.0f);
}

Engine::~Engine() { shutdown(); }

bool Engine::start(const EngineOptions& options) {
  shutdown();
  rate_ = options.rate >= 8000 ? options.rate : 48000;
  project_dir_ = options.project_dir;
  started_ = true;
  bool want_device = options.device;
  if (const char* env = std::getenv("TS_AUDIO")) {
    if (lower(env) == "null") want_device = false;
  }
  if (want_device) {
    std::string err;
    auto handle = detail::open_device(rate_, &device_mix, this, &err);
    if (handle) {
      device_ = std::make_unique<Device>();
      device_->handle = std::move(handle);
    }
  }
  return true;
}

void Engine::shutdown() {
  // The device thread takes the mutex in mix(): close it unlocked.
  std::unique_ptr<Device> dev = std::move(device_);
  dev.reset();
  std::lock_guard<std::mutex> lock(mutex_);
  voices_.clear();
  decks_[0].reset();
  decks_[1].reset();
  started_ = false;
}

bool Engine::has_device() const { return device_ != nullptr; }

std::string Engine::backend_name() const {
  return device_ ? device_->handle->name() : std::string("null");
}

void Engine::set_project_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (dir != project_dir_) {
    project_dir_ = dir;
    cache_.clear();
  }
}

std::string Engine::resolve(const std::string& ref) const {
  if (ref.empty() || is_builtin(ref)) return ref;
  std::filesystem::path p(ref);
  if (p.is_absolute() || project_dir_.empty()) return ref;
  return (std::filesystem::path(project_dir_) / p).string();
}

ClipPtr Engine::load(const std::string& ref, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [r, c] : cache_) {
      if (r == ref) return c;
    }
  }
  ClipPtr clip;
  if (is_builtin(ref)) {
    clip = synth_builtin(ref, rate_);
    if (!clip && error) *error = "unknown builtin sound " + ref;
  } else {
    auto c = std::make_shared<Clip>();
    if (decode_file(resolve(ref), rate_, c.get(), error)) clip = c;
  }
  if (!clip) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  if (cache_.size() >= 256) cache_.erase(cache_.begin());
  cache_.emplace_back(ref, clip);
  return clip;
}

void Engine::clear_cache() {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
}

void Engine::set_seed(std::uint32_t seed) {
  std::lock_guard<std::mutex> lock(mutex_);
  rng_ = seed ? seed : 1u;
}

float Engine::rand01_locked() {
  rng_ = rng_ * 1664525u + 1013904223u;
  return static_cast<float>(rng_ >> 8) / 16777216.0f;
}

VoiceId Engine::play(const std::string& ref, const PlayParams& params) {
  ClipPtr clip = load(ref);
  if (!clip) return 0;
  return play(std::move(clip), params, ref);
}

VoiceId Engine::play(ClipPtr clip, const PlayParams& params, const std::string& ref) {
  if (!clip || clip->frames() == 0) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (voices_.size() >= kMaxVoices) {
    // Steal the oldest one-shot; loops keep their place.
    auto it = std::find_if(voices_.begin(), voices_.end(),
                           [](const auto& v) { return !v->p.loop; });
    if (it == voices_.end()) return 0;
    voices_.erase(it);
  }
  auto v = std::make_unique<Voice>();
  v->id = next_voice_++;
  if (next_voice_ == 0) next_voice_ = 1;
  v->ref = ref;
  v->clip = std::move(clip);
  v->p = params;
  if (v->p.bus == Bus::Master || v->p.bus == Bus::Count) v->p.bus = Bus::Sfx;
  const float vv = clampf(params.volume_variation, 0.0f, 1.0f);
  const float pv = clampf(params.pitch_variation, 0.0f, 0.9f);
  v->volume = std::max(0.0f, params.volume * (1.0f + vv * (rand01_locked() * 2.0f - 1.0f)));
  v->pitch = clampf(params.pitch * (1.0f + pv * (rand01_locked() * 2.0f - 1.0f)), 0.05f, 8.0f);
  const VoiceId id = v->id;
  voices_.push_back(std::move(v));
  return id;
}

void Engine::stop(VoiceId id, float fade_seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& v : voices_) {
    if (v->id != id) continue;
    if (fade_seconds <= 0.0f) {
      v->done = true;
    } else {
      v->fade_step = -1.0f / (fade_seconds * static_cast<float>(rate_));
    }
  }
}

void Engine::stop_bus(Bus bus, float fade_seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& v : voices_) {
    if (v->p.bus != bus && bus != Bus::Master) continue;
    if (fade_seconds <= 0.0f) {
      v->done = true;
    } else {
      v->fade_step = -1.0f / (fade_seconds * static_cast<float>(rate_));
    }
  }
}

void Engine::stop_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  voices_.clear();
  decks_[0].reset();
  decks_[1].reset();
  playlist_.clear();
  playlist_index_ = -1;
}

bool Engine::playing(VoiceId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& v : voices_) {
    if (v->id == id) return !v->done;
  }
  return false;
}

void Engine::set_voice_position(VoiceId id, const Vec3f& pos, const Vec3f& vel) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& v : voices_) {
    if (v->id == id) {
      v->p.position = pos;
      v->p.velocity = vel;
    }
  }
}

void Engine::set_voice_volume(VoiceId id, float volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& v : voices_) {
    if (v->id == id) v->volume = std::max(0.0f, volume);
  }
}

std::size_t Engine::voice_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return voices_.size();
}

std::vector<VoiceInfo> Engine::voices() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<VoiceInfo> out;
  for (const auto& v : voices_) {
    out.push_back({v->id, v->ref, v->p.bus, std::max(v->gl, v->gr), v->p.loop});
  }
  return out;
}

void Engine::set_listener(const Listener& listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = listener;
}

Listener Engine::listener() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return listener_;
}

void Engine::set_bus_volume(Bus bus, float volume) {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= kBusCount) return;
  std::lock_guard<std::mutex> lock(mutex_);
  volume_[static_cast<std::size_t>(i)] = clampf(volume, 0.0f, 2.0f);
}

float Engine::bus_volume(Bus bus) const {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= kBusCount) return 0.0f;
  std::lock_guard<std::mutex> lock(mutex_);
  return volume_[static_cast<std::size_t>(i)];
}

void Engine::set_bus_muted(Bus bus, bool muted) {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= kBusCount) return;
  std::lock_guard<std::mutex> lock(mutex_);
  muted_[static_cast<std::size_t>(i)] = muted;
}

bool Engine::bus_muted(Bus bus) const {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= kBusCount) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  return muted_[static_cast<std::size_t>(i)];
}

float Engine::bus_peak(Bus bus) {
  const int i = static_cast<int>(bus);
  if (i < 0 || i >= kBusCount) return 0.0f;
  std::lock_guard<std::mutex> lock(mutex_);
  const float p = peak_[static_cast<std::size_t>(i)];
  peak_[static_cast<std::size_t>(i)] = 0.0f;
  return p;
}

// --- Music --------------------------------------------------------------

bool Engine::play_music(const std::string& ref, float fade_seconds, bool loop) {
  auto deck = std::make_unique<Deck>();
  deck->ref = ref;
  deck->loop = loop;
  if (is_builtin(ref)) {
    deck->clip = load(ref);
    if (!deck->clip) return false;
  } else {
    std::string err;
    deck->stream = detail::open_stream(resolve(ref), rate_, &err);
    if (!deck->stream) return false;
  }
  const float frames = std::max(1.0f, fade_seconds * static_cast<float>(rate_));
  std::lock_guard<std::mutex> lock(mutex_);
  const int next = 1 - active_deck_;
  if (decks_[static_cast<std::size_t>(active_deck_)]) {
    Deck& old = *decks_[static_cast<std::size_t>(active_deck_)];
    old.target = 0.0f;
    old.step = 1.0f / frames;
  }
  deck->gain = fade_seconds > 0.0f ? 0.0f : 1.0f;
  deck->target = 1.0f;
  deck->step = 1.0f / frames;
  decks_[static_cast<std::size_t>(next)] = std::move(deck);
  active_deck_ = next;
  return true;
}

void Engine::stop_music(float fade_seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  playlist_.clear();
  playlist_index_ = -1;
  const float frames = std::max(1.0f, fade_seconds * static_cast<float>(rate_));
  for (auto& d : decks_) {
    if (!d) continue;
    if (fade_seconds <= 0.0f) {
      d.reset();
    } else {
      d->target = 0.0f;
      d->step = 1.0f / frames;
    }
  }
}

std::string Engine::current_music() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& d = decks_[static_cast<std::size_t>(active_deck_)];
  return d && d->target > 0.0f ? d->ref : std::string();
}

float Engine::music_deck_gain(int deck) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const int i = deck == 0 ? active_deck_ : 1 - active_deck_;
  const auto& d = decks_[static_cast<std::size_t>(i)];
  return d ? d->gain : 0.0f;
}

void Engine::set_playlist(const std::vector<std::string>& refs, float fade_seconds,
                          bool shuffle) {
  std::vector<std::string> list = refs;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shuffle) {
      for (std::size_t i = list.size(); i > 1; --i) {
        const std::size_t j = static_cast<std::size_t>(rand01_locked() * static_cast<float>(i)) % i;
        std::swap(list[i - 1], list[j]);
      }
    }
    playlist_ = list;
    playlist_index_ = -1;
    playlist_fade_ = fade_seconds;
  }
  if (!list.empty()) next_track(fade_seconds);
}

void Engine::next_track(float fade_seconds) {
  std::vector<std::string> list;
  int index = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_advance_ = false;
    if (playlist_.empty()) return;
    list = playlist_;
    index = (playlist_index_ + 1) % static_cast<int>(list.size());
  }
  // A track that will not open is skipped (once round the list at most).
  for (std::size_t tries = 0; tries < list.size(); ++tries) {
    const int i = (index + static_cast<int>(tries)) % static_cast<int>(list.size());
    if (play_music(list[static_cast<std::size_t>(i)], fade_seconds, false)) {
      std::lock_guard<std::mutex> lock(mutex_);
      playlist_ = list;  // play_music leaves the list alone
      playlist_index_ = i;
      return;
    }
  }
}

// --- Mixing --------------------------------------------------------------

void Engine::mix(float* out, int frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  mix_locked(out, frames);
}

void Engine::mix_locked(float* out, int frames) {
  if (frames <= 0) return;
  const std::size_t n = static_cast<std::size_t>(frames) * 2;
  bus_buf_.assign(n * kBusCount, 0.0f);
  auto bus_ptr = [&](Bus b) { return bus_buf_.data() + n * static_cast<std::size_t>(b); };

  for (auto& vp : voices_) {
    Voice& v = *vp;
    if (v.done) continue;
    float gain = v.volume;
    float pan = v.p.pan;
    float pitch = v.pitch;
    if (v.p.spatial) {
      const Spatial s = spatialize(listener_, v.p.position, v.p.velocity,
                                   v.p.min_distance, v.p.max_distance, v.p.doppler);
      gain *= s.gain;
      pan = s.pan;
      pitch *= s.pitch;
    }
    const Clip& c = *v.clip;
    float tl, tr;
    if (c.channels == 1) {
      const PanGains g = pan_gains(pan);
      tl = g.left * gain;
      tr = g.right * gain;
    } else {
      // Stereo clips keep unity at centre and balance towards a side.
      tl = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
      tr = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
    }
    if (v.gl < 0.0f) {
      v.gl = tl;
      v.gr = tr;
    }
    const float dl = (tl - v.gl) / static_cast<float>(frames);
    const float dr = (tr - v.gr) / static_cast<float>(frames);
    float* dst = bus_ptr(v.p.bus);
    const double inc = static_cast<double>(pitch) * c.rate / rate_;
    const std::size_t total = c.frames();
    float gl = v.gl, gr = v.gr;
    for (int i = 0; i < frames; ++i) {
      gl += dl;
      gr += dr;
      if (v.fade_step != 0.0f) {
        v.fade += v.fade_step;
        if (v.fade <= 0.0f) {
          v.done = true;
          break;
        }
      }
      std::size_t i0 = static_cast<std::size_t>(v.cursor);
      if (i0 >= total) {
        if (!v.p.loop) {
          v.done = true;
          break;
        }
        v.cursor = std::fmod(v.cursor, static_cast<double>(total));
        i0 = static_cast<std::size_t>(v.cursor);
      }
      const float frac = static_cast<float>(v.cursor - static_cast<double>(i0));
      std::size_t i1 = i0 + 1;
      if (i1 >= total) i1 = v.p.loop ? 0 : i0;
      float l, r;
      if (c.channels == 1) {
        const float s = c.samples[i0] + (c.samples[i1] - c.samples[i0]) * frac;
        l = r = s;
      } else {
        const float* a = &c.samples[i0 * 2];
        const float* b = &c.samples[i1 * 2];
        l = a[0] + (b[0] - a[0]) * frac;
        r = a[1] + (b[1] - a[1]) * frac;
      }
      dst[2 * i] += l * gl * v.fade;
      dst[2 * i + 1] += r * gr * v.fade;
      v.cursor += inc;
    }
    v.gl = tl;
    v.gr = tr;
  }
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                               [](const auto& v) { return v->done; }),
                voices_.end());

  // Music decks.
  scratch_.resize(n);
  float* music = bus_ptr(Bus::Music);
  for (std::size_t di = 0; di < 2; ++di) {
    auto& dp = decks_[di];
    if (!dp) continue;
    Deck& d = *dp;
    int got = 0;
    if (d.stream) {
      bool rewound = false;
      while (got < frames && !d.ended) {
        const int r = d.stream->read(scratch_.data() + 2 * got, frames - got);
        got += r;
        if (got < frames) {
          if (d.loop && !rewound && d.stream->rewind()) {
            rewound = true;
            continue;
          }
          d.ended = true;
        }
      }
    } else if (d.clip) {
      const Clip& c = *d.clip;
      const std::size_t total = c.frames();
      while (got < frames && !d.ended) {
        std::size_t i0 = static_cast<std::size_t>(d.cursor);
        if (i0 >= total) {
          if (!d.loop) {
            d.ended = true;
            break;
          }
          d.cursor = 0.0;
          i0 = 0;
        }
        const float l = c.channels == 1 ? c.samples[i0] : c.samples[i0 * 2];
        const float r = c.channels == 1 ? c.samples[i0] : c.samples[i0 * 2 + 1];
        scratch_[2 * static_cast<std::size_t>(got)] = l;
        scratch_[2 * static_cast<std::size_t>(got) + 1] = r;
        d.cursor += static_cast<double>(c.rate) / rate_;
        ++got;
      }
    }
    for (int i = 0; i < got; ++i) {
      if (d.gain < d.target) d.gain = std::min(d.target, d.gain + d.step);
      else if (d.gain > d.target) d.gain = std::max(d.target, d.gain - d.step);
      music[2 * i] += scratch_[2 * static_cast<std::size_t>(i)] * d.gain;
      music[2 * i + 1] += scratch_[2 * static_cast<std::size_t>(i) + 1] * d.gain;
    }
    const bool faded_out = d.target <= 0.0f && d.gain <= 0.0f;
    if (d.ended && static_cast<int>(di) == active_deck_ && !playlist_.empty() &&
        d.target > 0.0f) {
      playlist_advance_ = true;
    }
    if (faded_out || (d.ended && got == 0)) dp.reset();
  }

  // Buses -> master -> out.
  std::fill(out, out + n, 0.0f);
  const float master = muted_[0] ? 0.0f : volume_[0];
  for (int b = 1; b < kBusCount; ++b) {
    const float g = muted_[static_cast<std::size_t>(b)] ? 0.0f : volume_[static_cast<std::size_t>(b)];
    const float* src = bus_ptr(static_cast<Bus>(b));
    float pk = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
      const float s = src[i] * g;
      pk = std::max(pk, std::fabs(s));
      out[i] += s;
    }
    peak_[static_cast<std::size_t>(b)] = std::max(peak_[static_cast<std::size_t>(b)], pk);
  }
  float pk = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = clampf(out[i] * master, -1.0f, 1.0f);
    pk = std::max(pk, std::fabs(out[i]));
  }
  peak_[0] = std::max(peak_[0], pk);
  frames_mixed_ += static_cast<std::uint64_t>(frames);
}

void Engine::update(double dt_seconds) {
  if (!started_) return;
  if (!device_ && dt_seconds > 0.0) {
    // Null backend: run the mixer so time passes for voices and decks.
    int frames = static_cast<int>(std::min(dt_seconds, 1.0) * rate_);
    std::vector<float> sink(static_cast<std::size_t>(std::max(frames, 0)) * 2);
    std::lock_guard<std::mutex> lock(mutex_);
    while (frames > 0) {
      const int chunk = std::min(frames, 1024);
      mix_locked(sink.data(), chunk);
      frames -= chunk;
    }
  }
  bool advance = false;
  float fade = 1.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    advance = playlist_advance_;
    fade = playlist_fade_;
  }
  if (advance) next_track(fade);
}

}  // namespace audio
}  // namespace ts
