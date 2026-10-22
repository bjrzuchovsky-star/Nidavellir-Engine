// VTF texture decoding and VMT parsing (see SourceFormats.h). Written from
// the public VTF / VMT / KeyValues descriptions; no Valve or third-party
// code.
#include "render3d/import/SourceFormats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
namespace source {

namespace {

enum Fmt {
  RGBA8888 = 0, ABGR8888 = 1, RGB888 = 2, BGR888 = 3, RGB565 = 4, I8 = 5, IA88 = 6, P8 = 7,
  A8 = 8, RGB888_BS = 9, BGR888_BS = 10, ARGB8888 = 11, BGRA8888 = 12, DXT1 = 13, DXT3 = 14,
  DXT5 = 15, BGRX8888 = 16, BGR565 = 17, BGRX5551 = 18, BGRA4444 = 19, DXT1_A = 20,
  BGRA5551 = 21, UV88 = 22, UVWQ8888 = 23, RGBA16F = 24, RGBA16 = 25, UVLX8888 = 26
};

std::uint32_t rd32(const unsigned char* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
         (std::uint32_t(p[3]) << 24);
}
std::uint16_t rd16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

int bytes_per_pixel(int f) {
  switch (f) {
    case RGBA8888: case ABGR8888: case ARGB8888: case BGRA8888: case BGRX8888:
    case UVWQ8888: case UVLX8888: return 4;
    case RGB888: case BGR888: case RGB888_BS: case BGR888_BS: return 3;
    case RGB565: case BGR565: case BGRX5551: case BGRA4444: case BGRA5551: case IA88:
    case UV88: return 2;
    case I8: case A8: case P8: return 1;
    case RGBA16F: case RGBA16: return 8;
    default: return 0;
  }
}

bool is_dxt(int f) { return f == DXT1 || f == DXT1_A || f == DXT3 || f == DXT5; }

void rgb565(std::uint16_t c, unsigned char* o) {
  o[0] = static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31);
  o[1] = static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63);
  o[2] = static_cast<unsigned char>((c & 31) * 255 / 31);
}

float half_to_float(std::uint16_t h) {
  const int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31) v = m ? 0.0f : 65504.0f;
  else v = std::ldexp(static_cast<float>(m + 1024), e - 25);
  return s ? -v : v;
}

