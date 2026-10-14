#pragma once

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace assets {

// Project images live under <project>/assets/. Paths stored in scene.json
// are project-relative with forward slashes ("assets/rider.png").
inline constexpr const char* kAssetsFolder = "assets";
// ImGui drag-and-drop payload type for an asset (data = NUL-terminated
// project-relative path). Supply Wagon -> Viewport / Inspector.
inline constexpr const char* kDragPayload = "TS_ASSET_IMAGE";

std::string assets_dir(const std::string& project_dir);
// Creates <project>/assets if missing. False (with error) on failure.
bool ensure_assets_dir(const std::string& project_dir,
                       std::string* error_out = nullptr);
// png/jpg/jpeg/bmp/tga/gif/psd/pnm/ppm/pgm (what stb_image decodes).
bool is_image_path(const std::string& path);
// Sorted project-relative paths of every image under assets/ (recursive).
std::vector<std::string> list_images(const std::string& project_dir);
// .wav / .ogg / .mp3 / .flac.
bool is_audio_path(const std::string& path);
// Sorted project-relative paths of every sound under assets/ (recursive).
std::vector<std::string> list_sounds(const std::string& project_dir);
// Copy an outside image into assets/ (name_2.png etc. on collision).
// rel_out receives the project-relative path.
bool import_image(const std::string& project_dir, const std::string& source,
                  std::string* rel_out, std::string* error_out = nullptr);
// Absolute-or-cwd-relative path for a project-relative asset path.
std::string resolve(const std::string& project_dir, const std::string& rel);
// File name without folders ("assets/props/crate.png" -> "crate.png").
std::string file_name(const std::string& rel);
// File stem ("assets/props/crate.png" -> "crate").
std::string file_stem(const std::string& rel);

}  // namespace assets
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
