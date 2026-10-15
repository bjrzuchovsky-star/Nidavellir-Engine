#pragma once

// UI fonts: the two bundled OFL faces (Arvo body, Rye display) baked with
// stb_truetype into one RGBA atlas (white + coverage alpha, a solid white
// block at the top-left for untextured quads). Text is measured and laid
// out in pixels; draw output is UiQuads that sample the atlas.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace ui {

enum class FontFace { Body = 0, Display = 1 };

// One textured, tinted screen-space quad (pixels, y down; uv into the
// atlas). The UI's whole draw output.
struct UiQuad {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  float rgba[4] = {1, 1, 1, 1};
};

// The atlas as a core/MemImages key: every host's texture cache knows it.
inline constexpr const char* kAtlasKey = "mem:ui/atlas";

class FontAtlas {
 public:
  // Bake the bundled faces (ASCII at every size). `extra` code points
  // (non-ASCII text of the current language) are baked too, at two sizes
  // per face, from the bundled face when it has the glyph, else from the
  // first of `fallback_fonts` (.ttf / .otf / .ttc files) that does. The
  // atlas grows 1024 -> 2048 -> 4096 as needed. False (with error()) if a
  // bundled font fails to load or the glyphs do not fit.
  bool build(const std::vector<std::uint32_t>& extra = {},
             const std::vector<std::string>& fallback_fonts = {});
  // Extra glyphs baked / requested-but-found-in-no-font.
  std::size_t extra_glyphs() const { return extra_count_; }
  const std::vector<std::uint32_t>& missing_glyphs() const { return missing_; }
  bool has_glyph(std::uint32_t cp) const;
  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

  int width() const { return width_; }
  int height() const { return height_; }
  const std::vector<unsigned char>& rgba() const { return rgba_; }
  // UV of a fully opaque white texel (solid fills).
  float white_u() const { return white_u_; }
  float white_v() const { return white_v_; }

  float line_height(FontFace face, float px) const;
  float ascent(FontFace face, float px) const;
  // Width of one line (stops at '\n').
  float measure(const std::string& text, FontFace face, float px) const;
  // Greedy word wrap to max_width (0 = only explicit newlines).
  std::vector<std::string> wrap(const std::string& text, FontFace face, float px,
                                float max_width) const;
  // Quads for one line; (x, y) is the top-left of the line box.
  void text_quads(const std::string& text, FontFace face, float px, float x, float y,
                  const float rgba[4], std::vector<UiQuad>* out) const;

  // Sizes baked per face (pixels).
  static const std::vector<float>& baked_sizes(FontFace face);

 private:
  struct Glyph {
    float ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;  // atlas texels
    float xoff = 0, yoff = 0, xoff2 = 0, yoff2 = 0, advance = 0;
  };
  struct Baked {
    float px = 0;
    float ascent = 0;
    float line = 0;
    std::vector<Glyph> glyphs;  // chars 32..126
  };
  struct ExtraBake {
    float px = 0;
    std::unordered_map<std::uint32_t, Glyph> glyphs;
  };
  const Baked& pick(FontFace face, float px) const;
  // Glyph for code point cp at px (ASCII bakes, then the extra bakes, then
  // '?'); *scale = px / baked px.
  const Glyph& glyph(FontFace face, float px, std::uint32_t cp, float* scale) const;
  bool build_at(int size, const std::vector<std::uint32_t>& extra,
                const std::vector<std::string>& fallback_fonts, bool* full);
  std::vector<ExtraBake> extras_[2];
  std::size_t extra_count_ = 0;
  std::vector<std::uint32_t> missing_;

  bool ok_ = false;
  std::string error_;
  int width_ = 0;
  int height_ = 0;
  std::vector<unsigned char> rgba_;
  float white_u_ = 0, white_v_ = 0;
  std::vector<Baked> faces_[2];
};

// Process-wide atlas, baked on first use and published as kAtlasKey.
// Rebaked (same object, new kAtlasKey version) after request_glyphs()
// changes what it must hold.
const FontAtlas& default_atlas();
// Ask the default atlas for every non-ASCII character of `text`, using
// `fallback_fonts` for glyphs the bundled faces lack (localisation calls
// this when the language changes). Cheap when nothing changed.
void request_glyphs(const std::string& text, const std::vector<std::string>& fallback_fonts);

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