unsigned char clamp8(float f) {
  return static_cast<unsigned char>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// One 4x4 colour block (8 bytes) into rgba (16 x 4); `dxt1` enables the
// three-colour + transparent mode.
void color_block(const unsigned char* b, bool dxt1, unsigned char* rgba) {
  const std::uint16_t c0 = rd16(b), c1 = rd16(b + 2);
  unsigned char pal[4][4];
  rgb565(c0, pal[0]);
  rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || !dxt1) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = static_cast<unsigned char>((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = static_cast<unsigned char>((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = static_cast<unsigned char>((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  const std::uint32_t bits = rd32(b + 4);
  for (int i = 0; i < 16; ++i) std::memcpy(rgba + i * 4, pal[(bits >> (2 * i)) & 3], 4);
}

void decode_dxt(const unsigned char* src, int f, int w, int h, unsigned char* out) {
  const int bw = (w + 3) / 4, bh = (h + 3) / 4;
  const int block = (f == DXT1 || f == DXT1_A) ? 8 : 16;
  unsigned char px[64];
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      const unsigned char* b = src + (static_cast<std::size_t>(by) * bw + bx) * block;
      if (block == 8) {
        color_block(b, true, px);
      } else {
        color_block(b + 8, false, px);
        if (f == DXT3) {
          for (int i = 0; i < 16; ++i) {
            const int a = (b[i / 2] >> ((i & 1) * 4)) & 15;
            px[i * 4 + 3] = static_cast<unsigned char>(a * 17);
          }
        } else {
          const int a0 = b[0], a1 = b[1];
          int pal[8] = {a0, a1};
          if (a0 > a1) {
            for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
          } else {
            for (int k = 1; k < 5; ++k) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
            pal[6] = 0;
            pal[7] = 255;
          }
          std::uint64_t bits = 0;
          for (int k = 0; k < 6; ++k) bits |= std::uint64_t(b[2 + k]) << (8 * k);
          for (int i = 0; i < 16; ++i) px[i * 4 + 3] = static_cast<unsigned char>(pal[(bits >> (3 * i)) & 7]);
        }
      }
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          const int X = bx * 4 + x, Y = by * 4 + y;
          if (X >= w || Y >= h) continue;
          std::memcpy(out + (static_cast<std::size_t>(Y) * w + X) * 4, px + (y * 4 + x) * 4, 4);
        }
      }
    }
  }
}

void decode_pixel(const unsigned char* p, int f, unsigned char* o) {
  switch (f) {
    case RGBA8888: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;
    case ABGR8888: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;
    case ARGB8888: o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; o[3] = p[0]; break;
    case BGRA8888: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break;
    case BGRX8888: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
    case UVWQ8888: case UVLX8888: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;
    case RGB888: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255; break;
    case BGR888: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
    case RGB888_BS:  // pure blue = transparent
      o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
      o[3] = (p[0] == 0 && p[1] == 0 && p[2] == 255) ? 0 : 255;
      break;
    case BGR888_BS:
      o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
      o[3] = (p[2] == 0 && p[1] == 0 && p[0] == 255) ? 0 : 255;
      break;
    case RGB565: {
      const std::uint16_t c = rd16(p);
      rgb565(c, o);
      o[3] = 255;
      break;
    }
    case BGR565: {
      const std::uint16_t c = rd16(p);
      unsigned char t[3];
      rgb565(c, t);
      o[0] = t[2]; o[1] = t[1]; o[2] = t[0]; o[3] = 255;
      break;
    }
    case BGRX5551: case BGRA5551: {
      const std::uint16_t c = rd16(p);
      o[2] = static_cast<unsigned char>((c & 31) * 255 / 31);
      o[1] = static_cast<unsigned char>(((c >> 5) & 31) * 255 / 31);
      o[0] = static_cast<unsigned char>(((c >> 10) & 31) * 255 / 31);
      o[3] = f == BGRA5551 ? ((c >> 15) ? 255 : 0) : 255;
      break;
    }
    case BGRA4444: {
      const std::uint16_t c = rd16(p);
      o[2] = static_cast<unsigned char>((c & 15) * 17);
      o[1] = static_cast<unsigned char>(((c >> 4) & 15) * 17);
      o[0] = static_cast<unsigned char>(((c >> 8) & 15) * 17);
      o[3] = static_cast<unsigned char>(((c >> 12) & 15) * 17);
      break;
    }
    case I8: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
    case P8: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
    case IA88: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;
    case A8: o[0] = o[1] = o[2] = 255; o[3] = p[0]; break;
    case UV88: o[0] = p[0]; o[1] = p[1]; o[2] = 255; o[3] = 255; break;
    case RGBA16: for (int k = 0; k < 4; ++k) o[k] = static_cast<unsigned char>(rd16(p + 2 * k) >> 8); break;
    case RGBA16F: for (int k = 0; k < 4; ++k) o[k] = clamp8(half_to_float(rd16(p + 2 * k))); break;
    default: o[0] = o[2] = 255; o[1] = 0; o[3] = 255; break;
  }
}

}  // namespace

std::size_t vtf_image_bytes(int f, int w, int h) {
  if (w <= 0 || h <= 0) return 0;
  if (is_dxt(f)) {
    const std::size_t blocks = static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4);
    return blocks * ((f == DXT1 || f == DXT1_A) ? 8 : 16);
  }
  const int bpp = bytes_per_pixel(f);
  return bpp ? static_cast<std::size_t>(w) * h * bpp : 0;
}

