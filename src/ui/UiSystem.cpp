// ts_ui: the retained UI system (documents, input, navigation) and the
// built-in pause menu.

#include "ui/Ui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace ui {

namespace fs = std::filesystem;

namespace {

void collect_focusable(Widget& w, std::vector<Widget*>* out) {
  if (!w.visible) return;
  if (w.focusable() && !w.id.empty()) out->push_back(&w);
  for (Widget& k : w.children) collect_focusable(k, out);
}

Widget* hit_widget(Widget& w, float x, float y) {
  if (!w.visible) return nullptr;
  // Children first, last drawn on top.
  for (auto it = w.children.rbegin(); it != w.children.rend(); ++it) {
    if (Widget* h = hit_widget(*it, x, y)) return h;
  }
  if (w.focusable() && !w.id.empty() && w.rect.contains(x, y)) return &w;
  return nullptr;
}

float snap(const Widget& w, float v) {
  v = std::clamp(v, std::min(w.min, w.max), std::max(w.min, w.max));
  if (w.step > 0) v = w.min + std::round((v - w.min) / w.step) * w.step;
  return std::clamp(v, std::min(w.min, w.max), std::max(w.min, w.max));
}

std::vector<Document*> draw_order(std::vector<Document>& docs) {
  std::vector<Document*> v;
  for (Document& d : docs) {
    if (d.visible) v.push_back(&d);
  }
  std::stable_sort(v.begin(), v.end(),
                   [](const Document* a, const Document* b) { return a->layer < b->layer; });
  return v;
}

}  // namespace

UiSystem::UiSystem() : atlas_(&default_atlas()) {}

int UiSystem::load_dir(const std::string& dir, std::string* errors) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".ui.json") == 0) {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  int n = 0;
  for (const fs::path& p : files) {
    Document d;
    std::string err;
    if (!load_document(p.string(), &d, &err)) {
      if (errors) *errors += p.filename().string() + ": " + err + "\n";
      continue;
    }
    const std::string fname = p.filename().string();
    if (d.name.empty()) d.name = fname.substr(0, fname.size() - 8);
    add(std::move(d));
    ++n;
  }
  return n;
}

void UiSystem::add(Document doc) {
  for (Document& d : docs_) {
    if (d.name == doc.name) {
      d = std::move(doc);
      return;
    }
  }
  docs_.push_back(std::move(doc));
}

void UiSystem::clear() {
  docs_.clear();
  focus_doc_.clear();
  focus_.clear();
  hover_.clear();
  pressed_.clear();
}

bool UiSystem::has(const std::string& name) const {
  return std::any_of(docs_.begin(), docs_.end(),
                     [&](const Document& d) { return d.name == name; });
}

Document* UiSystem::document(const std::string& name) {
  for (Document& d : docs_) {
    if (d.name == name) return &d;
  }
  return nullptr;
}

std::vector<std::string> UiSystem::names() const {
  std::vector<std::string> v;
  for (const Document& d : docs_) v.push_back(d.name);
  return v;
}

bool UiSystem::show(const std::string& name) {
  Document* d = document(name);
  if (!d) return false;
  d->visible = true;
  if (d->modal) {
    layout(*d, last_w_, last_h_, *atlas_);
    focus_doc_.clear();
    focus_.clear();
    ensure_focus(*d);
  }
  return true;
}

bool UiSystem::hide(const std::string& name) {
  Document* d = document(name);
  if (!d) return false;
  d->visible = false;
  if (focus_doc_ == name) {
    focus_doc_.clear();
    focus_.clear();
  }
  return true;
}

bool UiSystem::visible(const std::string& name) const {
  for (const Document& d : docs_) {
    if (d.name == name) return d.visible;
  }
  return false;
}

Widget* UiSystem::find(const std::string& id) {
  const std::size_t dot = id.find('.');
  if (dot != std::string::npos) {
    if (Document* d = document(id.substr(0, dot))) {
      return find_widget(d->root, id.substr(dot + 1));
    }
  }
  for (Document& d : docs_) {
    if (Widget* w = find_widget(d.root, id)) return w;
  }
  return nullptr;
}

bool UiSystem::set_text(const std::string& id, const std::string& text) {
  Widget* w = find(id);
  if (!w) return false;
  w->text = text;
  return true;
}

bool UiSystem::set_value(const std::string& id, float value) {
  Widget* w = find(id);
  if (!w) return false;
  if (w->type == WidgetType::List) {
    w->selected = std::clamp(static_cast<int>(value), -1, static_cast<int>(w->items.size()) - 1);
  } else {
    w->value = std::clamp(value, std::min(w->min, w->max), std::max(w->min, w->max));
  }
  return true;
}

