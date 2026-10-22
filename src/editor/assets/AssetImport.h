#pragma once

// Importers behind the asset database: PNG / JPG (stb_image), 3D models
// (render3d::load_model: glTF via cgltf, Source .mdl, Assimp formats,
// .blend through Blender), WAV / OGG / MP3 / FLAC (ts_audio
// decoders) and TrueType / OpenType fonts (sfnt tables read directly).
// Each validates the file and reports what it found in the .meta "info".

#include "editor/assets/AssetDb.h"

#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace assetdb {

bool import_asset(const std::string& abs_path, Kind kind, const json::Value& settings,
                  json::Value* info, std::string* error);

// sfnt reader: family name (name id 1) and glyph count (maxp).
bool read_font_info(const std::string& abs_path, std::string* family, int* glyphs,
                    std::string* error);

// One-line summary of an entry's info ("64x32", "1 mesh, 12 tris", "1.5 s
// stereo", "Rye, 250 glyphs").
std::string info_summary(const json::Value& info, Kind kind);

}  // namespace assetdb
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
