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
// VMT: KeyValues text ("Shader" { "$key" "value" ... }), keys case
// insensitive, // comments, [$PLATFORM] conditionals skipped; "patch"
// materials (include + insert / replace) resolved through a loader,
// nested patches too.
//
// Provenance: written from the public format descriptions only - no
// Source SDK code or headers and no (L)GPL libraries (VTFLib, gmad).

#include "render3d/Mesh.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
namespace source {

bool decode_vtf(const unsigned char* data, std::size_t size, Image* out, std::string* error);
// gfx::ExtraImageDecoder shape: false unless the bytes are a VTF that decodes.
bool decode_vtf_rgba(const unsigned char* data, std::size_t size, std::vector<unsigned char>* rgba,
                     int* width, int* height);
// Size of a w x h image of `format` (VTF enum) in bytes; 0 = unsupported.
std::size_t vtf_image_bytes(int format, int w, int h);
// "DXT1", "BGRA8888"... ("format 27" for unknown ones).
std::string vtf_format_name(int format);

struct VtfInfo {
  int major = 0, minor = 0;
  int width = 0, height = 0, depth = 1;
  int format = -1;
  int mips = 1, frames = 1, faces = 1, first_frame = 0;
  std::uint32_t flags = 0;
  bool normal_map() const { return (flags & 0x80u) != 0; }   // TEXTUREFLAGS_NORMAL
  bool envmap() const { return (flags & 0x4000u) != 0; }
};
// Header only (no pixel decode).
bool vtf_info(const unsigned char* data, std::size_t size, VtfInfo* out, std::string* error);

// KeyValues tree: a block has children, a pair has a value.
struct KeyValue {
  std::string key;     // as written
  std::string value;
  bool block = false;
  std::vector<KeyValue> children;
};
// Parses every top-level entry of a KeyValues text into root->children.
bool parse_keyvalues(const std::string& text, KeyValue* root, std::string* error = nullptr);

struct Vmt {
  std::string shader;                          // lower case ("vertexlitgeneric", "patch")
  std::map<std::string, std::string> params;   // "$basetexture" -> ..., keys lower case
  std::string include;                         // patch materials
  std::map<std::string, std::string> insert;   // patch: added when missing
  std::map<std::string, std::string> replace;  // patch: override
  std::string param(const std::string& key) const {
    auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  }
  bool flag(const std::string& key) const;     // "1" / "true" / non-zero number
};
// One file, includes not followed (patch params left in insert / replace).
bool parse_vmt(const std::string& text, Vmt* out, std::string* error = nullptr);

// Loads an included material's text ("materials/x/y.vmt" as written).
using VmtLoader = std::function<bool(const std::string& include, std::string* text)>;
// Parses and follows patch includes (up to 8 deep): the result has the
// base material's shader and params with insert (when missing) and
// replace (always) applied. `chain` (optional) gets the includes followed.
// A missing include fails with an error naming it; `out` still holds the
// patch's own params.
bool resolve_vmt(const std::string& text, const VmtLoader& load, Vmt* out, std::string* error = nullptr,
                 std::vector<std::string>* chain = nullptr);

}  // namespace source
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
