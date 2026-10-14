// ts_ui: layout (anchors, stack / row / grid), drawing to UiDraws, and the
// software rasteriser used for the 3D overlay and the VR panel.

#include "ui/Ui.h"

#include "core/MemImages.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace ui {

namespace {

constexpr float kRefH = 720.0f;

float line_h(const Widget& w, const FontAtlas& a) {
  return a.line_height(w.font, w.font_size);
}

struct Size {
  float w = 0, h = 0;
};

Size size_in(const Widget& k, const Rect& c, const FontAtlas& a) {
  Size s;
  natural_size(k, a, &s.w, &s.h);
  if (k.size[0] > 0) s.w = k.size[0];
  else if (k.stretch[0]) s.w = c.w;
  if (k.size[1] > 0) s.h = k.size[1];
  else if (k.stretch[1]) s.h = c.h;
  return s;
}

void arrange(Widget& w, const FontAtlas& a) {
  const float p = w.padding;
  const Rect c{w.rect.x + p, w.rect.y + p, std::max(0.0f, w.rect.w - 2 * p),
               std::max(0.0f, w.rect.h - 2 * p)};
  float cursor = 0;
  int index = 0;
  float row_h = 0;
  const int cols = std::max(1, w.columns);
  const float cell_w = (c.w - w.gap * (cols - 1)) / cols;
  for (Widget& k : w.children) {
    if (!k.visible) continue;
    Size s = size_in(k, c, a);
    switch (w.layout) {
      case LayoutMode::Free:
        k.rect = {c.x + k.anchor[0] * c.w - k.pivot[0] * s.w + k.offset[0],
                  c.y + k.anchor[1] * c.h - k.pivot[1] * s.h + k.offset[1], s.w, s.h};
        break;
      case LayoutMode::Stack:
        k.rect = {c.x + k.anchor[0] * (c.w - s.w) + k.offset[0], c.y + cursor + k.offset[1],
                  s.w, s.h};
        cursor += s.h + w.gap;
        break;
      case LayoutMode::Row:
        k.rect = {c.x + cursor + k.offset[0], c.y + k.anchor[1] * (c.h - s.h) + k.offset[1],
                  s.w, s.h};
        cursor += s.w + w.gap;
        break;
      case LayoutMode::Grid: {
        const int col = index % cols;
        if (col == 0 && index > 0) {
          cursor += row_h + w.gap;
          row_h = 0;
        }
        k.rect = {c.x + col * (cell_w + w.gap), c.y + cursor, cell_w, s.h};
        row_h = std::max(row_h, s.h);
        break;
      }
    }
    ++index;
    arrange(k, a);
  }
}

void scale_rects(Widget& w, float s) {
  w.rect = {w.rect.x * s, w.rect.y * s, w.rect.w * s, w.rect.h * s};
  for (Widget& k : w.children) scale_rects(k, s);
}

void push(std::vector<UiDraw>* out, const FontAtlas& a, float x0, float y0, float x1, float y1,
          const float* c, float alpha_mul = 1.0f) {
  UiDraw d;
  d.quad.x0 = x0;
  d.quad.y0 = y0;
  d.quad.x1 = x1;
  d.quad.y1 = y1;
  d.quad.u0 = d.quad.u1 = a.white_u();
  d.quad.v0 = d.quad.v1 = a.white_v();
  for (int i = 0; i < 4; ++i) d.quad.rgba[i] = c[i];
  d.quad.rgba[3] *= alpha_mul;
  out->push_back(std::move(d));
}

void border(std::vector<UiDraw>* out, const FontAtlas& a, const Rect& r, float t,
            const float* c) {
  push(out, a, r.x, r.y, r.x + r.w, r.y + t, c);
  push(out, a, r.x, r.y + r.h - t, r.x + r.w, r.y + r.h, c);
  push(out, a, r.x, r.y + t, r.x + t, r.y + r.h - t, c);
  push(out, a, r.x + r.w - t, r.y + t, r.x + r.w, r.y + r.h - t, c);
}

void text_line(std::vector<UiDraw>* out, const FontAtlas& a, const std::string& s, FontFace f,
               float px, float x, float y, const float* c) {
  std::vector<UiQuad> qs;
  a.text_quads(s, f, px, x, y, c, &qs);
  for (const UiQuad& q : qs) {
    UiDraw d;
    d.quad = q;
    out->push_back(std::move(d));
  }
}

// Text block inside r (wrapped when the widget has a fixed width),
// vertically centred, aligned per the widget.
void text_block(std::vector<UiDraw>* out, const FontAtlas& a, const Widget& w,
                const std::string& s, const Rect& r, float scale, const float* c,
                Align align) {
  const float px = w.font_size * scale;
  const float lh = a.line_height(w.font, px);
  const std::vector<std::string> lines =
      a.wrap(s, w.font, px, w.size[0] > 0 && w.type == WidgetType::Label ? r.w : 0);
  float y = r.y + (r.h - lh * lines.size()) * 0.5f;
  for (const std::string& line : lines) {
    const float tw = a.measure(line, w.font, px);
    float x = r.x;
    if (align == Align::Center) x = r.x + (r.w - tw) * 0.5f;
    if (align == Align::End) x = r.x + r.w - tw;
    text_line(out, a, line, w.font, px, std::round(x), std::round(y), c);
    y += lh;
  }
}

const float* label_color(const Widget& w, const Theme& t) {
  if (w.style == "title") return t.title;
  if (w.style == "muted") return t.text_muted;
  if (w.style == "accent") return t.accent;
  return t.text;
}

float frac(const Widget& w) {
  const float span = w.max - w.min;
  if (span == 0) return 0;
  return std::clamp((w.value - w.min) / span, 0.0f, 1.0f);
}

}  // namespace

