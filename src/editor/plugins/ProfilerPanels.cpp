#include "editor/plugins/ProfilerPanels.h"

#include "editor/ui/Theme.h"
#include "plugins/profiler/ProfilerPlugin.h"
#include "profiler/Console.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;
using profiler::FrameRecord;
using profiler::Profiler;

namespace {

double wall_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int resize_cb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* str = static_cast<std::string*>(d->UserData);
    str->resize(static_cast<std::size_t>(d->BufTextLen));
    d->Buf = str->data();
  }
  return 0;
}

bool input_str(const char* label, std::string* s, ImGuiInputTextFlags extra = 0,
               const char* hint = nullptr) {
  const ImGuiInputTextFlags f = ImGuiInputTextFlags_CallbackResize | extra;
  if (hint) return ImGui::InputTextWithHint(label, hint, s->data(), s->capacity() + 1, f, resize_cb, s);
  return ImGui::InputText(label, s->data(), s->capacity() + 1, f, resize_cb, s);
}

// Stable colour per zone name from the Dusk palette.
ImU32 zone_color(const std::string& name) {
  static const ImVec4 pal[] = {theme::Accent(), theme::Copper(), theme::Sand(),
                               theme::AccentMuted(), theme::CopperMuted(), theme::Success()};
  const std::string root = name.substr(0, name.find('.'));
  std::size_t h = std::hash<std::string>{}(root);
  ImVec4 c = pal[h % (sizeof(pal) / sizeof(pal[0]))];
  c.w = 0.85f;
  return theme::U32(c);
}

void scan_captures(AssayPanelState& s) {
  const double now = wall_seconds();
  if (s.scanned >= 0 && now - s.scanned < 2.0) return;
  s.scanned = now;
  s.captures.clear();
  std::error_code ec;
  const fs::path dir = Profiler::get().capture_dir();
  if (!fs::is_directory(dir, ec)) return;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string n = e.path().filename().string();
    if (n.size() > 12 && n.rfind(".tsprof.json") == n.size() - 12) s.captures.push_back(n);
  }
  std::sort(s.captures.rbegin(), s.captures.rend());
}

void sparkline(const char* label, const std::vector<float>& v, const char* fmt, float h = 28.0f) {
  if (v.empty()) {
    ImGui::TextDisabled("%s: no samples", label);
    return;
  }
  char overlay[96];
  std::snprintf(overlay, sizeof overlay, fmt, v.back());
  float hi = 0.0f;
  for (float x : v) hi = std::max(hi, x);
  ImGui::PushStyleColor(ImGuiCol_PlotLines, theme::Accent());
  ImGui::PlotLines(label, v.data(), static_cast<int>(v.size()), 0, overlay, 0.0f,
                   hi > 0 ? hi * 1.15f : 1.0f, ImVec2(-160, h));
  ImGui::PopStyleColor();
}

void frame_bars(AssayPanelState& s) {
  const auto& fr = s.frames;
  const float h = 70.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), theme::U32(theme::Charcoal()));
  ImGui::InvisibleButton("##bars", ImVec2(w, h));
  if (fr.empty()) {
    dl->AddText(ImVec2(p0.x + 8, p0.y + 8), theme::U32(theme::TextMuted()),
                "No frames yet. Press Play (or Record) and the bars fill in.");
    return;
  }
  const int n = static_cast<int>(fr.size());
  const float bw = std::max(1.0f, w / static_cast<float>(n));
  const double budget = 33.3;  // the bar top: 30 fps
  const auto y_of = [&](double ms) { return p0.y + h - static_cast<float>(std::min(ms, budget) / budget) * h; };
  for (int i = 0; i < n; ++i) {
    const float x = p0.x + w - static_cast<float>(n - i) * bw;
    if (x + bw < p0.x) continue;
    const double ms = fr[static_cast<std::size_t>(i)].cpu_ms;
    ImVec4 c = ms > 16.7 ? (ms > 33.3 ? theme::Danger() : theme::Warning()) : theme::Accent();
    if (i == s.selected) c = theme::Text();
    dl->AddRectFilled(ImVec2(x, y_of(ms)), ImVec2(x + std::max(1.0f, bw - 1), p0.y + h), theme::U32(c));
  }
  dl->AddLine(ImVec2(p0.x, y_of(16.7)), ImVec2(p0.x + w, y_of(16.7)), theme::U32(theme::Sand(), 0.5f));
  dl->AddText(ImVec2(p0.x + 4, y_of(16.7) - 14), theme::U32(theme::Sand(), 0.7f), "16.7 ms");
  if (ImGui::IsItemHovered()) {
    const float mx = ImGui::GetIO().MousePos.x;
    const int i = n - 1 - static_cast<int>((p0.x + w - mx) / bw);
    if (i >= 0 && i < n) {
      const FrameRecord& f = fr[static_cast<std::size_t>(i)];
      ImGui::SetTooltip("Frame %llu\n%.2f ms CPU, %zu zones", static_cast<unsigned long long>(f.index),
                        f.cpu_ms, f.zones.size());
      if (ImGui::IsMouseClicked(0)) {
        s.selected = i;
        if (s.live) s.live = false;  // freeze to inspect
      }
    }
  }
}

