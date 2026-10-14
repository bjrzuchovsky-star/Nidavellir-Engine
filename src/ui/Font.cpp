#include "ui/Font.h"

#include "core/MemImages.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern const unsigned char ts_font_body_ttf[];
extern const std::size_t ts_font_body_ttf_size;
extern const unsigned char ts_font_display_ttf[];
extern const std::size_t ts_font_display_ttf_size;

namespace ts {
namespace tombstone {
namespace ui {

namespace {
constexpr int kFirst = 32;
constexpr int kCount = 95;  // ' '..'~'
constexpr int kAtlas = 1024;
}  // namespace

const std::vector<float>& FontAtlas::baked_sizes(FontFace face) {
  static const std::vector<float> body = {14, 18, 24, 32};
  static const std::vector<float> display = {20, 28, 40, 56, 80};
  return face == FontFace::Body ? body : display;
}

bool FontAtlas::build() {
  ok_ = false;
  width_ = height_ = kAtlas;
  std::vector<unsigned char> alpha(static_cast<std::size_t>(kAtlas) * kAtlas, 0);
  stbtt_pack_context pc;
  if (!stbtt_PackBegin(&pc, alpha.data(), kAtlas, kAtlas, 0, 1, nullptr)) {
    error_ = "stbtt_PackBegin failed";
    return false;
  }
  // Reserve a small block (filled solid white after packing) for
  // untextured quads: panels, bars, outlines.
  stbrp_rect white{};
  white.w = 6;
  white.h = 6;
  stbtt_PackFontRangesPackRects(&pc, &white, 1);
  for (int f = 0; f < 2; ++f) {
    const FontFace face = static_cast<FontFace>(f);
    const unsigned char* ttf = f == 0 ? ts_font_body_ttf : ts_font_display_ttf;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
      error_ = f == 0 ? "body font failed to load" : "display font failed to load";
      stbtt_PackEnd(&pc);
      return false;
    }
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    faces_[f].clear();
    for (float px : baked_sizes(face)) {
      std::vector<stbtt_packedchar> pcs(kCount);
      if (!stbtt_PackFontRange(&pc, ttf, 0, px, kFirst, kCount, pcs.data())) {
        error_ = "font atlas full";
        stbtt_PackEnd(&pc);
        return false;
      }
      Baked b;
      b.px = px;
      const float scale = stbtt_ScaleForPixelHeight(&info, px);
      b.ascent = asc * scale;
      b.line = (asc - desc + gap) * scale;
      b.glyphs.resize(kCount);
      for (int i = 0; i < kCount; ++i) {
        const stbtt_packedchar& c = pcs[static_cast<std::size_t>(i)];
        Glyph& g = b.glyphs[static_cast<std::size_t>(i)];
        g.ax0 = c.x0;
        g.ay0 = c.y0;
        g.ax1 = c.x1;
        g.ay1 = c.y1;
        g.xoff = c.xoff;
        g.yoff = c.yoff;
        g.xoff2 = c.xoff2;
        g.yoff2 = c.yoff2;
        g.advance = c.xadvance;
      }
      faces_[f].push_back(std::move(b));
    }
  }
  stbtt_PackEnd(&pc);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      alpha[static_cast<std::size_t>(white.y + 1 + y) * kAtlas +
            static_cast<std::size_t>(white.x + 1 + x)] = 255;
    }
  }
  white_u_ = (white.x + 3.0f) / kAtlas;
  white_v_ = (white.y + 3.0f) / kAtlas;
  rgba_.assign(alpha.size() * 4, 255);
  for (std::size_t i = 0; i < alpha.size(); ++i) rgba_[i * 4 + 3] = alpha[i];
  ok_ = true;
  return true;
}

const FontAtlas::Baked& FontAtlas::pick(FontFace face, float px) const {
  const std::vector<Baked>& v = faces_[static_cast<int>(face)];
  // Smallest bake at least as large (downscaling reads cleaner), else largest.
  for (const Baked& b : v) {
    if (b.px >= px - 0.5f) return b;
  }
  return v.back();
}

float FontAtlas::line_height(FontFace face, float px) const {
  if (!ok_) return px * 1.2f;
  const Baked& b = pick(face, px);
  return b.line * (px / b.px);
}

float FontAtlas::ascent(FontFace face, float px) const {
  if (!ok_) return px * 0.8f;
  const Baked& b = pick(face, px);
  return b.ascent * (px / b.px);
}

float FontAtlas::measure(const std::string& text, FontFace face, float px) const {
  if (!ok_) return static_cast<float>(text.size()) * px * 0.5f;
  const Baked& b = pick(face, px);
  const float s = px / b.px;
  float w = 0;
  for (unsigned char ch : text) {
    if (ch == '\n') break;
    const int i = (ch >= kFirst && ch < kFirst + kCount) ? ch - kFirst : '?' - kFirst;
    w += b.glyphs[static_cast<std::size_t>(i)].advance * s;
  }
  return w;
}

std::vector<std::string> FontAtlas::wrap(const std::string& text, FontFace face, float px,
                                         float max_width) const {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = text.find('\n', start);
    const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (max_width <= 0) {
      lines.push_back(para);
    } else {
      std::string line;
      std::size_t i = 0;
      while (i <= para.size()) {
        std::size_t sp = para.find(' ', i);
        if (sp == std::string::npos) sp = para.size();
        const std::string word = para.substr(i, sp - i);
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(trial, face, px) > max_width) {
          lines.push_back(line);
          line = word;
        } else {
          line = trial;
        }
        i = sp + 1;
      }
      lines.push_back(line);
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return lines;
}

void FontAtlas::text_quads(const std::string& text, FontFace face, float px, float x, float y,
                           const float rgba[4], std::vector<UiQuad>* out) const {
  if (!ok_ || !out) return;
  const Baked& b = pick(face, px);
  const float s = px / b.px;
  const float base = y + b.ascent * s;
  const float iw = 1.0f / static_cast<float>(width_);
  const float ih = 1.0f / static_cast<float>(height_);
  float pen = x;
  for (unsigned char ch : text) {
    if (ch == '\n') break;
    const int i = (ch >= kFirst && ch < kFirst + kCount) ? ch - kFirst : '?' - kFirst;
    const Glyph& g = b.glyphs[static_cast<std::size_t>(i)];
    if (g.ax1 > g.ax0 && g.ay1 > g.ay0) {
      UiQuad q;
      // Snap to whole pixels at 1:1 so nearest-filtered text stays crisp.
      const float gx = s == 1.0f ? std::round(pen) : pen;
      q.x0 = gx + g.xoff * s;
      q.y0 = base + g.yoff * s;
      q.x1 = gx + g.xoff2 * s;
      q.y1 = base + g.yoff2 * s;
      if (s == 1.0f) {
        q.y0 = std::round(q.y0);
        q.y1 = q.y0 + (g.ay1 - g.ay0);
        q.x0 = std::round(q.x0);
        q.x1 = q.x0 + (g.ax1 - g.ax0);
      }
      q.u0 = g.ax0 * iw;
      q.v0 = g.ay0 * ih;
      q.u1 = g.ax1 * iw;
      q.v1 = g.ay1 * ih;
      for (int k = 0; k < 4; ++k) q.rgba[k] = rgba[k];
      out->push_back(q);
    }
    pen += g.advance * s;
  }
}

const FontAtlas& default_atlas() {
  static FontAtlas atlas;
  static bool built = false;
  if (!built) {
    built = true;
    if (atlas.build()) {
      mem_images::put(kAtlasKey, atlas.width(), atlas.height(), atlas.rgba());
    }
  }
  return atlas;
}

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
