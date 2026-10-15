#pragma once

// Sprite sheet slicer, Dusk theme: pick a sheet under assets/, cut it by a
// grid or by auto-detecting sprite islands, group the frames into clips
// (one per row to start with), preview them playing and save
// <sheet>.anim.json next to the image for AnimatedSprite.

#include "editor/assets/TextureCache.h"
#include "scene/SpriteSlice.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class SpriteSlicerPanel {
 public:
  static SpriteSlicerPanel& get();
  void open(const std::string& project_dir, const std::string& image_rel = "");
  bool is_open() const { return open_; }
  void draw();
  void pin(float x, float y, float w, float h) {
    pinned_ = true;
    pin_[0] = x; pin_[1] = y; pin_[2] = w; pin_[3] = h;
  }

  // Scriptable steps (also used by the screenshot / smoke).
  bool load_image(const std::string& image_rel);
  void set_auto_detect(bool on) { detect_mode_ = on; reslice(); }
  void reslice();
  const std::vector<AnimRect>& rects() const { return rects_; }
  std::vector<slicer::ClipPick>& clips() { return clips_; }
  slicer::GridSpec& grid() { return grid_; }  // call reslice() after editing
  AnimSet build() const;
  bool save(std::string* error);  // returns the written path in status
  std::string anim_path() const;   // project-relative .anim.json

 private:
  void draw_sheet();
  void draw_clips();

  bool open_ = false;
  bool pinned_ = false;
  float pin_[4] = {};
  std::string project_dir_;
  std::vector<std::string> images_;
  std::string image_rel_;
  std::vector<unsigned char> rgba_;
  int w_ = 0, h_ = 0;
  std::string error_;
  bool detect_mode_ = false;
  slicer::GridSpec grid_;
  slicer::DetectSpec detect_;
  std::vector<AnimRect> rects_;
  std::vector<slicer::ClipPick> clips_;
  int selected_clip_ = 0;
  float zoom_ = 2.0f;
  std::string status_;
  TextureCache textures_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