void timeline(AssayPanelState& s) {
  if (s.frames.empty()) return;
  if (s.selected < 0 || s.selected >= static_cast<int>(s.frames.size()))
    s.selected = static_cast<int>(s.frames.size()) - 1;
  const FrameRecord& f = s.frames[static_cast<std::size_t>(s.selected)];
  ImGui::Text("Frame %llu  %.2f ms", static_cast<unsigned long long>(f.index), f.cpu_ms);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160);
  ImGui::SliderFloat("Zoom", &s.zoom, 1.0f, 20.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
  int max_depth = 0;
  for (const auto& z : f.zones) max_depth = std::max(max_depth, static_cast<int>(z.depth));
  const float row = 20.0f;
  const float h = row * static_cast<float>(max_depth + 1) + 4;
  ImGui::BeginChild("##flame", ImVec2(0, h + 18), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  const float w = std::max(200.0f, ImGui::GetContentRegionAvail().x * s.zoom);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##flame_area", ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const double span = std::max(0.001, f.cpu_ms);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  for (const auto& z : f.zones) {
    if (z.thread != 0) continue;
    const float x0 = p0.x + static_cast<float>(z.start_ms / span) * w;
    const float x1 = std::max(x0 + 1.0f, p0.x + static_cast<float>((z.start_ms + z.dur_ms) / span) * w);
    const float y0 = p0.y + row * z.depth + 2;
    const ImVec2 a(x0, y0), b(x1, y0 + row - 2);
    dl->AddRectFilled(a, b, zone_color(z.name), 3.0f);
    if (x1 - x0 > 40) {
      char label[128];
      std::snprintf(label, sizeof label, "%s %.2f", z.name.c_str(), z.dur_ms);
      dl->PushClipRect(a, b, true);
      dl->AddText(ImVec2(x0 + 4, y0 + 2), theme::U32(theme::TextOnAccent()), label);
      dl->PopClipRect();
    }
    if (ImGui::IsItemHovered() && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) {
      ImGui::SetTooltip("%s\n%.3f ms (starts at %.3f ms)", z.name.c_str(), z.dur_ms, z.start_ms);
    }
  }
  ImGui::EndChild();
}

void stats_table(AssayPanelState& s) {
  ImGui::SetNextItemWidth(120);
  ImGui::SliderInt("Frames", &s.stats_window, 10, 600);
  const auto stats = profiler::zone_stats(s.frames, static_cast<std::size_t>(s.stats_window));
  if (ImGui::BeginTable("##zones", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, 220))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Scope", 0, 3.0f);
    ImGui::TableSetupColumn("Calls");
    ImGui::TableSetupColumn("Avg ms");
    ImGui::TableSetupColumn("Self ms");
    ImGui::TableSetupColumn("Max ms");
    ImGui::TableSetupColumn("Total ms");
    ImGui::TableHeadersRow();
    for (const auto& z : stats) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(zone_color(z.name)), "%s", z.name.c_str());
      ImGui::TableNextColumn(); ImGui::Text("%d", z.calls);
      ImGui::TableNextColumn(); ImGui::Text("%.3f", z.avg_ms);
      ImGui::TableNextColumn(); ImGui::Text("%.3f", z.self_ms);
      ImGui::TableNextColumn();
      if (z.max_ms > 16.7) ImGui::TextColored(theme::Danger(), "%.3f", z.max_ms);
      else ImGui::Text("%.3f", z.max_ms);
      ImGui::TableNextColumn(); ImGui::Text("%.2f", z.total_ms);
    }
    ImGui::EndTable();
  }
}

