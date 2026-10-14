#pragma once

// Retained runtime UI (ts_ui): documents of widgets (panel, label, button,
// image, progress, slider, text input, list) loaded from
// <project>/ui/*.ui.json, laid out with anchors + stack / row / grid,
// themed in the Dusk palette, driven by mouse and keyboard / gamepad
// navigation. Output is UiQuads in screen pixels (font atlas + images),
// which hosts draw directly (2D) or rasterise (3D overlay, VR panel).
//
// Layout is authored against a 1280x720 reference screen and scaled
// uniformly to the target height, so a document reads the same on any
// window and on the VR panel.

#include "ui/Font.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace ui {

enum class WidgetType { Panel, Label, Button, Image, Progress, Slider, TextInput, List };
enum class LayoutMode { Free, Stack, Row, Grid };
enum class Align { Start, Center, End };

const char* widget_type_name(WidgetType t);
bool widget_type_from(const std::string& s, WidgetType* out);

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool contains(float px, float py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};

struct Widget {
  std::string id;
  WidgetType type = WidgetType::Panel;
  // Placement in a Free parent: the anchor point of the parent (0..1) plus
  // offset, minus pivot * size. In stack / row parents only anchor on the
  // cross axis matters (0 start, 0.5 centre, 1 end).
  float anchor[2] = {0, 0};
  float pivot[2] = {0, 0};
  float offset[2] = {0, 0};
  float size[2] = {0, 0};          // 0 = natural size
  bool stretch[2] = {false, false};  // fill the parent's content box
  LayoutMode layout = LayoutMode::Free;  // how children are placed
  float gap = 8;
  float padding = 0;
  int columns = 2;  // grid
  std::string text;
  FontFace font = FontFace::Body;
  float font_size = 18;
  Align align = Align::Start;  // text alignment
  std::string style;   // theme role: "", "title", "muted", "accent", "clear", "dark"
  std::string image;   // image widget: project-relative path or "mem:" key
  float value = 0, min = 0, max = 1, step = 0.05f;  // progress / slider
  std::vector<std::string> items;  // list
  int selected = -1;
  std::string placeholder;  // text input
  int max_length = 64;
  bool visible = true;
  std::vector<Widget> children;

  Rect rect;  // pixels, computed by layout()

  bool focusable() const {
    return type == WidgetType::Button || type == WidgetType::Slider ||
           type == WidgetType::TextInput || type == WidgetType::List;
  }
};

struct Document {
  std::string name;  // file stem ("pause" for ui/pause.ui.json)
  Widget root;
  bool visible = false;  // "visible": shown when loaded
  bool modal = false;    // takes all input; game input should pause
  int layer = 0;         // higher draws on top
  std::string focus;     // widget focused when shown
};

bool parse_document(const std::string& text, Document* out, std::string* error);
std::string write_document(const Document& doc);
bool load_document(const std::string& path, Document* out, std::string* error);

Widget* find_widget(Widget& root, const std::string& id);
const Widget* find_widget(const Widget& root, const std::string& id);

struct Theme {
  float panel[4], panel_dark[4], frame[4], frame_hover[4], frame_active[4];
  float border[4], text[4], text_muted[4], accent[4], accent_hover[4], title[4];
  static Theme dusk();
};

float ui_scale(float screen_w, float screen_h);
// Natural (unscaled, reference-unit) size of a widget.
void natural_size(const Widget& w, const FontAtlas& atlas, float* out_w, float* out_h);
// The draggable track of a slider (pixels; after layout).
Rect slider_track(const Widget& w, float scale);
// Compute every rect (pixels) for a w x h screen.
void layout(Document& doc, float screen_w, float screen_h, const FontAtlas& atlas);

// A quad plus the texture it samples ("" = the font atlas).
struct UiDraw {
  UiQuad quad;
  std::string image;
};

