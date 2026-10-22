#pragma once

// Source engine material files, read from the public format descriptions
// (Valve Developer Community wiki: VTF, VMT, KeyValues). Clean-room code.
//
// VTF 7.0 - 7.5: the largest mip of frame 0 / face 0 / slice 0 decoded to
// RGBA8. Formats: DXT1 (+ one-bit alpha), DXT3, DXT5, RGBA8888, ABGR8888,
// ARGB8888, BGRA8888, BGRX8888, RGB888, BGR888 (+ bluescreen), RGB565,
// BGR565, BGRA4444, BGRA5551, BGRX5551, I8, IA88, A8, UV88, UVWQ8888,
// UVLX8888, RGBA16161616 and RGBA16161616F (tone-mapped by clamping).
//
// VMT: KeyValues text ("Shader" { "$key" "value" ... }) with "patch"
// materials (include + insert / replace) followed one level.

#include "render3d/Mesh.h"

#include <cstddef>
#include <map>
#include <string>

namespace ts {
namespace tombstone {
namespace render3d {
namespace source {

bool decode_vtf(const unsigned char* data, std::size_t size, Image* out, std::string* error);
// Size of a w x h image of `format` (VTF enum) in bytes; 0 = unsupported.
std::size_t vtf_image_bytes(int format, int w, int h);

struct Vmt {
  std::string shader;                          // lower case
  std::map<std::string, std::string> params;   // "$basetexture" -> ..., keys lower case
  std::string include;                         // patch materials
  std::string param(const std::string& key) const {
    auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  }
};
bool parse_vmt(const std::string& text, Vmt* out, std::string* error = nullptr);

}  // namespace source
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