std::vector<FrameRecord> live_frames() { return Profiler::get().frames(); }

void counter_rows(const std::vector<FrameRecord>& frames, const std::string& prefix) {
  for (const std::string& name : counter_names(frames, prefix)) {
    std::vector<float> v;
    counter_series(frames, name, &v);
    sparkline(name.c_str(), v, "%.1f");
  }
}

}  // namespace

double counter_series(const std::vector<FrameRecord>& frames, const std::string& name,
                      std::vector<float>* out) {
  double sum = 0.0;
  for (const auto& f : frames) {
    auto it = f.counters.find(name);
    if (it == f.counters.end()) continue;
    sum += it->second;
    if (out) out->push_back(static_cast<float>(it->second));
  }
  return sum;
}

std::vector<std::string> counter_names(const std::vector<FrameRecord>& frames, const std::string& prefix) {
  std::set<std::string> names;
  for (const auto& f : frames)
    for (const auto& kv : f.counters)
      if (kv.first.rfind(prefix, 0) == 0) names.insert(kv.first);
  return {names.begin(), names.end()};
}

void draw_assay_panel(AssayPanelState& s) {
  Profiler& p = Profiler::get();
  bool rec = p.enabled();
  {  // recording lamp (drawn: the UI font has no circle glyph)
    const float r = ImGui::GetTextLineHeight() * 0.32f;
    const ImVec2 c = ImGui::GetCursorScreenPos();
    const ImVec2 mid(c.x + r + 1, c.y + ImGui::GetFrameHeight() * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (rec) dl->AddCircleFilled(mid, r, theme::U32(theme::Danger()));
    else dl->AddCircle(mid, r, theme::U32(theme::TextMuted()));
    ImGui::Dummy(ImVec2(r * 2 + 2, ImGui::GetFrameHeight()));
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Record", &rec)) p.set_enabled(rec);
  ImGui::SameLine();
  if (ImGui::Button(s.live ? "Freeze" : "Live")) {
    s.live = !s.live;
    s.loaded_path.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    p.clear();
    s.frames.clear();
    s.selected = -1;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140);
  input_str("##capname", &s.capture_name, 0, "capture name");
  ImGui::SameLine();
  if (ImGui::Button("Capture")) {
    std::string err;
    const std::string path = p.capture(&err, s.capture_name);
    s.status = path.empty() ? "Capture failed: " + err : "Saved " + path;
    s.scanned = -1;
  }
  ImGui::SameLine();
  scan_captures(s);
  if (ImGui::BeginCombo("##load", s.loaded_path.empty() ? "Load capture..." : fs::path(s.loaded_path).filename().string().c_str(),
                        ImGuiComboFlags_WidthFitPreview)) {
    if (s.captures.empty()) ImGui::TextDisabled("No captures in %s", p.capture_dir().c_str());
    for (const auto& c : s.captures) {
      if (ImGui::Selectable(c.c_str())) {
        std::string err;
        std::vector<FrameRecord> fr;
        const std::string path = (fs::path(p.capture_dir()) / c).string();
        if (profiler::load_capture(path, &fr, &err)) {
          s.frames = std::move(fr);
          s.live = false;
          s.loaded_path = path;
          s.selected = static_cast<int>(s.frames.size()) - 1;
          s.status = "Loaded " + c + " (" + std::to_string(s.frames.size()) + " frames)";
        } else {
          s.status = "Load failed: " + err;
        }
      }
    }
    ImGui::EndCombo();
  }
  if (s.live) {
    s.frames = live_frames();
    s.selected = static_cast<int>(s.frames.size()) - 1;
  }
  ImGui::TextColored(theme::Accent(), "%.1f fps", p.fps());
  ImGui::SameLine();
  ImGui::TextDisabled("avg %.2f ms  max %.2f ms  %zu frames%s", p.avg_frame_ms(), p.max_frame_ms(),
                      s.frames.size(), s.live ? "" : (s.loaded_path.empty() ? "  (frozen)" : "  (capture)"));
  if (!s.status.empty()) ImGui::TextDisabled("%s", s.status.c_str());
  ImGui::Separator();
  frame_bars(s);
  ImGui::SeparatorText("Timeline");
  timeline(s);
  ImGui::SeparatorText("Scopes");
  stats_table(s);
}

void draw_render_memory_panel() {
  const auto frames = live_frames();
  if (frames.empty()) {
    ImGui::TextDisabled("Nothing recorded. Press Play or tick Record in the Assay Office.");
    return;
  }
  const FrameRecord& last = frames.back();
  ImGui::SeparatorText("GPU passes");
  if (last.gpu_ms.empty()) {
    ImGui::TextDisabled("No GPU timer queries (2D, the null renderer, or GL without timer queries).");
  } else {
    double total = 0;
    for (const auto& kv : last.gpu_ms) total += kv.second;
    for (const auto& kv : last.gpu_ms) {
      ImGui::Text("%-12s %6.3f ms", kv.first.c_str(), kv.second);
      ImGui::SameLine(200);
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::Copper());
      ImGui::ProgressBar(total > 0 ? static_cast<float>(kv.second / total) : 0.0f, ImVec2(-1, 0), "");
      ImGui::PopStyleColor();
    }
    ImGui::TextColored(theme::Accent(), "GPU total %.3f ms", total);
  }
  ImGui::SeparatorText("Draws");
  counter_rows(frames, "render.");
  ImGui::SeparatorText("Assets");
  counter_rows(frames, "assets.");
  ImGui::SeparatorText("Memory");
  counter_rows(frames, "mem.");
  ImGui::SeparatorText("World");
  counter_rows(frames, "world.");
}

void draw_net_panel() {
  const auto frames = live_frames();
  if (counter_names(frames, "net.").empty()) {
    ImGui::TextDisabled("No posse on the wire. Ride with a Posse (host or join) while recording.");
    return;
  }
  const auto client = counter_names(frames, "net.");
  bool any_client = false;
  for (const auto& n : client) any_client |= n.rfind("net.server.", 0) != 0;
  if (any_client) {
    ImGui::SeparatorText("Client");
    for (const auto& n : client) {
      if (n.rfind("net.server.", 0) == 0) continue;
      std::vector<float> v;
      counter_series(frames, n, &v);
      sparkline(n.c_str(), v, "%.1f");
    }
  }
  if (!counter_names(frames, "net.server.").empty()) {
    ImGui::SeparatorText("Server");
    counter_rows(frames, "net.server.");
  }
}

bool draw_console_line(ConsoleLineState& s, std::string* echo, std::string* output) {
  ImGui::SetNextItemWidth(-1);
  const bool enter = input_str("##console_line", &s.line, ImGuiInputTextFlags_EnterReturnsTrue,
                               "> command (help, fps, scopes, counters, capture...)");
  if (ImGui::IsItemFocused() && !s.history.empty()) {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      s.cursor = s.cursor < 0 ? static_cast<int>(s.history.size()) - 1 : std::max(0, s.cursor - 1);
      s.line = s.history[static_cast<std::size_t>(s.cursor)];
    }
  }
  if (!enter || s.line.empty()) return false;
  if (echo) *echo = "> " + s.line;
  const std::string out = profiler::Console::shared().run(s.line);
  if (output) *output = out;
  s.history.push_back(s.line);
  s.cursor = -1;
  s.line.clear();
  ImGui::SetKeyboardFocusHere(-1);
  return true;
}

void install_profiler_panels() {
  plugins::ProfilerPlugin::set_editor_hook([](plugin::Registrar& reg) {
    auto st = std::make_shared<AssayPanelState>();
    plugin::EditorPanel a;
    a.id = "profiler.assay_office";
    a.title = "Assay Office";
    a.menu_label = "Assay Office (Profiler)";
    a.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver); };
    a.draw = [st] { draw_assay_panel(*st); };
    reg.add_editor_panel(std::move(a));
    plugin::EditorPanel r;
    r.id = "profiler.render_memory";
    r.title = "Render & Memory";
    r.menu_label = "Render & Memory";
    r.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver); };
    r.draw = [] { draw_render_memory_panel(); };
    reg.add_editor_panel(std::move(r));
    plugin::EditorPanel n;
    n.id = "profiler.net";
    n.title = "Net";
    n.menu_label = "Net Stats";
    n.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_FirstUseEver); };
    n.draw = [] { draw_net_panel(); };
    reg.add_editor_panel(std::move(n));
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