bool UiSystem::set_visible(const std::string& id, bool visible) {
  Widget* w = find(id);
  if (!w) return false;
  w->visible = visible;
  return true;
}

void UiSystem::set_focus(const std::string& doc, const std::string& widget) {
  focus_doc_ = doc;
  focus_ = widget;
}

bool UiSystem::modal_open() const {
  return std::any_of(docs_.begin(), docs_.end(),
                     [](const Document& d) { return d.visible && d.modal; });
}

bool UiSystem::wants_text() const {
  if (focus_.empty()) return false;
  for (const Document& d : docs_) {
    if (d.name == focus_doc_ && d.visible) {
      const Widget* w = find_widget(d.root, focus_);
      return w && w->type == WidgetType::TextInput;
    }
  }
  return false;
}

Document* UiSystem::active() {
  std::vector<Document*> order = draw_order(docs_);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    if ((*it)->modal) return *it;
  }
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    std::vector<Widget*> f;
    collect_focusable((*it)->root, &f);
    if (!f.empty()) return *it;
  }
  return nullptr;
}

void UiSystem::ensure_focus(Document& d) {
  if (focus_doc_ == d.name && !focus_.empty()) {
    const Widget* w = find_widget(d.root, focus_);
    if (w && w->visible) return;
  }
  std::vector<Widget*> f;
  collect_focusable(d.root, &f);
  focus_doc_ = d.name;
  focus_.clear();
  if (!d.focus.empty()) {
    for (Widget* w : f) {
      if (w->id == d.focus) focus_ = w->id;
    }
  }
  if (focus_.empty() && !f.empty()) focus_ = f.front()->id;
}

void UiSystem::move_focus(Document& d, int dx, int dy) {
  std::vector<Widget*> f;
  collect_focusable(d.root, &f);
  if (f.empty()) return;
  Widget* cur = nullptr;
  std::size_t cur_i = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (f[i]->id == focus_) {
      cur = f[i];
      cur_i = i;
    }
  }
  if (!cur) {
    focus_ = f.front()->id;
    return;
  }
  const float cx = cur->rect.x + cur->rect.w * 0.5f, cy = cur->rect.y + cur->rect.h * 0.5f;
  Widget* best = nullptr;
  float best_score = 1e30f;
  for (Widget* c : f) {
    if (c == cur) continue;
    const float vx = c->rect.x + c->rect.w * 0.5f - cx;
    const float vy = c->rect.y + c->rect.h * 0.5f - cy;
    const float along = dx != 0 ? vx * dx : vy * dy;
    const float perp = std::fabs(dx != 0 ? vy : vx);
    if (along <= 1.0f) continue;
    const float score = along + perp * 2.0f;
    if (score < best_score) {
      best_score = score;
      best = c;
    }
  }
  if (!best && dy != 0) {
    // Wrap around in document order.
    best = f[(cur_i + f.size() + (dy > 0 ? 1 : f.size() - 1)) % f.size()];
  }
  if (best) focus_ = best->id;
}

Widget* UiSystem::hit(Document& d, float x, float y) { return hit_widget(d.root, x, y); }