float ui_scale(float, float screen_h) { return std::max(0.25f, screen_h / kRefH); }

Rect slider_track(const Widget& w, float scale) {
  Rect r = w.rect;
  if (!w.text.empty()) {
    const float label = r.w * 0.4f;
    r.x += label;
    r.w -= label;
  }
  const float pad = 8 * scale;
  return {r.x + pad, r.y, std::max(1.0f, r.w - 2 * pad), r.h};
}

void natural_size(const Widget& w, const FontAtlas& a, float* ow, float* oh) {
  const float lh = line_h(w, a);
  float W = 0, H = 0;
  switch (w.type) {
    case WidgetType::Label: {
      const std::vector<std::string> lines =
          a.wrap(w.text, w.font, w.font_size, w.size[0] > 0 ? w.size[0] : 0);
      for (const std::string& l : lines) W = std::max(W, a.measure(l, w.font, w.font_size));
      H = lh * static_cast<float>(lines.size());
      break;
    }
    case WidgetType::Button:
      W = std::max(120.0f, a.measure(w.text, w.font, w.font_size) + 32);
      H = lh + 14;
      break;
    case WidgetType::Image:
      W = H = 64;
      break;
    case WidgetType::Progress:
      W = 220;
      H = w.text.empty() ? 20 : lh + 4;
      break;
    case WidgetType::Slider:
      W = 260;
      H = std::max(28.0f, lh + 6);
      break;
    case WidgetType::TextInput:
      W = 260;
      H = lh + 14;
      break;
    case WidgetType::List: {
      W = 200;
      for (const std::string& s : w.items) W = std::max(W, a.measure(s, w.font, w.font_size) + 24);
      H = std::max<std::size_t>(1, w.items.size()) * (lh + 6) + 8;
      break;
    }
    case WidgetType::Panel: {
      const int cols = std::max(1, w.columns);
      int n = 0;
      float cell_w = 0, row_h = 0;
      for (const Widget& k : w.children) {
        if (!k.visible) continue;
        float kw = 0, kh = 0;
        natural_size(k, a, &kw, &kh);
        if (k.size[0] > 0) kw = k.size[0];
        if (k.size[1] > 0) kh = k.size[1];
        switch (w.layout) {
          case LayoutMode::Stack:
            W = std::max(W, kw);
            H += kh + (n > 0 ? w.gap : 0);
            break;
          case LayoutMode::Row:
            W += kw + (n > 0 ? w.gap : 0);
            H = std::max(H, kh);
            break;
          case LayoutMode::Grid:
            cell_w = std::max(cell_w, kw);
            if (n % cols == 0 && n > 0) {
              H += row_h + w.gap;
              row_h = 0;
            }
            row_h = std::max(row_h, kh);
            break;
          case LayoutMode::Free:
            W = std::max(W, k.offset[0] + kw);
            H = std::max(H, k.offset[1] + kh);
            break;
        }
        ++n;
      }
      if (w.layout == LayoutMode::Grid && n > 0) {
        const int used = std::min(n, cols);
        W = cell_w * used + w.gap * (used - 1);
        H += row_h;
      }
      W += 2 * w.padding;
      H += 2 * w.padding;
      break;
    }
  }
  *ow = W;
  *oh = H;
}

