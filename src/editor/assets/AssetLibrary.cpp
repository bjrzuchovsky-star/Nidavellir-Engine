#include "editor/assets/AssetLibrary.h"

#include "audio/Audio.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace ts {
namespace tombstone {
namespace editor {
namespace assets {

namespace fs = std::filesystem;

std::string assets_dir(const std::string& project_dir) {
  return (fs::path(project_dir) / kAssetsFolder).string();
}

bool ensure_assets_dir(const std::string& project_dir, std::string* error_out) {
  if (project_dir.empty()) {
    if (error_out) *error_out = "project path is empty";
    return false;
  }
  std::error_code ec;
  const fs::path dir = fs::path(project_dir) / kAssetsFolder;
  if (fs::is_directory(dir, ec)) {
    return true;
  }
  fs::create_directories(dir, ec);
  if (ec) {
    if (error_out) {
      *error_out = "Could not create " + dir.string() + ": " + ec.message();
    }
    return false;
  }
  return true;
}

bool is_image_path(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  static const char* kExts[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga",
                                ".gif", ".psd", ".pnm",  ".ppm", ".pgm"};
  for (const char* e : kExts) {
    if (ext == e) {
      return true;
    }
  }
  return false;
}

namespace {

std::vector<std::string> list_matching(const std::string& project_dir,
                                       bool (*keep)(const std::string&)) {
  std::vector<std::string> out;
  if (project_dir.empty()) {
    return out;
  }
  const fs::path root = fs::path(project_dir);
  const fs::path dir = root / kAssetsFolder;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return out;
  }
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec) || !keep(it->path().string())) {
      continue;
    }
    std::error_code rec;
    fs::path rel = fs::relative(it->path(), root, rec);
    if (rec || rel.empty()) {
      continue;
    }
    out.push_back(rel.generic_string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::vector<std::string> list_images(const std::string& project_dir) {
  return list_matching(project_dir, &is_image_path);
}

bool is_audio_path(const std::string& path) { return audio::is_audio_file(path); }

std::vector<std::string> list_sounds(const std::string& project_dir) {
  return list_matching(project_dir, &is_audio_path);
}

bool import_image(const std::string& project_dir, const std::string& source,
                  std::string* rel_out, std::string* error_out) {
  const fs::path src = fs::path(source);
  std::error_code ec;
  if (!fs::is_regular_file(src, ec)) {
    if (error_out) *error_out = "Not a file: " + source;
    return false;
  }
  if (!is_image_path(source)) {
    if (error_out) *error_out = "Not a supported image: " + source;
    return false;
  }
  if (!ensure_assets_dir(project_dir, error_out)) {
    return false;
  }
  const fs::path dir = fs::path(project_dir) / kAssetsFolder;
  const std::string stem = src.stem().string();
  const std::string ext = src.extension().string();
  fs::path dest = dir / src.filename();
  // Re-importing the file already in assets/ is a no-op.
  std::error_code eq_ec;
  if (fs::exists(dest, ec) && fs::equivalent(src, dest, eq_ec)) {
    if (rel_out) {
      *rel_out = std::string(kAssetsFolder) + "/" + dest.filename().string();
    }
    return true;
  }
  for (int n = 2; fs::exists(dest, ec) && n < 10000; ++n) {
    dest = dir / fs::path(stem + "_" + std::to_string(n) + ext);
  }
  fs::copy_file(src, dest, fs::copy_options::none, ec);
  if (ec) {
    if (error_out) {
      *error_out = "Copy failed: " + ec.message();
    }
    return false;
  }
  if (rel_out) {
    *rel_out = std::string(kAssetsFolder) + "/" + dest.filename().string();
  }
  return true;
}

std::string resolve(const std::string& project_dir, const std::string& rel) {
  if (rel.empty()) {
    return {};
  }
  if (rel.rfind("mem:", 0) == 0) {
    return rel;  // core/MemImages key (the UI font atlas)
  }
  const fs::path p = fs::path(rel);
  if (p.is_absolute() || project_dir.empty()) {
    return p.string();
  }
  return (fs::path(project_dir) / p).string();
}

std::string file_name(const std::string& rel) {
  return fs::path(rel).filename().string();
}

std::string file_stem(const std::string& rel) {
  return fs::path(rel).stem().string();
}

}  // namespace assets
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