std::vector<UiEvent> UiSystem::update(const UiInput& in, float w, float h) {
  std::vector<UiEvent> ev;
  clock_ += in.dt;
  last_w_ = std::max(1.0f, w);
  last_h_ = std::max(1.0f, h);
  for (Document& d : docs_) {
    if (d.visible) layout(d, last_w_, last_h_, *atlas_);
  }
  const float s = ui_scale(last_w_, last_h_);
  auto emit = [&](UiEventKind k, const std::string& doc, const Widget& wd) {
    UiEvent e;
    e.kind = k;
    e.document = doc;
    e.widget = wd.id;
    e.value = wd.type == WidgetType::List ? static_cast<float>(wd.selected) : wd.value;
    e.text = wd.type == WidgetType::List
                 ? (wd.selected >= 0 && wd.selected < static_cast<int>(wd.items.size())
                        ? wd.items[static_cast<std::size_t>(wd.selected)]
                        : std::string())
                 : wd.text;
    ev.push_back(std::move(e));
  };

  // Mouse: topmost visible document under the pointer (only the modal
  // one while a modal is open).
  std::vector<Document*> order = draw_order(docs_);
  const bool modal = modal_open();
  Document* hover_doc = nullptr;
  Widget* hw = nullptr;
  if (in.mouse_x >= 0 && in.mouse_y >= 0) {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      if (modal && !(*it)->modal) continue;
      if (Widget* x = hit(**it, in.mouse_x, in.mouse_y)) {
        hw = x;
        hover_doc = *it;
        break;
      }
    }
  }
  hover_ = hw ? hw->id : std::string();
  const bool press = in.mouse_down && !mouse_was_down_;
  const bool release = !in.mouse_down && mouse_was_down_;
  mouse_was_down_ = in.mouse_down;
  auto slide_to = [&](Widget& sw, const std::string& doc) {
    const Rect tr = slider_track(sw, s);
    const float v = snap(sw, sw.min + (sw.max - sw.min) * std::clamp((in.mouse_x - tr.x) / tr.w, 0.0f, 1.0f));
    if (v != sw.value) {
      sw.value = v;
      emit(UiEventKind::Change, doc, sw);
    }
  };
  if (press && hw) {
    pressed_ = hw->id;
    pressed_doc_ = hover_doc->name;
    set_focus(hover_doc->name, hw->id);
    if (hw->type == WidgetType::Slider) {
      dragging_ = true;
      slide_to(*hw, hover_doc->name);
    } else if (hw->type == WidgetType::List) {
      const float rh = atlas_->line_height(hw->font, hw->font_size * s) + 6 * s;
      const int i = static_cast<int>((in.mouse_y - hw->rect.y - 4 * s) / rh);
      if (i >= 0 && i < static_cast<int>(hw->items.size()) && i != hw->selected) {
        hw->selected = i;
        emit(UiEventKind::Change, hover_doc->name, *hw);
      }
    }
  } else if (press && !hw && wants_text()) {
    focus_.clear();  // clicking elsewhere leaves the text box
  }
  if (dragging_ && in.mouse_down) {
    if (Document* d = document(pressed_doc_)) {
      if (Widget* sw = find_widget(d->root, pressed_)) slide_to(*sw, d->name);
    }
  }
  if (release) {
    if (!pressed_.empty() && hw && hw->id == pressed_ && hover_doc &&
        hover_doc->name == pressed_doc_ &&
        (hw->type == WidgetType::Button || hw->type == WidgetType::List)) {
      emit(UiEventKind::Click, hover_doc->name, *hw);
    }
    pressed_.clear();
    dragging_ = false;
  }

  // Keyboard / gamepad navigation in the active document.
  Document* act = active();
  const bool nav = in.up || in.down || in.left || in.right || in.accept;
  if (act && (focus_doc_ != act->name || focus_.empty())) {
    if (nav) {
      ensure_focus(*act);
      return ev;  // the first press only lands focus
    }
  }
  if (act && focus_doc_ == act->name && !focus_.empty()) {
    Widget* f = find_widget(act->root, focus_);
    if (f && f->visible) {
      switch (f->type) {
        case WidgetType::TextInput: {
          bool changed = false;
          for (char c : in.text) {
            if (c >= 32 && c < 127 && static_cast<int>(f->text.size()) < f->max_length) {
              f->text.push_back(c);
              changed = true;
            }
          }
          if (in.backspace && !f->text.empty()) {
            f->text.pop_back();
            changed = true;
          }
          if (changed) emit(UiEventKind::Change, act->name, *f);
          if (in.accept) emit(UiEventKind::Submit, act->name, *f);
          if (in.up || in.down) move_focus(*act, 0, in.down ? 1 : -1);
          break;
        }
        case WidgetType::Slider:
          if (in.left || in.right) {
            const float step = f->step > 0 ? f->step : (f->max - f->min) * 0.05f;
            const float v = snap(*f, f->value + (in.right ? step : -step));
            if (v != f->value) {
              f->value = v;
              emit(UiEventKind::Change, act->name, *f);
            }
          }
          if (in.up || in.down) move_focus(*act, 0, in.down ? 1 : -1);
          break;
        case WidgetType::List: {
          const int n = static_cast<int>(f->items.size());
          if (in.down && f->selected < n - 1) {
            ++f->selected;
            emit(UiEventKind::Change, act->name, *f);
          } else if (in.up && f->selected > 0) {
            --f->selected;
            emit(UiEventKind::Change, act->name, *f);
          } else if (in.up || in.down) {
            move_focus(*act, 0, in.down ? 1 : -1);
          }
          if (in.left || in.right) move_focus(*act, in.right ? 1 : -1, 0);
          if (in.accept) emit(UiEventKind::Click, act->name, *f);
          break;
        }
        default:
          if (in.up || in.down) move_focus(*act, 0, in.down ? 1 : -1);
          if (in.left || in.right) move_focus(*act, in.right ? 1 : -1, 0);
          if (in.accept) emit(UiEventKind::Click, act->name, *f);
          break;
      }
    }
  }
  if (in.back && act) {
    UiEvent e;
    e.kind = UiEventKind::Back;
    e.document = act->name;
    e.widget = focus_doc_ == act->name ? focus_ : std::string();
    ev.push_back(std::move(e));
  }
  return ev;
}

