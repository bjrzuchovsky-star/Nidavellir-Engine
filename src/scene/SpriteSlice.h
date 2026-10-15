#pragma once

// Sprite sheet slicing for the Sprite Slicer panel: cut a sheet into frame
// rects by grid (cell size, offset, spacing) or by auto-detecting the
// opaque islands, group them into rows and turn picks into an AnimSet
// (.anim.json). Pure (no UI): the editor and the smoke share it.

#include "scene/Animation.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace slicer {

struct GridSpec {
  int frame_w = 32, frame_h = 32;
  int offset_x = 0, offset_y = 0;
  int spacing_x = 0, spacing_y = 0;
  bool skip_empty = true;  // drop fully transparent cells
};

// Row-major cells that fit in the image.
std::vector<AnimRect> grid_rects(int image_w, int image_h, const GridSpec& g);
// Same, minus fully transparent cells when g.skip_empty (needs pixels).
std::vector<AnimRect> grid_rects(const unsigned char* rgba, int image_w, int image_h,
                                 const GridSpec& g);

struct DetectSpec {
  int alpha_threshold = 8;  // alpha above this is "ink"
  int min_size = 2;         // islands smaller than this (w or h) are noise
  int merge_gap = 1;        // islands closer than this merge (e.g. a hat)
};
// Opaque islands (8-connected), merged, sorted into rows (top to bottom,
// then left to right; a row is islands whose vertical centres overlap).
std::vector<AnimRect> detect_rects(const unsigned char* rgba, int image_w, int image_h,
                                   const DetectSpec& d);
// Row index of each rect (same order as rects).
std::vector<int> row_of(const std::vector<AnimRect>& rects);

struct ClipPick {
  std::string name;
  int first = 0;   // index into the rects
  int count = 1;
  float fps = 8;
  AnimMode mode = AnimMode::Loop;
};
// One clip per row ("row0", "row1", ... or the names given).
std::vector<ClipPick> clips_per_row(const std::vector<AnimRect>& rects,
                                    const std::vector<std::string>& names = {});

// The AnimSet: grid clips when the rects are an offset/spacing-free grid
// (compact .anim.json), explicit rect clips otherwise.
AnimSet build_set(const std::string& image_file_name, const std::vector<AnimRect>& rects,
                  const std::vector<ClipPick>& clips, const GridSpec* grid, int image_w, int image_h);

}  // namespace slicer
}  // namespace tombstone
}  // namespace ts