bool decode_vtf(const unsigned char* d, std::size_t size, Image* out, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = "VTF: " + why;
    return false;
  };
  if (size < 64 || std::memcmp(d, "VTF\0", 4) != 0) return fail("not a VTF file");
  const std::uint32_t major = rd32(d + 4), minor = rd32(d + 8), header = rd32(d + 12);
  if (major != 7 || minor > 5) return fail("unsupported version " + std::to_string(major) + "." + std::to_string(minor));
  const int w = rd16(d + 16), h = rd16(d + 18);
  const std::uint32_t flags = rd32(d + 20);
  const int frames = std::max<int>(1, rd16(d + 24));
  const int fmt = static_cast<int>(rd32(d + 52));
  const int mips = std::max<int>(1, d[56]);
  const int low_fmt = static_cast<int>(rd32(d + 57));
  const int low_w = d[61], low_h = d[62];
  const int depth = (minor >= 2 && size >= 65) ? std::max<int>(1, rd16(d + 63)) : 1;
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return fail("bad size");
  if (vtf_image_bytes(fmt, 1, 1) == 0) return fail("unsupported image format " + std::to_string(fmt));
  const bool envmap = (flags & 0x4000u) != 0;
  const int faces = envmap ? (minor < 5 ? 7 : 6) : 1;
  std::size_t data = header;
  if (minor >= 3 && size >= 80) {
    const std::uint32_t n = rd32(d + 68);
    data = 0;
    for (std::uint32_t i = 0; i < n && 80 + (i + 1) * 8 <= size; ++i) {
      const unsigned char* r = d + 80 + i * 8;
      if (r[0] == 0x30 && r[1] == 0 && r[2] == 0) data = rd32(r + 4);
    }
    if (!data) return fail("no image resource");
  } else {
    // Low-res thumbnail first.
    if (low_w > 0 && low_h > 0 && low_fmt >= 0) data += vtf_image_bytes(low_fmt, low_w, low_h);
  }
  // Mips are stored smallest first; skip to mip 0.
  for (int m = mips - 1; m >= 1; --m) {
    const int mw = std::max(1, w >> m), mh = std::max(1, h >> m), md = std::max(1, depth >> m);
    data += vtf_image_bytes(fmt, mw, mh) * frames * faces * md;
  }
  const std::size_t need = vtf_image_bytes(fmt, w, h);
  if (data + need > size) return fail("truncated image data");
  const unsigned char* src = d + data;
  out->width = w;
  out->height = h;
  out->rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
  if (is_dxt(fmt)) {
    decode_dxt(src, fmt, w, h, out->rgba.data());
  } else {
    const int bpp = bytes_per_pixel(fmt);
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
      decode_pixel(src + i * bpp, fmt, &out->rgba[i * 4]);
    }
  }
  return true;
}

// ---- VMT (KeyValues) ------------------------------------------------------

namespace {

struct Lexer {
  const std::string& s;
  std::size_t i = 0;
  enum Tok { End, Str, Open, Close };
  std::string text;
  Tok next() {
    for (;;) {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
        while (i < s.size() && s[i] != '\n') ++i;
        continue;
      }
      break;
    }
    if (i >= s.size()) return End;
    const char c = s[i];
    if (c == '{') { ++i; return Open; }
    if (c == '}') { ++i; return Close; }
    text.clear();
    if (c == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) ++i;
        text += s[i++];
      }
      ++i;
      return Str;
    }
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '{' &&
           s[i] != '}' && s[i] != '"') {
      text += s[i++];
    }
    return Str;
  }
};

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Reads a block's key / values after its '{'. Nested blocks (patch
// "insert" / "replace", fallbacks) are flattened; the first value of a key
// wins, so a patch's own values beat its include's.
void read_block(Lexer& lx, Vmt* out, int depth) {
  for (;;) {
    Lexer::Tok t = lx.next();
    if (t == Lexer::End || t == Lexer::Close) return;
    if (t != Lexer::Str) continue;
    const std::string key = lower(lx.text);
    t = lx.next();
    if (t == Lexer::Open) {
      if (depth < 4) read_block(lx, out, depth + 1);
      continue;
    }
    if (t != Lexer::Str) return;
    if (key == "include") out->include = lx.text;
    else if (!key.empty() && key[0] == '$' && depth <= 2 && !out->params.count(key)) out->params[key] = lx.text;
    else if (!key.empty() && key[0] == '%' && !out->params.count(key)) out->params[key] = lx.text;
  }
}

}  // namespace

bool parse_vmt(const std::string& text, Vmt* out, std::string* error) {
  Lexer lx{text};
  *out = Vmt{};
  if (lx.next() != Lexer::Str) {
    if (error) *error = "VMT: no shader name";
    return false;
  }
  out->shader = lower(lx.text);
  if (lx.next() != Lexer::Open) {
    if (error) *error = "VMT: expected '{'";
    return false;
  }
  read_block(lx, out, 1);
  return true;
}

}  // namespace source
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
