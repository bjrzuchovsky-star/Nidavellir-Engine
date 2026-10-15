#include "debugtools/DebugTools.h"

#include "profiler/Profiler.h"

#include <algorithm>
#include <cstdio>

namespace ts {
namespace tombstone {
namespace profiler {

std::string builtin_overlay_ui() {
  return R"({
  "name": "debug_overlay", "visible": false, "modal": false, "layer": 90,
  "root": {
    "type": "panel", "id": "ov_root", "anchor": [0, 0], "pivot": [0, 0], "offset": [12, 12],
    "size": [430, 0], "layout": "stack", "padding": 12, "gap": 3, "style": "dark",
    "children": [
      {"type": "label", "id": "ov_title", "text": "Assay Office", "font": "display", "font_size": 20, "style": "title"},
      {"type": "label", "id": "ov_fps", "text": "", "font_size": 16},
      {"type": "progress", "id": "ov_budget", "value": 0, "size": [400, 8]},
      {"type": "label", "id": "ov_scopes", "text": "", "font_size": 14},
      {"type": "label", "id": "ov_render", "text": "", "font_size": 14, "style": "muted"},
      {"type": "label", "id": "ov_net", "text": "", "font_size": 14, "style": "muted"}
    ]
  }
})";
}

std::string builtin_console_ui() {
  return R"({
  "name": "debug_console", "visible": false, "modal": true, "layer": 95, "focus": "dc_input",
  "root": {
    "type": "panel", "id": "dc_root", "anchor": [0.5, 0], "pivot": [0.5, 0], "offset": [0, 0],
    "size": [1100, 0], "layout": "stack", "padding": 14, "gap": 8, "style": "dark",
    "children": [
      {"type": "label", "text": "Console", "font": "display", "font_size": 22, "style": "title"},
      {"type": "label", "id": "dc_log", "text": "", "font_size": 15},
      {"type": "input", "id": "dc_input", "text": "", "placeholder": "help, fps, scopes, capture, overlay ...",
       "font_size": 17, "max_length": 200, "stretch": [true, false]}
    ]
  }
})";
}

namespace {

std::string f1(const char* f, double a) {
  char b[96];
  std::snprintf(b, sizeof b, f, a);
  return b;
}

double counter_of(const FrameRecord& f, const char* k, double fb = -1) {
  const auto it = f.counters.find(k);
  return it == f.counters.end() ? fb : it->second;
}

}  // namespace

DebugTools::~DebugTools() {
  if (registered_) {
    console_.remove("overlay");
    console_.remove("clear");
  }
}

void DebugTools::attach(ui::UiSystem& ui) {
  ui_ = &ui;
  registered_ = true;
  console_.add("overlay", "overlay [on|off]: the in-game stats overlay (F3)", [this](const std::vector<std::string>& a) {
    if (a.size() > 1) set_overlay(a[1] == "on" || a[1] == "1");
    else toggle_overlay();
    return std::string("overlay ") + (overlay_ ? "on" : "off");
  });
  console_.add("clear", "clear the console", [this](const std::vector<std::string>&) {
    console_.clear_log();
    return std::string();
  });
  for (const std::string& text : {builtin_overlay_ui(), builtin_console_ui()}) {
    ui::Document d;
    if (ui::parse_document(text, &d, nullptr) && !ui.has(d.name)) ui.add(std::move(d));
  }
  shown_log_ = static_cast<std::size_t>(-1);
  if (overlay_) ui.show("debug_overlay");
  if (console_open_) ui.show("debug_console");
}

void DebugTools::detach() {
  set_overlay(false);
  set_console(false);
  ui_ = nullptr;
}

void DebugTools::set_overlay(bool on) {
  overlay_ = on;
  if (ui_) on ? (void)ui_->show("debug_overlay") : (void)ui_->hide("debug_overlay");
  Profiler& p = Profiler::get();
  if ((overlay_ || console_open_) && !p.enabled()) {
    p.set_enabled(true);
    switched_on_ = true;
  } else if (!overlay_ && !console_open_ && switched_on_) {
    p.set_enabled(false);
    switched_on_ = false;
  }
  since_ = 1.0;
}

void DebugTools::set_console(bool on) {
  console_open_ = on;
  if (ui_) {
    if (on) {
      ui_->show("debug_console");
      ui_->set_text("debug_console.dc_input", "");
      ui_->set_focus("debug_console", "dc_input");
    } else {
      ui_->hide("debug_console");
    }
  }
  shown_log_ = static_cast<std::size_t>(-1);
  set_overlay(overlay_);  // keeps the profiler switch consistent
}

std::vector<ui::UiEvent> DebugTools::filter(std::vector<ui::UiEvent> events) {
  std::vector<ui::UiEvent> rest;
  for (ui::UiEvent& e : events) {
    if (e.document != "debug_console" && e.document != "debug_overlay") {
      rest.push_back(std::move(e));
      continue;
    }
    if (e.document == "debug_console") {
      if (e.kind == ui::UiEventKind::Back) {
        set_console(false);
      } else if (e.kind == ui::UiEventKind::Submit && e.widget == "dc_input") {
        std::string line = e.text;
        line.erase(std::remove(line.begin(), line.end(), '`'), line.end());
        if (!line.empty()) console_.run(line);
        if (ui_) {
          ui_->set_text("debug_console.dc_input", "");
          ui_->set_focus("debug_console", "dc_input");
        }
      }
    }
  }
  return rest;
}

