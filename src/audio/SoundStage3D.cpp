#include "audio/SoundStage3D.h"

#include "world3d/World3D.h"

namespace ts {
namespace audio {

namespace {

Vec3f to_f(const tombstone::render3d::Vec3& v) { return Vec3f{v.x, v.y, v.z}; }

Vec3f velocity(const Vec3f& now, const Vec3f& before, double dt) {
  if (dt <= 1e-6) return Vec3f{};
  const float k = static_cast<float>(1.0 / dt);
  return Vec3f{(now.x - before.x) * k, (now.y - before.y) * k, (now.z - before.z) * k};
}

}  // namespace

void SoundStage3D::update(tombstone::world3d::World3D& world, double dt) {
  stage_.handle_all(world.take_sounds());

  std::vector<SourceState> sources;
  std::map<std::uint64_t, Vec3f> seen;
  for (const tombstone::world3d::Entity3D& e : world.entities()) {
    if (!e.audio) continue;
    SourceState s;
    s.id = e.id;
    s.data = &*e.audio;
    s.position = to_f(e.transform.position);
    auto it = last_.find(e.id);
    if (it != last_.end()) s.velocity = velocity(s.position, it->second, dt);
    seen[e.id] = s.position;
    sources.push_back(s);
  }
  last_.swap(seen);
  stage_.sync_sources(sources);

  tombstone::render3d::Vec3 pos;
  tombstone::render3d::Vec3 fwd;
  tombstone::render3d::Vec3 up;
  world.listener_pose(&pos, &fwd, &up);
  Listener l;
  l.position = to_f(pos);
  l.forward = to_f(fwd);
  l.up = to_f(up);
  if (has_listener_) l.velocity = velocity(l.position, last_listener_, dt);
  last_listener_ = l.position;
  has_listener_ = true;
  stage_.set_listener(l);
}

}  // namespace audio
}  // namespace ts
