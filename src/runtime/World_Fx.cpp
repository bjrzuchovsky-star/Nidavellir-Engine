// Particles + 2D lighting for the 2D world: entity emitters on the tick,
// fx events (Lua particles.*, gameplay, the "ts.fx" channel), footstep
// dust, and the draw-list side (particle quads, the lightmap quad).
#include <algorithm>
#include <cmath>

#include "core/MemImages.h"
#include "runtime/World.h"

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

// Effects pointed with ev.dir (degrees clockwise from +x on screen): the
// preset's launch direction turns with it.
void aim(fx::EmitterDesc& d, float deg) {
  if (deg == 0.0f) return;
  const float a = deg * 3.14159265f / 180.0f;
  const float c = std::cos(a), s = std::sin(a);
  auto turn = [&](float* v) {
    const float x = v[0] * c - v[1] * s, y = v[0] * s + v[1] * c;
    v[0] = x;
    v[1] = y;
  };
  turn(d.direction);
}

}  // namespace

void World::script_fx(FxEvent event) { fx(std::move(event)); }

void World::set_lighting(const fx::Lighting2D& lighting) {
  lighting_ = lighting;
  fx::normalize(lighting_);
}

void World::fx(FxEvent ev) {
  if (ev.at_entity) {
    const Actor* a = find(ev.entity);
    if (!a || a->dead) return;
    ev.x = a->data.center_x();
    ev.y = a->data.center_y();
    ev.at_entity = false;
  }
  ++fx_total_;
  if (!ev.local) {
    // A client's own (predicted / scripted) effects would double up with
    // the server's copy: only the server's count there.
    if (net_role_ == NetRole::Client) return;
    if (net_role_ == NetRole::Server) net_post(kFxChannel, -1, encode_fx(ev));
  }
  apply_fx(ev);
}

void World::apply_fx(const FxEvent& ev) {
  // Scripts start before the first emitter sync: meet the entity now.
  if (ev.kind != FxEvent::Kind::Burst && !particles_.has(ev.entity)) {
    const Actor* a = find(ev.entity);
    if (a && a->data.particles && !a->dead) {
      const float pos[3] = {a->data.center_x(), a->data.center_y(), 0.0f};
      particles_.sync(ev.entity, *a->data.particles, pos);
    }
  }
  switch (ev.kind) {
    case FxEvent::Kind::Burst: {
      std::optional<fx::EmitterDesc> d;
      if (!ev.preset.empty()) {
        d = fx::preset(ev.preset, fx::Space::TwoD);
      } else if (const Actor* a = find(ev.entity); a && a->data.particles) {
        d = *a->data.particles;
      }
      if (!d) {
        post(LogLevel::Warn, "fx", "unknown particle preset \"" + ev.preset + "\"", ev.entity);
        return;
      }
      aim(*d, ev.dir);
      const float pos[3] = {ev.x, ev.y, 0.0f};
      particles_.burst(*d, pos, ev.count);
      break;
    }
    case FxEvent::Kind::Emit:
      particles_.emit(ev.entity, ev.count);
      break;
    case FxEvent::Kind::Stop:
      particles_.stop(ev.entity);
      break;
    case FxEvent::Kind::Start:
      particles_.start(ev.entity);
      break;
  }
}

void World::step_fx() {
  for (const Actor& a : actors_) {
    if (!a.data.particles || a.hidden || a.dead) continue;
    const float pos[3] = {a.data.center_x(), a.data.center_y(), 0.0f};
    particles_.sync(a.data.id, *a.data.particles, pos);
  }
  particles_.prune();
  // Footstep dust: riders on the move kick up a puff every ~0.3 s.
  if (footstep_dust) {
    for (const Actor& a : actors_) {
      if (!a.data.player || a.hidden || a.dead) continue;
      if (std::fabs(a.vx) + std::fabs(a.vy) < 40.0f) continue;
      if ((tick_ + a.data.id * 7) % 18 != 0) continue;
      FxEvent ev;
      ev.preset = "dust";
      ev.local = true;
      ev.count = 4;
      ev.x = a.data.center_x();
      ev.y = a.data.y + a.data.h - 2.0f;
      fx(std::move(ev));
    }
  }
  particles_.update(static_cast<float>(kTickSeconds));
}