std::string DebugTools::overlay_text() {
  Profiler& p = Profiler::get();
  FrameRecord f;
  const bool have = p.last(&f);
  std::string o = f1("%.0f fps", p.fps()) + f1("  cpu %.2f ms", p.avg_frame_ms()) +
                  f1(" (worst %.2f)", p.max_frame_ms());
  if (have && !f.gpu_ms.empty()) {
    double g = 0;
    for (const auto& [k, v] : f.gpu_ms) g += v;
    o += f1("  gpu %.2f ms", g);
  }
  o += "\n";
  const auto st = p.zone_stats(60);
  for (std::size_t i = 0; i < st.size() && i < 5; ++i) {
    char b[128];
    std::snprintf(b, sizeof b, "%-22s %6.3f ms\n", st[i].name.c_str(), st[i].avg_ms);
    o += b;
  }
  if (have) {
    const double draws = counter_of(f, "render.draws"), tris = counter_of(f, "render.triangles");
    if (draws >= 0) o += f1("draws %.0f", draws) + f1("  tris %.0f", std::max(0.0, tris)) + "\n";
    o += f1("mem %.1f MB", counter_of(f, "mem.process_mb", process_memory_mb()));
    const double lua = counter_of(f, "mem.lua_kb");
    if (lua >= 0) o += f1("  lua %.0f KB", lua);
    const double ents = counter_of(f, "world.entities");
    if (ents >= 0) o += f1("  entities %.0f", ents);
    o += "\n";
    const double rtt = counter_of(f, "net.rtt_ms");
    if (rtt >= 0) o += f1("net rtt %.0f ms", rtt) + f1("  in %.1f KB/s", counter_of(f, "net.kbps_in", 0)) +
                       f1("  out %.1f KB/s", counter_of(f, "net.kbps_out", 0)) + "\n";
  }
  return o;
}

void DebugTools::refresh_overlay() {
  if (!ui_) return;
  Profiler& p = Profiler::get();
  FrameRecord f;
  const bool have = p.last(&f);
  double gpu = 0;
  if (have) for (const auto& [k, v] : f.gpu_ms) gpu += v;
  std::string fps = f1("%.0f fps", p.fps()) + f1("   cpu %.2f ms", p.avg_frame_ms()) +
                    f1("  worst %.2f", p.max_frame_ms());
  if (gpu > 0) fps += f1("   gpu %.2f ms", gpu);
  ui_->set_text("debug_overlay.ov_fps", fps);
  ui_->set_value("debug_overlay.ov_budget", static_cast<float>(std::min(1.0, p.avg_frame_ms() / 16.667)));
  std::string scopes;
  const auto st = p.zone_stats(60);
  for (std::size_t i = 0; i < st.size() && i < 5; ++i) {
    char b[128];
    std::snprintf(b, sizeof b, "%s  %.3f ms", st[i].name.c_str(), st[i].avg_ms);
    scopes += (scopes.empty() ? "" : "\n") + std::string(b);
  }
  ui_->set_text("debug_overlay.ov_scopes", scopes.empty() ? "(no scopes yet)" : scopes);
  std::string render;
  if (have) {
    const double draws = counter_of(f, "render.draws");
    if (draws >= 0) render += f1("draws %.0f", draws) + f1("  tris %.0f", std::max(0.0, counter_of(f, "render.triangles", 0))) + "\n";
    render += f1("mem %.1f MB", counter_of(f, "mem.process_mb", process_memory_mb()));
    const double lua = counter_of(f, "mem.lua_kb");
    if (lua >= 0) render += f1("  lua %.0f KB", lua);
    const double ents = counter_of(f, "world.entities");
    if (ents >= 0) render += f1("  entities %.0f", ents);
    const double tex = counter_of(f, "assets.textures"), mesh = counter_of(f, "assets.meshes");
    if (tex >= 0 || mesh >= 0) render += f1("\ntextures %.0f", std::max(0.0, tex)) + f1("  meshes %.0f", std::max(0.0, mesh));
  }
  ui_->set_text("debug_overlay.ov_render", render);
  std::string net;
  if (have) {
    const double rtt = counter_of(f, "net.rtt_ms");
    if (rtt >= 0) net = f1("net rtt %.0f ms", rtt) + f1("  in %.1f KB/s", counter_of(f, "net.kbps_in", 0)) +
                        f1("  out %.1f KB/s", counter_of(f, "net.kbps_out", 0));
  }
  ui_->set_text("debug_overlay.ov_net", net.empty() ? "net: not riding with a posse" : net);
}

void DebugTools::refresh_console() {
  if (!ui_ || shown_log_ == console_.log().size()) return;
  shown_log_ = console_.log().size();
  std::string text;
  const auto& log = console_.log();
  const std::size_t from = log.size() > 14 ? log.size() - 14 : 0;
  for (std::size_t i = from; i < log.size(); ++i) text += (text.empty() ? "" : "\n") + log[i];
  ui_->set_text("debug_console.dc_log", text.empty() ? "Type help and press Enter. Backtick or Esc closes." : text);
}

void DebugTools::update(double dt) {
  if (!ui_) return;
  if (console_open_) {
    if (ui::Widget* w = ui_->find("debug_console.dc_input")) {
      w->text.erase(std::remove(w->text.begin(), w->text.end(), '`'), w->text.end());
    }
    refresh_console();
  }
  since_ += dt;
  if (overlay_ && since_ >= 0.25) {
    since_ = 0.0;
    refresh_overlay();
  }
}

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
