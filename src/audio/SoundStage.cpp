#include "audio/SoundStage.h"

#include "runtime/World.h"

#include <algorithm>

namespace ts {
namespace audio {

using tombstone::AudioSourceData;
using tombstone::runtime::SoundEvent;

SoundStage::SoundStage(Engine& engine) : engine_(engine) { saved_volume_.fill(1.0f); }

SoundStage::~SoundStage() {
  if (riding_) end_ride(0.0f);
}

void SoundStage::begin_ride(const std::string& project_dir, bool three_d) {
  if (riding_) end_ride(0.0f);
  riding_ = true;
  three_d_ = three_d;
  engine_.set_project_dir(project_dir);
  for (int b = 0; b < kBusCount; ++b) {
    saved_volume_[static_cast<std::size_t>(b)] = engine_.bus_volume(static_cast<Bus>(b));
  }
  sources_.clear();
  finished_.clear();
  owned_.clear();
  events_ = 0;
  engine_.set_listener(three_d ? Listener() : listener_2d(0.0f, 0.0f));
}

void SoundStage::end_ride(float music_fade) {
  if (!riding_) return;
  riding_ = false;
  for (const auto& [id, v] : sources_) engine_.stop(v, 0.05f);
  for (const auto& [id, v] : owned_) engine_.stop(v, 0.05f);
  sources_.clear();
  owned_.clear();
  finished_.clear();
  engine_.stop_music(music_fade);
  for (int b = 0; b < kBusCount; ++b) {
    engine_.set_bus_volume(static_cast<Bus>(b), saved_volume_[static_cast<std::size_t>(b)]);
  }
}

PlayParams SoundStage::params_for(const AudioSourceData& a) const {
  PlayParams p;
  if (!parse_bus(a.bus, &p.bus)) p.bus = Bus::Sfx;
  p.volume = a.volume;
  p.pitch = a.pitch;
  p.loop = a.loop;
  p.spatial = a.spatial;
  p.min_distance = a.min_distance;
  p.max_distance = a.max_distance;
  p.doppler = a.doppler;
  return p;
}

void SoundStage::prune() {
  for (auto it = owned_.begin(); it != owned_.end();) {
    it = engine_.playing(it->second) ? std::next(it) : owned_.erase(it);
  }
}

void SoundStage::handle(const SoundEvent& ev) {
  ++events_;
  Bus bus = Bus::Sfx;
  parse_bus(ev.bus, &bus);
  switch (ev.kind) {
    case SoundEvent::Kind::Play: {
      PlayParams p;
      p.bus = bus == Bus::Master ? Bus::Sfx : bus;
      p.volume = ev.volume;
      p.pitch = ev.pitch;
      p.volume_variation = ev.variation;
      p.pitch_variation = ev.variation * 0.5f;
      p.loop = ev.loop;
      if (ev.positional) {
        p.spatial = true;
        p.position = {ev.x, ev.y, ev.z};
        const bool metres = three_d_ || ev.three_d;
        p.min_distance = metres ? 2.0f : 200.0f;
        p.max_distance = metres ? 40.0f : 1400.0f;
      }
      const VoiceId v = engine_.play(ev.clip, p);
      if (v != 0 && ev.entity != 0) {
        prune();
        if (owned_.size() < 512) owned_.emplace(ev.entity, v);
      }
      break;
    }
    case SoundEvent::Kind::Music:
      engine_.play_music(ev.clip, ev.fade, ev.loop);
      break;
    case SoundEvent::Kind::StopMusic:
      engine_.stop_music(ev.fade);
      break;
    case SoundEvent::Kind::Playlist:
      engine_.set_playlist(ev.list, ev.fade);
      break;
    case SoundEvent::Kind::Volume:
      engine_.set_bus_volume(bus, ev.volume);
      break;
    case SoundEvent::Kind::Stop: {
      auto [lo, hi] = owned_.equal_range(ev.entity);
      for (auto it = lo; it != hi; ++it) engine_.stop(it->second, ev.fade);
      owned_.erase(lo, hi);
      break;
    }
  }
}

void SoundStage::handle_all(const std::vector<SoundEvent>& events) {
  for (const SoundEvent& ev : events) handle(ev);
}

void SoundStage::sync_sources(const std::vector<SourceState>& sources) {
  std::set<std::uint64_t> seen;
  for (const SourceState& s : sources) {
    if (!s.data || s.data->clip.empty()) continue;
    seen.insert(s.id);
    auto it = sources_.find(s.id);
    if (it != sources_.end()) {
      if (engine_.playing(it->second)) {
        engine_.set_voice_position(it->second, s.position, s.velocity);
        continue;
      }
      // A one-shot that ran out: done for this ride.
      sources_.erase(it);
      if (!s.data->loop) finished_.insert(s.id);
      continue;
    }
    if (!s.data->autoplay || finished_.count(s.id)) continue;
    PlayParams p = params_for(*s.data);
    p.position = s.position;
    p.velocity = s.velocity;
    const VoiceId v = engine_.play(s.data->clip, p);
    if (v != 0) {
      sources_[s.id] = v;
    } else {
      finished_.insert(s.id);  // missing file: do not retry every frame
    }
  }
  for (auto it = sources_.begin(); it != sources_.end();) {
    if (seen.count(it->first)) {
      ++it;
      continue;
    }
    engine_.stop(it->second, 0.05f);
    it = sources_.erase(it);
  }
}

void SoundStage::update_2d(tombstone::runtime::World& world, float cam_x, float cam_y) {
  handle_all(world.take_sounds());
  std::vector<SourceState> states;
  for (const tombstone::runtime::Actor& a : world.actors()) {
    if (!a.data.audio || a.hidden || a.dead) continue;
    SourceState s;
    s.id = a.data.id;
    s.data = &*a.data.audio;
    s.position = {a.data.center_x(), a.data.center_y(), 0.0f};
    s.velocity = {a.vx, a.vy, 0.0f};
    states.push_back(s);
  }
  sync_sources(states);
  Listener l = listener_2d(cam_x, cam_y);
  engine_.set_listener(l);
}

std::size_t SoundStage::sources_playing() const {
  std::size_t n = 0;
  for (const auto& [id, v] : sources_) n += engine_.playing(v) ? 1 : 0;
  return n;
}

std::size_t SoundStage::owned_voices(std::uint64_t entity) const {
  std::size_t n = 0;
  auto [lo, hi] = owned_.equal_range(entity);
  for (auto it = lo; it != hi; ++it) n += engine_.playing(it->second) ? 1 : 0;
  return n;
}

}  // namespace audio
}  // namespace ts
