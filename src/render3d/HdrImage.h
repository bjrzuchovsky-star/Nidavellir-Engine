#pragma once

#include "render3d/Mesh.h"

#include <string>

namespace ts {
namespace tombstone {
namespace render3d {

// .hdr (Radiance RGBE) and uncompressed / RLE / ZIP .exr, stored as RGBM
// in an RGBA8 image. range is the RGBM multiplier (16 for these files, 0
// when the loader fell back to a straight 8-bit image).
bool load_hdr_rgba(const std::string& path, Image* out, float* rgbm_range, std::string* error);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
