#include "core/MemImages.h"

#include <map>
#include <utility>

namespace ts {
namespace tombstone {
namespace mem_images {

namespace {
std::map<std::string, MemImage>& store() {
  static std::map<std::string, MemImage> s;
  return s;
}
std::uint64_t g_version = 0;
}  // namespace

std::uint64_t put(const std::string& key, int width, int height,
                  std::vector<unsigned char> rgba) {
  MemImage& m = store()[key];
  m.width = width;
  m.height = height;
  m.rgba = std::move(rgba);
  m.version = ++g_version;
  return m.version;
}

const MemImage* get(const std::string& key) {
  auto it = store().find(key);
  return it == store().end() ? nullptr : &it->second;
}

void remove(const std::string& key) { store().erase(key); }

}  // namespace mem_images
}  // namespace tombstone
}  // namespace ts