void layout(Document& doc, float screen_w, float screen_h, const FontAtlas& a) {
  const float s = ui_scale(screen_w, screen_h);
  const Rect screen{0, 0, screen_w / s, screen_h / s};
  Widget& r = doc.root;
  const Size sz = size_in(r, screen, a);
  r.rect = {screen.w * r.anchor[0] - r.pivot[0] * sz.w + r.offset[0],
            screen.h * r.anchor[1] - r.pivot[1] * sz.h + r.offset[1], sz.w, sz.h};
  arrange(r, a);
  scale_rects(r, s);
}

namespace {

void draw_widget(const Widget& w, const DrawState& st, const FontAtlas& a, const Theme& t,
                 std::vector<UiDraw>* out) {
  if (!w.visible) return;
  const float s = st.scale;
  const Rect& r = w.rect;
  const bool focus = st.focus && !w.id.empty() && *st.focus == w.id;
  const bool hover = st.hover && !w.id.empty() && *st.hover == w.id;
  const bool pressed = st.pressed && !w.id.empty() && *st.pressed == w.id;
  const float bt = std::max(1.0f, std::round(1.5f * s));
  switch (w.type) {
    case WidgetType::Panel:
      if (w.style != "clear") {
        push(out, a, r.x, r.y, r.x + r.w, r.y + r.h, w.style == "dark" ? t.panel_dark : t.panel);
        border(out, a, r, bt, t.border);
      }
      break;
    case WidgetType::Label:
      text_block(out, a, w, w.text, r, s, label_color(w, t), w.align);
      break;
    case WidgetType::Button: {
      const bool accent = w.style == "accent";
      const float* bg = pressed ? t.frame_active
                        : (hover || focus) ? (accent ? t.accent_hover : t.frame_hover)
                        : (accent ? t.accent : t.frame);
      push(out, a, r.x, r.y, r.x + r.w, r.y + r.h, bg);
      border(out, a, r, focus ? bt * 2 : bt, focus ? t.accent : t.border);
      static const float dark[4] = {0.08f, 0.07f, 0.06f, 1.0f};
      text_block(out, a, w, w.text, r, s, accent && !pressed ? dark : t.text,
                 w.text.empty() ? Align::Center : (w.align == Align::Start ? Align::Center : w.align));
      break;
    }
    case WidgetType::Image: {
      UiDraw d;
      d.quad.x0 = r.x;
      d.quad.y0 = r.y;
      d.quad.x1 = r.x + r.w;
      d.quad.y1 = r.y + r.h;
      d.quad.u1 = d.quad.v1 = 1;
      d.image = w.image;
      if (!w.image.empty()) out->push_back(std::move(d));
      break;
    }
    case WidgetType::Progress:
      push(out, a, r.x, r.y, r.x + r.w, r.y + r.h, t.panel_dark);
      push(out, a, r.x + bt, r.y + bt, r.x + bt + (r.w - 2 * bt) * frac(w), r.y + r.h - bt,
           t.accent);
      border(out, a, r, bt, t.border);
      if (!w.text.empty()) text_block(out, a, w, w.text, r, s, t.text, Align::Center);
      break;
    case WidgetType::Slider: {
      if (!w.text.empty()) {
        Rect lr = r;
        lr.w *= 0.4f;
        text_block(out, a, w, w.text, lr, s, t.text, Align::Start);
      }
      const Rect tr = slider_track(w, s);
      const float cy = tr.y + tr.h * 0.5f;
      const float th = std::max(2.0f, 6 * s);
      push(out, a, tr.x, cy - th * 0.5f, tr.x + tr.w, cy + th * 0.5f, t.frame);
      const float kx = tr.x + tr.w * frac(w);
      push(out, a, tr.x, cy - th * 0.5f, kx, cy + th * 0.5f, t.accent);
      const float kw = 7 * s, kh = std::min(tr.h, 22 * s) * 0.5f;
      push(out, a, kx - kw, cy - kh, kx + kw, cy + kh,
           (focus || hover || pressed) ? t.accent_hover : t.text);
      if (focus) border(out, a, r, bt, t.accent);
      break;
    }
    case WidgetType::TextInput: {
      push(out, a, r.x, r.y, r.x + r.w, r.y + r.h, t.panel_dark);
      border(out, a, r, focus ? bt * 2 : bt, focus ? t.accent : t.border);
      const float px = w.font_size * s;
      const float pad = 8 * s;
      const bool empty = w.text.empty();
      std::string shown = empty ? w.placeholder : w.text;
      while (!empty && shown.size() > 1 && a.measure(shown, w.font, px) > r.w - 2 * pad - 4 * s) {
        shown.erase(0, 1);  // keep the tail (where the caret is) visible
      }
      Rect tr{r.x + pad, r.y, r.w - 2 * pad, r.h};
      text_block(out, a, w, shown, tr, s, empty ? t.text_muted : t.text, Align::Start);
      if (focus && st.caret) {
        const float cx = tr.x + (empty ? 0 : a.measure(shown, w.font, px)) + 1 * s;
        const float lh = a.line_height(w.font, px);
        push(out, a, cx, r.y + (r.h - lh) * 0.5f, cx + std::max(1.0f, 2 * s),
             r.y + (r.h + lh) * 0.5f, t.accent);
      }
      break;
    }
    case WidgetType::List: {
      push(out, a, r.x, r.y, r.x + r.w, r.y + r.h, t.panel_dark);
      border(out, a, r, focus ? bt * 2 : bt, focus ? t.accent : t.border);
      const float px = w.font_size * s;
      const float rh = a.line_height(w.font, px) + 6 * s;
      float y = r.y + 4 * s;
      for (std::size_t i = 0; i < w.items.size() && y + rh <= r.y + r.h + 0.5f; ++i) {
        if (static_cast<int>(i) == w.selected) {
          push(out, a, r.x + 3 * s, y, r.x + r.w - 3 * s, y + rh, focus ? t.frame_active : t.frame);
        }
        text_block(out, a, w, w.items[i], Rect{r.x + 12 * s, y, r.w - 24 * s, rh}, s, t.text,
                   Align::Start);
        y += rh;
      }
      break;
    }
  }
  for (const Widget& k : w.children) draw_widget(k, st, a, t, out);
}

}  // namespace

