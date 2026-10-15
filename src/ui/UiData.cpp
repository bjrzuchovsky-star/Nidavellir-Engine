// ts_ui: document JSON (read / write round trip) and the Dusk theme.

#include "ui/Ui.h"

#include "scene/JsonTree.h"

#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace ui {

namespace {

const char* kTypes[] = {"panel", "label", "button", "image",
                        "progress", "slider", "input", "list"};

const char* layout_name(LayoutMode m) {
  switch (m) {
    case LayoutMode::Stack: return "stack";
    case LayoutMode::Row: return "row";
    case LayoutMode::Grid: return "grid";
    default: return "free";
  }
}

LayoutMode layout_from(const std::string& s) {
  if (s == "stack") return LayoutMode::Stack;
  if (s == "row") return LayoutMode::Row;
  if (s == "grid") return LayoutMode::Grid;
  return LayoutMode::Free;
}

const char* align_name(Align a) {
  return a == Align::Center ? "center" : a == Align::End ? "end" : "start";
}

Align align_from(const std::string& s) {
  if (s == "center") return Align::Center;
  if (s == "end" || s == "right") return Align::End;
  return Align::Start;
}

void read_pair(const json::Value& v, const char* key, float* out) {
  float tmp[2];
  if (v.floats(key, tmp, 2)) {
    out[0] = tmp[0];
    out[1] = tmp[1];
  } else if (v[key].is_number()) {
    out[0] = out[1] = v[key].as_float();
  }
}

bool read_widget(const json::Value& v, Widget* w, std::string* error, int depth) {
  if (!v.is_object()) {
    *error = "widget must be an object";
    return false;
  }
  if (depth > 32) {
    *error = "widgets nested too deep";
    return false;
  }
  w->id = v.str("id");
  const std::string type = v.str("type", "panel");
  if (!widget_type_from(type, &w->type)) {
    *error = "unknown widget type '" + type + "'";
    return false;
  }
  read_pair(v, "anchor", w->anchor);
  read_pair(v, "pivot", w->pivot);
  read_pair(v, "offset", w->offset);
  read_pair(v, "size", w->size);
  if (v["stretch"].is_bool()) {
    w->stretch[0] = w->stretch[1] = v["stretch"].as_bool();
  } else if (v["stretch"].is_array() && v["stretch"].size() == 2) {
    w->stretch[0] = v["stretch"][0].as_bool();
    w->stretch[1] = v["stretch"][1].as_bool();
  }
  w->layout = layout_from(v.str("layout", "free"));
  w->gap = v.flt("gap", w->gap);
  w->padding = v.flt("padding", w->padding);
  w->columns = static_cast<int>(v.num("columns", w->columns));
  if (w->columns < 1) w->columns = 1;
  w->text = v.str("text");
  w->text_key = v.str("text_key");
  w->text_fallback = w->text;
  w->font = v.str("font", "body") == "display" ? FontFace::Display : FontFace::Body;
  w->font_size = v.flt("font_size", w->font_size);
  w->align = align_from(v.str("align", "start"));
  w->style = v.str("style");
  w->image = v.str("image");
  w->value = v.flt("value", w->value);
  w->min = v.flt("min", w->min);
  w->max = v.flt("max", w->max);
  w->step = v.flt("step", w->step);
  for (const json::Value& it : v["items"].items()) w->items.push_back(it.as_string_or(""));
  w->selected = static_cast<int>(v.num("selected", w->selected));
  w->placeholder = v.str("placeholder");
  w->max_length = static_cast<int>(v.num("max_length", w->max_length));
  w->visible = v.flag("visible", true);
  for (const json::Value& c : v["children"].items()) {
    Widget child;
    if (!read_widget(c, &child, error, depth + 1)) return false;
    w->children.push_back(std::move(child));
  }
  return true;
}

void put_pair(json::Value& o, const char* key, const float* v, float def) {
  if (v[0] != def || v[1] != def) o.set(key, json::floats(v, 2));
}

json::Value write_widget(const Widget& w) {
  const Widget d;  // defaults: only differences are written
  json::Value o = json::Value::object();
  if (!w.id.empty()) o.set("id", json::Value::string(w.id));
  o.set("type", json::Value::string(widget_type_name(w.type)));
  put_pair(o, "anchor", w.anchor, 0);
  put_pair(o, "pivot", w.pivot, 0);
  put_pair(o, "offset", w.offset, 0);
  put_pair(o, "size", w.size, 0);
  if (w.stretch[0] || w.stretch[1]) {
    json::Value a = json::Value::array();
    a.push(json::Value::boolean(w.stretch[0]));
    a.push(json::Value::boolean(w.stretch[1]));
    o.set("stretch", a);
  }
  if (w.layout != d.layout) o.set("layout", json::Value::string(layout_name(w.layout)));
  if (w.gap != d.gap) o.set("gap", json::Value::number(w.gap));
  if (w.padding != d.padding) o.set("padding", json::Value::number(w.padding));
  if (w.columns != d.columns) o.set("columns", json::Value::number(w.columns));
  if (!w.text_key.empty()) {
    o.set("text_key", json::Value::string(w.text_key));
    if (!w.text_fallback.empty()) o.set("text", json::Value::string(w.text_fallback));
  } else if (!w.text.empty()) {
    o.set("text", json::Value::string(w.text));
  }
  if (w.font != d.font) o.set("font", json::Value::string("display"));
  if (w.font_size != d.font_size) o.set("font_size", json::Value::number(w.font_size));
  if (w.align != d.align) o.set("align", json::Value::string(align_name(w.align)));
  if (!w.style.empty()) o.set("style", json::Value::string(w.style));
  if (!w.image.empty()) o.set("image", json::Value::string(w.image));
  if (w.value != d.value) o.set("value", json::Value::number(w.value));
  if (w.min != d.min) o.set("min", json::Value::number(w.min));
  if (w.max != d.max) o.set("max", json::Value::number(w.max));
  if (w.step != d.step) o.set("step", json::Value::number(w.step));
  if (!w.items.empty()) {
    json::Value a = json::Value::array();
    for (const std::string& s : w.items) a.push(json::Value::string(s));
    o.set("items", a);
  }
  if (w.selected != d.selected) o.set("selected", json::Value::number(w.selected));
  if (!w.placeholder.empty()) o.set("placeholder", json::Value::string(w.placeholder));
  if (w.max_length != d.max_length) o.set("max_length", json::Value::number(w.max_length));
  if (!w.visible) o.set("visible", json::Value::boolean(false));
  if (!w.children.empty()) {
    json::Value a = json::Value::array();
    for (const Widget& c : w.children) a.push(write_widget(c));
    o.set("children", a);
  }
  return o;
}

}  // namespace

