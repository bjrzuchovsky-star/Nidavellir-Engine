#include "plugins/rider3d/Rider3D.h"

#include "plugins/physics_jolt/JoltWorld.h"

namespace ts {
namespace tombstone {
namespace plugins {

void offer_rider3d();  // Rider3D.cpp

void offer_3d_providers() {
  physics3d::offer_jolt();
  offer_rider3d();
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