void draw_document(const Document& doc, const DrawState& st, const FontAtlas& atlas,
                   const Theme& theme, std::vector<UiDraw>* out) {
  if (!out) return;
  draw_widget(doc.root, st, atlas, theme, out);
}

void raster(const std::vector<UiDraw>& draws, const FontAtlas& atlas, int w, int h,
            std::vector<unsigned char>* rgba) {
  rgba->assign(static_cast<std::size_t>(std::max(0, w)) * std::max(0, h) * 4, 0);
  for (const UiDraw& d : draws) {
    const unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    if (d.image.empty()) {
      if (!atlas.ok()) continue;
      tex = atlas.rgba().data();
      tw = atlas.width();
      th = atlas.height();
    } else if (const mem_images::MemImage* m = mem_images::get(d.image)) {
      tex = m->rgba.data();
      tw = m->width;
      th = m->height;
    }
    if (!tex || tw <= 0 || th <= 0) continue;
    const UiQuad& q = d.quad;
    const float qw = q.x1 - q.x0, qh = q.y1 - q.y0;
    if (qw <= 0 || qh <= 0) continue;
    const int x0 = std::max(0, static_cast<int>(std::floor(q.x0)));
    const int y0 = std::max(0, static_cast<int>(std::floor(q.y0)));
    const int x1 = std::min(w, static_cast<int>(std::ceil(q.x1)));
    const int y1 = std::min(h, static_cast<int>(std::ceil(q.y1)));
    for (int y = y0; y < y1; ++y) {
      const float fy = (y + 0.5f - q.y0) / qh;
      if (fy < 0 || fy >= 1) continue;
      const int ty = std::clamp(static_cast<int>((q.v0 + (q.v1 - q.v0) * fy) * th), 0, th - 1);
      for (int x = x0; x < x1; ++x) {
        const float fx = (x + 0.5f - q.x0) / qw;
        if (fx < 0 || fx >= 1) continue;
        const int tx = std::clamp(static_cast<int>((q.u0 + (q.u1 - q.u0) * fx) * tw), 0, tw - 1);
        const unsigned char* t = tex + (static_cast<std::size_t>(ty) * tw + tx) * 4;
        const float sa = t[3] / 255.0f * q.rgba[3];
        if (sa <= 0.0f) continue;
        unsigned char* o = rgba->data() + (static_cast<std::size_t>(y) * w + x) * 4;
        const float da = o[3] / 255.0f;
        const float oa = sa + da * (1 - sa);
        for (int c = 0; c < 3; ++c) {
          const float sc = t[c] / 255.0f * q.rgba[c];
          const float dc = o[c] / 255.0f;
          o[c] = static_cast<unsigned char>(
              std::clamp((sc * sa + dc * da * (1 - sa)) / oa, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        o[3] = static_cast<unsigned char>(std::clamp(oa, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
  }
}

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
