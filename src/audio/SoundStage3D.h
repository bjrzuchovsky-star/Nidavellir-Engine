#pragma once

// SoundStage for a World3D ride (editor Play / VR preview, ts_game): drains
// the world's sound events, keeps 3D audio sources positioned, and puts the
// listener on the camera -- or the headset in VR. Library ts_audio3d (built
// beside ts_world3d).

#include "audio/SoundStage.h"

#include <cstdint>
#include <map>

namespace ts {
namespace tombstone {
namespace world3d {
class World3D;
}  // namespace world3d
}  // namespace tombstone

namespace audio {

class SoundStage3D {
 public:
  explicit SoundStage3D(SoundStage& stage) : stage_(stage) {}
  // dt (seconds) turns position deltas into velocities (doppler).
  void update(tombstone::world3d::World3D& world, double dt);
  void reset() { last_.clear(); has_listener_ = false; }

 private:
  SoundStage& stage_;
  std::map<std::uint64_t, Vec3f> last_;
  Vec3f last_listener_;
  bool has_listener_ = false;
};

}  // namespace audio
}  // namespace ts
