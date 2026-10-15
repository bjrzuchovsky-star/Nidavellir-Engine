#include "scene/SpriteSlice.h"

#include <algorithm>

namespace ts {
namespace tombstone {
namespace slicer {

std::vector<AnimRect> grid_rects(int iw, int ih, const GridSpec& g) {
  std::vector<AnimRect> out;
  if (g.frame_w <= 0 || g.frame_h <= 0) return out;
  for (int y = g.offset_y; y + g.frame_h <= ih; y += g.frame_h + g.spacing_y) {
    for (int x = g.offset_x; x + g.frame_w <= iw; x += g.frame_w + g.spacing_x) {
      out.push_back({x, y, g.frame_w, g.frame_h});
    }
  }
  return out;
}

std::vector<AnimRect> grid_rects(const unsigned char* rgba, int iw, int ih, const GridSpec& g) {
  std::vector<AnimRect> all = grid_rects(iw, ih, g);
  if (!g.skip_empty || !rgba) return all;
  std::vector<AnimRect> out;
  for (const AnimRect& r : all) {
    bool ink = false;
    for (int y = r.y; y < r.y + r.h && !ink; ++y) {
      for (int x = r.x; x < r.x + r.w; ++x) {
        if (rgba[(static_cast<std::size_t>(y) * iw + x) * 4 + 3] > 0) {
          ink = true;
          break;
        }
      }
    }
    if (ink) out.push_back(r);
  }
  return out;
}

namespace {
bool near(const AnimRect& a, const AnimRect& b, int gap) {
  // Overlapping once each box grows by `gap` px (gap 1 bridges a 1 px gutter).
  return a.x - gap <= b.x + b.w && b.x - gap <= a.x + a.w && a.y - gap <= b.y + b.h && b.y - gap <= a.y + a.h;
}
AnimRect unite(const AnimRect& a, const AnimRect& b) {
  const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
  return {x0, y0, x1 - x0, y1 - y0};
}
}  // namespace

std::vector<int> row_of(const std::vector<AnimRect>& rects) {
  std::vector<int> rows(rects.size(), 0);
  // Rects arrive sorted by top; a new row starts when a rect's centre is
  // below the current row's bottom.
  int row = 0, bottom = -1;
  for (std::size_t i = 0; i < rects.size(); ++i) {
    const int cy = rects[i].y + rects[i].h / 2;
    if (i > 0 && cy >= bottom) ++row;
    if (i == 0 || cy >= bottom) bottom = rects[i].y + rects[i].h;
    else bottom = std::max(bottom, rects[i].y + rects[i].h);
    rows[i] = row;
  }
  return rows;
}

std::vector<AnimRect> detect_rects(const unsigned char* rgba, int iw, int ih, const DetectSpec& d) {
  std::vector<AnimRect> boxes;
  if (!rgba || iw <= 0 || ih <= 0) return boxes;
  std::vector<int> label(static_cast<std::size_t>(iw) * ih, 0);
  std::vector<int> stack;
  auto ink = [&](int x, int y) { return rgba[(static_cast<std::size_t>(y) * iw + x) * 4 + 3] > d.alpha_threshold; };
  int next = 0;
  for (int y = 0; y < ih; ++y) {
    for (int x = 0; x < iw; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * iw + x;
      if (label[i] || !ink(x, y)) continue;
      ++next;
      AnimRect b{x, y, 1, 1};
      int x0 = x, y0 = y, x1 = x, y1 = y;
      stack.assign(1, static_cast<int>(i));
      label[i] = next;
      while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        const int px = p % iw, py = p / iw;
        x0 = std::min(x0, px); x1 = std::max(x1, px);
        y0 = std::min(y0, py); y1 = std::max(y1, py);
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = px + dx, ny = py + dy;
            if (nx < 0 || ny < 0 || nx >= iw || ny >= ih) continue;
            const std::size_t ni = static_cast<std::size_t>(ny) * iw + nx;
            if (!label[ni] && ink(nx, ny)) {
              label[ni] = next;
              stack.push_back(static_cast<int>(ni));
            }
          }
        }
      }
      b = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
      boxes.push_back(b);
    }
  }
  // Merge islands that touch within merge_gap (repeat until stable).
  for (bool merged = true; merged;) {
    merged = false;
    for (std::size_t i = 0; i < boxes.size() && !merged; ++i) {
      for (std::size_t j = i + 1; j < boxes.size(); ++j) {
        if (near(boxes[i], boxes[j], d.merge_gap)) {
          boxes[i] = unite(boxes[i], boxes[j]);
          boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(j));
          merged = true;
          break;
        }
      }
    }
  }
  boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                             [&](const AnimRect& r) { return r.w < d.min_size || r.h < d.min_size; }),
              boxes.end());
  // Rows: by top, then each row left to right.
  std::sort(boxes.begin(), boxes.end(), [](const AnimRect& a, const AnimRect& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });
  const std::vector<int> rows = row_of(boxes);
  std::vector<std::pair<int, AnimRect>> tagged;
  for (std::size_t i = 0; i < boxes.size(); ++i) tagged.push_back({rows[i], boxes[i]});
  std::stable_sort(tagged.begin(), tagged.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second.x < b.second.x;
  });
  std::vector<AnimRect> out;
  for (const auto& t : tagged) out.push_back(t.second);
  return out;
}

