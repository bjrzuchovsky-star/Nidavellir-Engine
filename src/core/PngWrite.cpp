#include "core/PngWrite.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace ts {
namespace tombstone {

bool write_png_rgba(const std::string& path, int w, int h, const std::vector<unsigned char>& rgba) {
  if (w <= 0 || h <= 0 || rgba.size() < static_cast<std::size_t>(w) * h * 4) return false;
  auto crc32 = [](const unsigned char* d, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
      c ^= d[i];
      for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
  };
  std::string raw;
  raw.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) * 4 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back('\0');
    raw.append(reinterpret_cast<const char*>(rgba.data()) + static_cast<std::size_t>(y) * w * 4,
               static_cast<std::size_t>(w) * 4);
  }
  std::string z = "\x78\x01";
  std::uint32_t a = 1, b = 0;
  for (unsigned char c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (std::size_t off = 0;;) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
    const bool last = off + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<char>(n & 0xff));
    z.push_back(static_cast<char>(n >> 8));
    z.push_back(static_cast<char>(~n & 0xff));
    z.push_back(static_cast<char>((~n >> 8) & 0xff));
    z.append(raw, off, n);
    off += n;
    if (last) break;
  }
  const std::uint32_t adler = (b << 16) | a;
  for (int i = 3; i >= 0; --i) z.push_back(static_cast<char>((adler >> (i * 8)) & 0xff));
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  auto be = [&](std::uint32_t v) {
    const char c[4] = {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
                       static_cast<char>(v)};
    out.write(c, 4);
  };
  auto chunk = [&](const char* type, const std::string& data) {
    be(static_cast<std::uint32_t>(data.size()));
    const std::string td = std::string(type, 4) + data;
    out.write(td.data(), static_cast<std::streamsize>(td.size()));
    be(crc32(reinterpret_cast<const unsigned char*>(td.data()), td.size()));
  };
  out.write("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  for (std::uint32_t v : {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)}) {
    for (int i = 3; i >= 0; --i) ihdr.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", "");
  return static_cast<bool>(out);
}

}  // namespace tombstone
}  // namespace ts
