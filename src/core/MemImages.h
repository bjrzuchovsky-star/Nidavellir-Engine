#pragma once

// In-memory RGBA images addressed by "mem:" keys (the UI font atlas, UI
// rasters for the 3D overlay and the VR panel). Texture caches in every
// host look these up instead of the filesystem, and re-upload when the
// version changes. Main-thread only.

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace mem_images {

struct MemImage {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;  // RGBA8, top row first
  std::uint64_t version = 0;        // bumps on every put()
};

inline bool is_key(const std::string& key) { return key.rfind("mem:", 0) == 0; }

// Store (or replace) an image; returns its new version.
std::uint64_t put(const std::string& key, int width, int height,
                  std::vector<unsigned char> rgba);
const MemImage* get(const std::string& key);
void remove(const std::string& key);

}  // namespace mem_images
}  // namespace tombstone
}  // namespace ts
