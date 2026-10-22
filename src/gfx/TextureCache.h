#pragma once

// Image decode (stb_image) + texture cache shared by the editor and
// ts_game. Renderer-agnostic: uploads go through a TextureUploader.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace gfx {

// Renderer-agnostic texture handle. handle is whatever the uploader returned
// (a GL texture name in ts_admin); 0 when no uploader is registered, e.g.
// --smoke runs headless. width/height are known either way.
struct TextureInfo {
  std::uint64_t handle = 0;
  int width = 0;
  int height = 0;
  bool ok = false;     // file decoded
  std::string error;   // why not ok
};

// Implemented per renderer (gfx::GlTextureUploader for ts_admin and
// ts_game). The cache itself never talks to a graphics API.
class TextureUploader {
 public:
  virtual ~TextureUploader() = default;
  // RGBA8, tightly packed, top row first. Returns 0 on failure.
  virtual std::uint64_t upload_rgba(const unsigned char* pixels, int width,
                                    int height) = 0;
  virtual void destroy(std::uint64_t handle) = 0;
};

// Process-wide uploader. Register after the GL context exists; set nullptr
// before the context goes away (live caches then simply drop their handles).
void set_texture_uploader(TextureUploader* uploader);
TextureUploader* texture_uploader();

// Extra in-memory decoder tried before stb_image (the editor registers the
// Source .vtf decoder so .vtf thumbnails work). Returns false when the
// bytes are not its format.
using ExtraImageDecoder = bool (*)(const unsigned char* data, std::size_t size,
                                   std::vector<unsigned char>* rgba, int* width, int* height);
void set_extra_image_decoder(ExtraImageDecoder decoder);

// Decodes images with stb_image (png/jpg/bmp/tga/gif/psd/pnm) and caches
// them by path, including failures, so a missing file costs one stat per
// poll instead of one decode per frame.
class TextureCache {
 public:
  TextureCache() = default;
  ~TextureCache();
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Load on first use; later calls return the cached entry. "mem:" keys
  // come from core/MemImages and re-upload when their version changes.
  const TextureInfo& get(const std::string& path);
  // Re-decode files whose mtime changed (or that appeared / vanished).
  // Throttled to once per `interval` seconds of `now`. Returns reloads.
  std::size_t poll_changes(double now, double interval = 1.0);
  void invalidate(const std::string& path);
  void clear();
  std::size_t size() const { return entries_.size(); }

  // Headless helper: decode a file to RGBA8.
  static bool decode_file(const std::string& path,
                          std::vector<unsigned char>* rgba, int* width,
                          int* height, std::string* error);

 private:
  struct Entry {
    TextureInfo info;
    std::filesystem::file_time_type mtime{};
    bool had_file = false;
    TextureUploader* owner = nullptr;
    std::uint64_t mem_version = 0;  // "mem:" keys (core/MemImages)
  };
  void load_into(const std::string& path, Entry& entry);
  void release(Entry& entry);

  std::unordered_map<std::string, Entry> entries_;
  double last_poll_ = -1.0e9;
};

}  // namespace gfx
}  // namespace tombstone
}  // namespace ts