std::vector<ClipPick> clips_per_row(const std::vector<AnimRect>& rects, const std::vector<std::string>& names) {
  std::vector<ClipPick> out;
  const std::vector<int> rows = row_of(rects);
  for (std::size_t i = 0; i < rects.size(); ++i) {
    if (out.empty() || rows[i] != rows[i - 1]) {
      ClipPick c;
      const std::size_t r = out.size();
      c.name = r < names.size() && !names[r].empty() ? names[r] : "row" + std::to_string(r);
      c.first = static_cast<int>(i);
      c.count = 0;
      out.push_back(c);
    }
    ++out.back().count;
  }
  return out;
}

AnimSet build_set(const std::string& image_file_name, const std::vector<AnimRect>& rects,
                  const std::vector<ClipPick>& clips, const GridSpec* grid, int iw, int ih) {
  AnimSet set;
  set.image = image_file_name;
  // Grid clips need cells on the sheet's own grid (no offset / spacing);
  // a clip whose frames are not consecutive cells falls back to rects.
  const bool plain_grid = grid && grid->offset_x == 0 && grid->offset_y == 0 && grid->spacing_x == 0 &&
                          grid->spacing_y == 0 && grid->frame_w > 0 && grid->frame_h > 0;
  if (plain_grid) {
    set.grid.frame_w = grid->frame_w;
    set.grid.frame_h = grid->frame_h;
    set.grid.cols = std::max(1, iw / std::max(1, grid->frame_w));
    set.grid.rows = std::max(1, ih / std::max(1, grid->frame_h));
  } else if (!rects.empty()) {
    set.grid.frame_w = rects.front().w;
    set.grid.frame_h = rects.front().h;
  }
  for (const ClipPick& p : clips) {
    if (p.count <= 0 || p.first < 0 || p.first >= static_cast<int>(rects.size())) continue;
    AnimClip c;
    c.name = p.name;
    c.fps = p.fps;
    c.mode = p.mode;
    const int n = std::min(p.count, static_cast<int>(rects.size()) - p.first);
    bool consecutive = plain_grid;
    int start = 0;
    for (int k = 0; consecutive && k < n; ++k) {
      const AnimRect& r = rects[static_cast<std::size_t>(p.first + k)];
      const int cell = (r.y / grid->frame_h) * set.grid.cols + r.x / grid->frame_w;
      if (k == 0) start = cell;
      consecutive = r.w == grid->frame_w && r.h == grid->frame_h && cell == start + k;
    }
    if (consecutive) {
      c.grid = true;
      c.start = start;
      c.count = n;
    } else {
      c.grid = false;
      for (int k = 0; k < n; ++k) c.rects.push_back(rects[static_cast<std::size_t>(p.first + k)]);
    }
    set.clips.push_back(std::move(c));
  }
  if (!set.clips.empty()) set.default_clip = set.clips.front().name;
  set.normalize();
  return set;
}

}  // namespace slicer
}  // namespace tombstone
}  // namespace ts