void World::build_particles(bool additive, std::vector<DrawQuad>* out) const {
  if (particles_.alive() == 0) return;
  fx::ensure_builtin_images();  // the soft dot ("mem:fx/dot")
  std::vector<fx::Sprite> sprites;
  particles_.sprites(&sprites);
  for (const fx::Sprite& s : sprites) {
    if (s.additive != additive) continue;
    DrawQuad q;
    const float h = s.size * 0.5f;
    q.x0 = s.pos[0] - h;
    q.y0 = s.pos[1] - h;
    q.x1 = s.pos[0] + h;
    q.y1 = s.pos[1] + h;
    for (int i = 0; i < 4; ++i) q.rgba[i] = s.rgba[i];
    q.image = s.image;
    q.u0 = s.u0;
    q.v0 = s.v0;
    q.u1 = s.u1;
    q.v1 = s.v1;
    q.rotation = s.rotation;
    q.blend = additive ? 1 : 0;
    out->push_back(q);
  }
}

bool World::lighting_active(float ambient_out[3]) const {
  float amb[3] = {1, 1, 1};
  bool on = false;
  if (ext_) {
    for (const WorldExtensions::Ambient& a : ext_->ambients) {
      if (a.ambient && a.ambient(*this, amb)) {
        on = true;
        break;
      }
    }
  }
  if (!on && lighting_.enabled) {
    lighting_.ambient_at(time_seconds(), amb);
    on = true;
  }
  if (ambient_out) {
    for (int i = 0; i < 3; ++i) ambient_out[i] = amb[i];
  }
  return on;
}

void World::collect_lights(const WorldRect& view, std::vector<fx::LightInstance>* out) const {
  for (const Actor& a : actors_) {
    if (!a.data.light || a.hidden || a.dead) continue;
    fx::LightInstance L;
    L.x = a.data.center_x();
    L.y = a.data.center_y();
    L.data = *a.data.light;
    out->push_back(L);
  }
  if (ext_) {
    for (const WorldExtensions::LightSource& s : ext_->lights) {
      if (!s.lights) continue;
      try {
        s.lights(*this, view, out);
      } catch (...) {
      }
    }
  }
}

fx::Occluders World::occluders() const {
  fx::Occluders occ;
  if (ext_) {
    for (const WorldExtensions::Occluder& o : ext_->occluders) {
      if (!o.solid) continue;
      occ.cell = o.cell;
      occ.solid = [this, fn = o.solid](int cx, int cy) { return fn(*this, cx, cy); };
      return occ;
    }
  }
  if (grids_.empty()) return occ;
  occ.cell = static_cast<float>(actors_[grids_.front().actor].data.tilemap->tile_size);
  occ.solid = [this, cell = occ.cell](int cx, int cy) {
    const float px = (static_cast<float>(cx) + 0.5f) * cell;
    const float py = (static_cast<float>(cy) + 0.5f) * cell;
    for (const SolidGrid& g : grids_) {
      const Actor& t = actors_[g.actor];
      if (t.dead) continue;
      const TileMapData& tm = *t.data.tilemap;
      const float ts = static_cast<float>(tm.tile_size);
      const int c = static_cast<int>(std::floor((px - t.data.x) / ts));
      const int r = static_cast<int>(std::floor((py - t.data.y) / ts));
      if (tm.in_bounds(c, r) && g.table(tm.at(c, r))) return true;
    }
    return false;
  };
  return occ;
}

void World::build_lightmap(const WorldRect& view, std::vector<DrawQuad>* out) const {
  float amb[3];
  if (!lighting_active(amb)) {
    lightmap_ = fx::Lightmap{};
    return;
  }
  std::vector<fx::LightInstance> lights;
  collect_lights(view, &lights);
  fx::LightmapView v{view.x0, view.y0, view.x1, view.y1};
  // Keep the map near 160 texels across whatever the zoom.
  const float texel = std::max(static_cast<float>(lighting_.resolution),
                               (view.x1 - view.x0) / 200.0f);
  fx::render_lightmap(v, texel, amb, lights, occluders(),
                      static_cast<std::uint32_t>(tick_ / 4), &lightmap_);
  if (lightmap_key_.empty()) {
    static std::uint64_t counter = 0;
    lightmap_key_ = std::string(fx::kLightmapImage) + "/" + std::to_string(++counter);
  }
  mem_images::put(lightmap_key_, lightmap_.width, lightmap_.height, lightmap_.rgba);
  DrawQuad q;
  q.x0 = lightmap_.view.x0;
  q.y0 = lightmap_.view.y0;
  q.x1 = lightmap_.view.x1;
  q.y1 = lightmap_.view.y1;
  q.image = &lightmap_key_;
  q.blend = 2;
  out->push_back(q);
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