struct DrawState {
  const std::string* focus = nullptr;  // focused widget id (in this doc)
  const std::string* hover = nullptr;
  const std::string* pressed = nullptr;
  bool caret = false;  // text input caret phase
  float scale = 1.0f;
};
// Quads for a laid-out document (empty UiQuad::image = font atlas).
void draw_document(const Document& doc, const DrawState& st, const FontAtlas& atlas,
                   const Theme& theme, std::vector<UiDraw>* out);

struct UiInput {
  float mouse_x = -1.0f, mouse_y = -1.0f;  // pixels; negative = no mouse
  bool mouse_down = false;
  bool up = false, down = false, left = false, right = false;  // edges
  bool accept = false, back = false, backspace = false;        // edges
  std::string text;  // typed characters this frame (ASCII)
  double dt = 0.0;
};

enum class UiEventKind { Click, Change, Submit, Back };
struct UiEvent {
  UiEventKind kind = UiEventKind::Click;
  std::string document;
  std::string widget;
  float value = 0;
  std::string text;
};

class UiSystem {
 public:
  UiSystem();
  void set_atlas(const FontAtlas* atlas) { atlas_ = atlas; }
  const FontAtlas& atlas() const { return *atlas_; }
  Theme theme = Theme::dusk();

  // Every *.ui.json in dir (sorted). Returns documents loaded; parse
  // failures are appended to errors (one line each).
  int load_dir(const std::string& dir, std::string* errors = nullptr);
  void add(Document doc);  // replaces a document of the same name
  void clear();
  bool has(const std::string& name) const;
  Document* document(const std::string& name);
  std::vector<std::string> names() const;

  bool show(const std::string& name);
  bool hide(const std::string& name);
  bool visible(const std::string& name) const;
  // Widget lookup: "doc.widget", or a widget id searched in every document.
  Widget* find(const std::string& id);
  bool set_text(const std::string& id, const std::string& text);
  bool set_value(const std::string& id, float value);
  bool set_visible(const std::string& id, bool visible);

  // Input + layout for a w x h screen; returns what happened.
  std::vector<UiEvent> update(const UiInput& in, float w, float h);
  // Draw the visible documents (layer order) as of the last update.
  void draw(std::vector<UiDraw>* out) const;

  bool modal_open() const;      // a visible modal document
  bool wants_text() const;      // a text input has focus
  bool wants_input() const { return modal_open() || wants_text(); }
  const std::string& focus() const { return focus_; }
  const std::string& focus_document() const { return focus_doc_; }
  void set_focus(const std::string& doc, const std::string& widget);

 private:
  Document* active();
  void ensure_focus(Document& d);
  void move_focus(Document& d, int dx, int dy);
  Widget* hit(Document& d, float x, float y);

  const FontAtlas* atlas_ = nullptr;
  std::vector<Document> docs_;
  std::string focus_doc_, focus_;
  std::string hover_, pressed_, pressed_doc_;
  bool dragging_ = false;
  bool mouse_was_down_ = false;
  double clock_ = 0.0;
  float last_w_ = 1280, last_h_ = 720;
};

// Software rasteriser for UiDraws (atlas + "mem:" images) into an RGBA8
// image with straight alpha over transparent black.
void raster(const std::vector<UiDraw>& draws, const FontAtlas& atlas, int w, int h,
            std::vector<unsigned char>* rgba);

// The built-in pause menu ("pause": Resume / Settings with master, music
// and sfx sliders / Quit). A project ui/pause.ui.json replaces it if it
// keeps the widget ids.
Document builtin_pause_menu();

class PauseMenu {
 public:
  enum class Action { None, Resume, Quit, Volume };
  // Adds the built-in document unless the system already has "pause".
  static void install(UiSystem& ui);
  void open(UiSystem& ui, float master, float music, float sfx);
  void close(UiSystem& ui);
  bool is_open(const UiSystem& ui) const { return ui.visible("pause"); }
  // Feed every event; Volume fills bus ("master" / "music" / "sfx") + value.
  Action handle(UiSystem& ui, const UiEvent& ev, std::string* bus, float* value);
};

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
