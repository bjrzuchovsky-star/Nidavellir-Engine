#pragma once

// Glue between the game worlds' Lua ui.* queue and a ui::UiSystem.
// Header-only so ts_ui stays independent of the runtime; hosts that link
// both (ts_game, ts_client, the editor) include it.

#include "runtime/Script.h"
#include "ui/Ui.h"

#include <vector>

namespace ts {
namespace tombstone {
namespace ui {

// Apply ui.show / hide / set_text / set_value / set_visible requests.
inline void apply_ui_commands(UiSystem& ui, const std::vector<runtime::UiCommand>& cmds) {
  using K = runtime::UiCommand::Kind;
  for (const runtime::UiCommand& c : cmds) {
    switch (c.kind) {
      case K::Show: ui.show(c.target); break;
      case K::Hide: ui.hide(c.target); break;
      case K::SetText: ui.set_text(c.target, c.text); break;
      case K::SetValue: ui.set_value(c.target, static_cast<float>(c.value)); break;
      case K::SetVisible: ui.set_visible(c.target, c.value != 0.0); break;
      case K::AddDocument: {
        if (ui.has(c.target)) break;
        Document d;
        if (parse_document(c.text, &d, nullptr)) {
          d.name = c.target;
          const bool show = d.visible;
          ui.add(std::move(d));
          if (show) ui.show(c.target);
        }
        break;
      }
      case K::SetItems: {
        if (Widget* w = ui.find(c.target)) {
          w->items.clear();
          std::size_t start = 0;
          while (start <= c.text.size() && !c.text.empty()) {
            const std::size_t nl = c.text.find('\n', start);
            w->items.push_back(c.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
          }
          if (w->selected >= static_cast<int>(w->items.size())) w->selected = static_cast<int>(w->items.size()) - 1;
          if (w->selected < 0 && !w->items.empty()) w->selected = 0;
        }
        break;
      }
      case K::Focus: {
        const std::size_t dot = c.target.find('.');
        if (dot != std::string::npos) ui.set_focus(c.target.substr(0, dot), c.target.substr(dot + 1));
        break;
      }
    }
  }
}

// Clicks (and text-input submits) reach ui.on_click handlers, changes reach
// ui.on_change. World is runtime::World or world3d::World3D.
template <class World>
void forward_ui_events(World& world, const std::vector<UiEvent>& events) {
  for (const UiEvent& e : events) {
    if (e.kind == UiEventKind::Click || e.kind == UiEventKind::Submit) {
      world.ui_event(e.widget, false, e.value, e.text);
    } else if (e.kind == UiEventKind::Change) {
      world.ui_event(e.widget, true, e.value, e.text);
    }
  }
}

}  // namespace ui
}  // namespace tombstone
}  // namespace ts