const char* widget_type_name(WidgetType t) { return kTypes[static_cast<int>(t)]; }

bool widget_type_from(const std::string& s, WidgetType* out) {
  for (int i = 0; i < 8; ++i) {
    if (s == kTypes[i]) {
      *out = static_cast<WidgetType>(i);
      return true;
    }
  }
  if (s == "text_input" || s == "textinput") {
    *out = WidgetType::TextInput;
    return true;
  }
  return false;
}

bool parse_document(const std::string& text, Document* out, std::string* error) {
  std::string err;
  json::Value v;
  if (!json::parse(text, &v, &err)) {
    if (error) *error = err;
    return false;
  }
  if (!v.is_object()) {
    if (error) *error = "document must be an object";
    return false;
  }
  Document d;
  d.name = v.str("name");
  d.visible = v.flag("visible", false);
  d.modal = v.flag("modal", false);
  d.layer = static_cast<int>(v.num("layer", 0));
  d.focus = v.str("focus");
  if (!read_widget(v["root"], &d.root, &err, 0)) {
    if (error) *error = err;
    return false;
  }
  *out = std::move(d);
  return true;
}

std::string write_document(const Document& doc) {
  json::Value o = json::Value::object();
  o.set("format", json::Value::string("tombstone.ui"));
  o.set("version", json::Value::number(1));
  if (!doc.name.empty()) o.set("name", json::Value::string(doc.name));
  if (doc.visible) o.set("visible", json::Value::boolean(true));
  if (doc.modal) o.set("modal", json::Value::boolean(true));
  if (doc.layer != 0) o.set("layer", json::Value::number(doc.layer));
  if (!doc.focus.empty()) o.set("focus", json::Value::string(doc.focus));
  o.set("root", write_widget(doc.root));
  return json::write(o);
}

bool load_document(const std::string& path, Document* out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_document(ss.str(), out, error);
}

Widget* find_widget(Widget& root, const std::string& id) {
  if (root.id == id) return &root;
  for (Widget& c : root.children) {
    if (Widget* w = find_widget(c, id)) return w;
  }
  return nullptr;
}

const Widget* find_widget(const Widget& root, const std::string& id) {
  return find_widget(const_cast<Widget&>(root), id);
}

namespace {
void rgb(float* o, int r, int g, int b, float a = 1.0f) {
  o[0] = r / 255.0f;
  o[1] = g / 255.0f;
  o[2] = b / 255.0f;
  o[3] = a;
}
}  // namespace

Theme Theme::dusk() {
  // The editor's Dusk palette: charcoal + warm stone, bone text, amber.
  Theme t{};
  rgb(t.panel, 28, 25, 23, 0.92f);
  rgb(t.panel_dark, 14, 13, 12, 0.85f);
  rgb(t.frame, 52, 45, 39);
  rgb(t.frame_hover, 88, 61, 42);
  rgb(t.frame_active, 140, 76, 44);
  rgb(t.border, 74, 62, 50);
  rgb(t.text, 234, 224, 204);
  rgb(t.text_muted, 148, 136, 116);
  rgb(t.accent, 222, 160, 52);
  rgb(t.accent_hover, 240, 186, 88);
  rgb(t.title, 240, 186, 88);
  return t;
}

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
