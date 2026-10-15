#include "ui/Font.h"

#include "core/Localization.h"
#include "core/MemImages.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <set>
#include <sstream>

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
constexpr int kMaxExtra = 6000;

std::string read_file(const std::string& p) {
  std::ifstream in(std::filesystem::path(p), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const std::vector<float>& extra_sizes(FontFace face) {
  static const std::vector<float> body = {18, 32};
  static const std::vector<float> display = {28, 56};
  return face == FontFace::Body ? body : display;
}
}  // namespace

const std::vector<float>& FontAtlas::baked_sizes(FontFace face) {
  static const std::vector<float> body = {14, 18, 24, 32};
  static const std::vector<float> display = {20, 28, 40, 56, 80};
  return face == FontFace::Body ? body : display;
}

bool FontAtlas::build(const std::vector<std::uint32_t>& extra,
                      const std::vector<std::string>& fallback_fonts) {
  for (int size : {1024, 2048, 4096}) {
    bool full = false;
    if (build_at(size, extra, fallback_fonts, &full)) return true;
    if (!full) return false;
  }
  return false;
}

bool FontAtlas::has_glyph(std::uint32_t cp) const {
  if (cp >= kFirst && cp < kFirst + kCount) return true;
  for (const auto& v : extras_) {
    for (const ExtraBake& e : v) {
      if (e.glyphs.count(cp)) return true;
    }
  }
  return false;
}

bool FontAtlas::build_at(int kAtlas, const std::vector<std::uint32_t>& extra_in,
                         const std::vector<std::string>& fallback_files, bool* full) {
  ok_ = false;
  *full = false;
  width_ = height_ = kAtlas;
  extra_count_ = 0;
  missing_.clear();
  std::vector<std::uint32_t> extra;
  for (std::uint32_t cp : std::set<std::uint32_t>(extra_in.begin(), extra_in.end())) {
    if (cp >= 128 && cp != 0xFFFD && extra.size() < kMaxExtra) extra.push_back(cp);
  }
  std::vector<std::string> fallback_data;
  std::vector<stbtt_fontinfo> fallback_info;
  if (!extra.empty()) {
    for (const std::string& f : fallback_files) {
      std::string d = read_file(f);
      if (d.size() < 12) continue;
      fallback_data.push_back(std::move(d));
    }
    fallback_info.resize(fallback_data.size());
    for (std::size_t i = 0; i < fallback_data.size(); ++i) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(fallback_data[i].data());
      if (!stbtt_InitFont(&fallback_info[i], p, stbtt_GetFontOffsetForIndex(p, 0))) {
        fallback_info[i] = stbtt_fontinfo{};
      }
    }
  }
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
        *full = true;
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
    // Extra (non-ASCII) glyphs: bundled face first, then the fallbacks.
    extras_[f].clear();
    if (!extra.empty()) {
      std::vector<std::vector<int>> groups(1 + fallback_info.size());
      for (std::uint32_t cp : extra) {
        if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0) {
          groups[0].push_back(static_cast<int>(cp));
          continue;
        }
        bool found = false;
        for (std::size_t k = 0; k < fallback_info.size() && !found; ++k) {
          if (fallback_info[k].data && stbtt_FindGlyphIndex(&fallback_info[k], static_cast<int>(cp)) != 0) {
            groups[k + 1].push_back(static_cast<int>(cp));
            found = true;
          }
        }
        if (!found && f == 0) missing_.push_back(cp);
      }
      for (float px : extra_sizes(face)) {
        ExtraBake eb;
        eb.px = px;
        for (std::size_t g = 0; g < groups.size(); ++g) {
          if (groups[g].empty()) continue;
          const unsigned char* data =
              g == 0 ? ttf : reinterpret_cast<const unsigned char*>(fallback_data[g - 1].data());
          std::vector<stbtt_packedchar> pcs(groups[g].size());
          stbtt_pack_range r{};
          r.font_size = px;
          r.array_of_unicode_codepoints = groups[g].data();
          r.num_chars = static_cast<int>(groups[g].size());
          r.chardata_for_range = pcs.data();
          if (!stbtt_PackFontRanges(&pc, data, 0, &r, 1)) {
            error_ = "font atlas full";
            *full = true;
            stbtt_PackEnd(&pc);
            return false;
          }
          for (std::size_t i = 0; i < pcs.size(); ++i) {
            const stbtt_packedchar& c = pcs[i];
            Glyph gl;
            gl.ax0 = c.x0;
            gl.ay0 = c.y0;
            gl.ax1 = c.x1;
            gl.ay1 = c.y1;
            gl.xoff = c.xoff;
            gl.yoff = c.yoff;
            gl.xoff2 = c.xoff2;
            gl.yoff2 = c.yoff2;
            gl.advance = c.xadvance;
            eb.glyphs[static_cast<std::uint32_t>(groups[g][i])] = gl;
          }
        }
        extra_count_ += eb.glyphs.size();
        extras_[f].push_back(std::move(eb));
      }
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

const FontAtlas::Glyph& FontAtlas::glyph(FontFace face, float px, std::uint32_t cp, float* scale) const {
  if (cp >= kFirst && cp < kFirst + kCount) {
    const Baked& b = pick(face, px);
    *scale = px / b.px;
    return b.glyphs[cp - kFirst];
  }
  const std::vector<ExtraBake>& v = extras_[static_cast<int>(face)];
  const ExtraBake* best = nullptr;
  for (const ExtraBake& e : v) {
    if (e.glyphs.count(cp) && (!best || (best->px < px - 0.5f && e.px > best->px))) best = &e;
    if (best && best->px >= px - 0.5f) break;
  }
  if (best) {
    *scale = px / best->px;
    return best->glyphs.at(cp);
  }
  const Baked& b = pick(face, px);
  *scale = px / b.px;
  return b.glyphs['?' - kFirst];
}

float FontAtlas::measure(const std::string& text, FontFace face, float px) const {
  if (!ok_) return static_cast<float>(text.size()) * px * 0.5f;
  float w = 0;
  for (std::uint32_t cp : loc::utf8_decode(text)) {
    if (cp == '\n') break;
    float s = 1;
    const Glyph& g = glyph(face, px, cp, &s);
    w += g.advance * s;
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
        std::string word = para.substr(i, sp - i);
        // No spaces to break at (CJK, long words): break between characters.
        while (measure(word, face, px) > max_width && word.size() > 1) {
          if (!line.empty()) {
            lines.push_back(line);
            line.clear();
          }
          std::size_t cut = 0, next = 0;
          while (next < word.size()) {
            std::size_t n = next + 1;
            while (n < word.size() && (static_cast<unsigned char>(word[n]) & 0xC0) == 0x80) ++n;
            if (cut > 0 && measure(word.substr(0, n), face, px) > max_width) break;
            cut = next = n;
          }
          if (cut == 0 || cut >= word.size()) break;
          lines.push_back(word.substr(0, cut));
          word = word.substr(cut);
        }
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
  const float base = y + b.ascent * (px / b.px);
  const float iw = 1.0f / static_cast<float>(width_);
  const float ih = 1.0f / static_cast<float>(height_);
  float pen = x;
  for (std::uint32_t cp : loc::utf8_decode(text)) {
    if (cp == '\n') break;
    float s = 1;
    const Glyph& g = glyph(face, px, cp, &s);
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

namespace {
struct GlyphRequest {
  std::vector<std::uint32_t> cps;
  std::vector<std::string> fonts;
  bool dirty = false;
};
GlyphRequest& glyph_request() {
  static GlyphRequest r;
  return r;
}
}  // namespace

void request_glyphs(const std::string& text, const std::vector<std::string>& fallback_fonts) {
  std::set<std::uint32_t> set;
  for (std::uint32_t cp : loc::utf8_decode(text)) {
    if (cp >= 128) set.insert(cp);
  }
  std::vector<std::uint32_t> cps(set.begin(), set.end());
  GlyphRequest& r = glyph_request();
  if (cps == r.cps && fallback_fonts == r.fonts) return;
  r.cps = std::move(cps);
  r.fonts = fallback_fonts;
  r.dirty = true;
}

const FontAtlas& default_atlas() {
  static FontAtlas atlas;
  static bool built = false;
  GlyphRequest& r = glyph_request();
  if (!built || r.dirty) {
    built = true;
    r.dirty = false;
    if (atlas.build(r.cps, r.fonts) || (!r.cps.empty() && atlas.build())) {
      mem_images::put(kAtlasKey, atlas.width(), atlas.height(), atlas.rgba());
    }
    if (!atlas.missing_glyphs().empty()) {
      std::fprintf(stderr, "[ui] missing glyphs: %zu character(s) (first U+%04X) in no bundled or fallback font; "
                           "add one to <project>/fonts/\n",
                   atlas.missing_glyphs().size(), static_cast<unsigned>(atlas.missing_glyphs().front()));
    }
  }
  return atlas;
}

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
