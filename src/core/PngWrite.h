#pragma once

// Minimal PNG writer (RGBA8, stored deflate: no compression library). For
// generated assets (autotile templates) and screenshots.
#include <string>
#include <vector>

namespace ts {
namespace tombstone {

bool write_png_rgba(const std::string& path, int w, int h, const std::vector<unsigned char>& rgba);

}  // namespace tombstone
}  // namespace ts