void UiSystem::draw(std::vector<UiDraw>* out) const {
  std::vector<Document*> order = draw_order(const_cast<std::vector<Document>&>(docs_));
  for (const Document* d : order) {
    DrawState st;
    st.scale = ui_scale(last_w_, last_h_);
    if (focus_doc_ == d->name) st.focus = &focus_;
    st.hover = &hover_;
    if (pressed_doc_ == d->name) st.pressed = &pressed_;
    st.caret = std::fmod(clock_, 1.0) < 0.55;
    draw_document(*d, st, *atlas_, theme, out);
  }
}

// --- pause menu -------------------------------------------------------------

namespace {
const char* kPauseJson = R"JSON({
  "name": "pause", "modal": true, "layer": 100, "focus": "resume",
  "root": {
    "type": "panel", "id": "pause_root", "anchor": [0.5, 0.5], "pivot": [0.5, 0.5],
    "size": [440, 0], "layout": "stack", "padding": 26, "gap": 14,
    "children": [
      {"type": "label", "id": "pause_title", "text": "Paused", "font": "display",
       "font_size": 56, "style": "title", "anchor": [0.5, 0]},
      {"type": "panel", "id": "pause_main", "style": "clear", "layout": "stack", "gap": 10,
       "stretch": [true, false], "children": [
        {"type": "button", "id": "resume", "text": "Resume", "font_size": 24, "stretch": [true, false], "style": "accent"},
        {"type": "button", "id": "settings", "text": "Settings", "font_size": 24, "stretch": [true, false]},
        {"type": "button", "id": "quit", "text": "Quit", "font_size": 24, "stretch": [true, false]}
      ]},
      {"type": "panel", "id": "pause_settings", "style": "clear", "layout": "stack", "gap": 10,
       "stretch": [true, false], "visible": false, "children": [
        {"type": "label", "text": "Volume", "style": "muted", "font_size": 18},
        {"type": "slider", "id": "vol_master", "text": "Master", "value": 1, "stretch": [true, false]},
        {"type": "slider", "id": "vol_music", "text": "Music", "value": 1, "stretch": [true, false]},
        {"type": "slider", "id": "vol_sfx", "text": "Effects", "value": 1, "stretch": [true, false]},
        {"type": "button", "id": "settings_back", "text": "Back", "font_size": 22, "stretch": [true, false]}
      ]}
    ]
  }
})JSON";
}  // namespace

Document builtin_pause_menu() {
  Document d;
  std::string err;
  parse_document(kPauseJson, &d, &err);
  return d;
}

void PauseMenu::install(UiSystem& ui) {
  if (!ui.has("pause")) ui.add(builtin_pause_menu());
}

void PauseMenu::open(UiSystem& ui, float master, float music, float sfx) {
  install(ui);
  ui.set_value("pause.vol_master", master);
  ui.set_value("pause.vol_music", music);
  ui.set_value("pause.vol_sfx", sfx);
  ui.set_visible("pause.pause_main", true);
  ui.set_visible("pause.pause_settings", false);
  ui.show("pause");
}

void PauseMenu::close(UiSystem& ui) { ui.hide("pause"); }

PauseMenu::Action PauseMenu::handle(UiSystem& ui, const UiEvent& ev, std::string* bus,
                                    float* value) {
  if (ev.document != "pause") return Action::None;
  const Widget* settings = ui.find("pause.pause_settings");
  const bool in_settings = settings && settings->visible;
  auto main_view = [&](bool main) {
    ui.set_visible("pause.pause_main", main);
    ui.set_visible("pause.pause_settings", !main);
    ui.set_focus("pause", main ? "settings" : "vol_master");
  };
  if (ev.kind == UiEventKind::Back) {
    if (in_settings) {
      main_view(true);
      return Action::None;
    }
    close(ui);
    return Action::Resume;
  }
  if (ev.kind == UiEventKind::Click) {
    if (ev.widget == "resume") {
      close(ui);
      return Action::Resume;
    }
    if (ev.widget == "quit") return Action::Quit;
    if (ev.widget == "settings") main_view(false);
    if (ev.widget == "settings_back") main_view(true);
    return Action::None;
  }
  if (ev.kind == UiEventKind::Change && ev.widget.rfind("vol_", 0) == 0) {
    if (bus) *bus = ev.widget.substr(4);
    if (value) *value = ev.value;
    return Action::Volume;
  }
  return Action::None;
}

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
