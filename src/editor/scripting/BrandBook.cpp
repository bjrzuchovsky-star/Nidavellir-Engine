#include "editor/scripting/BrandBook.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/Actions.h"

#include "editor/ui/Theme.h"
#include "runtime/CSharpScript.h"
#include "runtime/NativeScript.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;
using runtime::Graph;
using runtime::GraphLink;
using runtime::GraphNode;
using runtime::GraphNodeDef;

namespace {

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

ScriptValue N(double v) { return ScriptValue::of_number(v); }
ScriptValue T(const std::string& v) { return ScriptValue::of_text(v); }

// The Brand Book's palette: one colour per node family.
ImVec4 family_color(const std::string& cat) {
  if (cat == "Events") return theme::Accent();
  if (cat == "Flow") return theme::Copper();
  if (cat == "Math" || cat == "Compare") return theme::Sand();
  if (cat == "Values") return ImVec4(0.62f, 0.58f, 0.50f, 1.0f);
  if (cat == "Variables") return ImVec4(0.42f, 0.64f, 0.58f, 1.0f);
  if (cat == "Entities") return ImVec4(0.47f, 0.57f, 0.74f, 1.0f);
  if (cat == "Audio") return ImVec4(0.64f, 0.52f, 0.74f, 1.0f);
  if (cat == "Particles") return ImVec4(0.86f, 0.46f, 0.30f, 1.0f);
  if (cat == "UI") return ImVec4(0.80f, 0.70f, 0.44f, 1.0f);
  if (cat == "Plugins") return ImVec4(0.56f, 0.68f, 0.40f, 1.0f);
  return theme::TextMuted();
}

ImVec4 value_color(const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Bool: return theme::Copper();
    case ScriptValue::Type::Text: return ImVec4(0.80f, 0.76f, 0.66f, 1.0f);
    case ScriptValue::Type::Number: break;
  }
  return theme::Sand();
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

constexpr float kNodeW = 196.0f;
constexpr float kHeaderH = 30.0f;
constexpr float kRowH = 21.0f;
constexpr float kPinR = 5.5f;

struct Layout {
  const GraphNodeDef* def = nullptr;
  std::vector<std::pair<std::string, bool>> left;   // name, exec
  std::vector<std::pair<std::string, bool>> right;
  float h = 0.0f;
};

Layout layout_of(const GraphNode& n) {
  Layout l;
  l.def = runtime::find_graph_node(n.type);
  if (!l.def) {
    l.h = kHeaderH + kRowH;
    return l;
  }
  if (l.def->exec_in) l.left.push_back({"in", true});
  for (const auto& p : l.def->data_ins) l.left.push_back({p.name, false});
  for (const auto& p : l.def->exec_outs) l.right.push_back({p, true});
  for (const auto& p : l.def->data_outs) l.right.push_back({p, false});
  const std::size_t rows = std::max<std::size_t>(std::max(l.left.size(), l.right.size()), 1);
  l.h = kHeaderH + kRowH * static_cast<float>(rows) + 8.0f;
  return l;
}

bool is_exec_out(const Graph& g, int node, const std::string& pin) {
  const GraphNode* n = g.node(node);
  const GraphNodeDef* d = n ? runtime::find_graph_node(n->type) : nullptr;
  return d && std::find(d->exec_outs.begin(), d->exec_outs.end(), pin) != d->exec_outs.end();
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct LanguageStatus {
  std::string cpp, cs;
  bool cpp_ok = false, cs_ok = false;
};
std::shared_future<LanguageStatus>& language_status() {
  static std::shared_future<LanguageStatus> f = std::async(std::launch::async, [] {
    LanguageStatus s;
    std::string why;
    s.cpp_ok = runtime::native_scripts::toolchain_available(&why);
    s.cpp = s.cpp_ok ? "ready (cmake + host compiler)" : why;
    s.cs_ok = runtime::csharp_scripts::sdk_available(&why);
    s.cs = s.cs_ok ? "ready (.NET SDK " + runtime::csharp_scripts::dotnet_info().sdk + ")"
                   : std::string("Install the .NET 8 SDK to enable C#");
    return s;
  }).share();
  return f;
}

}  // namespace

BrandBook& BrandBook::get() {
  static BrandBook b;
  return b;
}

Graph BrandBook::starter_graph() {
  Graph g;
  auto add = [&g](const std::string& type, float x, float y, std::map<std::string, ScriptValue> in = {}) {
    GraphNode n;
    n.id = g.next_id();
    n.type = type;
    n.x = x;
    n.y = y;
    n.inputs = std::move(in);
    g.nodes.push_back(n);
    return n.id;
  };
  auto link = [&g](int a, const std::string& pa, int b, const std::string& pb) { g.links.push_back({a, pa, b, pb}); };
  g.variables.push_back({"visits", N(0)});
  const int start = add("event.start", 0, 0);
  const int reset = add("var.set", 230, 0, {{"name", T("visits")}, {"value", N(0)}});
  const int interact = add("event.interact", 0, 170);
  const int visit = add("var.add", 230, 170, {{"name", T("visits")}, {"amount", N(1)}});
  const int dry = add("cmp.ge", 230, 320, {{"b", N(3)}});
  const int branch = add("flow.branch", 460, 170);
  const int toast = add("toast", 690, 60, {{"text", T("The well's run dry, pardner")}, {"seconds", N(3)}});
  const int sound = add("audio.play", 690, 210, {{"clip", T("builtin:click")}, {"volume", N(0.8)}});
  const int dust = add("fx.burst", 690, 350, {{"preset", T("dust")}});
  link(start, "out", reset, "in");
  link(interact, "out", visit, "in");
  link(visit, "out", branch, "in");
  link(visit, "value", dry, "a");
  link(dry, "value", branch, "condition");
  link(branch, "true", toast, "in");
  link(branch, "false", sound, "in");
  link(sound, "out", dust, "in");
  return g;
}

void BrandBook::open(const std::string& project_dir, const std::string& project_name) {
  if (project_dir != project_dir_) {
    rel_.clear();
    graph_ = Graph{};
    dirty_ = false;
  }
  project_dir_ = project_dir;
  project_name_ = project_name;
  open_ = true;
  (void)language_status();
  refresh_list();
  if (rel_.empty() && !graphs_.empty()) load(graphs_.front());
}

void BrandBook::open_graph(const std::string& project_dir, const std::string& project_name,
                           const std::string& rel) {
  open(project_dir, project_name);
  std::error_code ec;
  if (!fs::exists(fs::path(project_dir_) / rel, ec)) {
    graph_ = starter_graph();
    rel_ = rel;
    save();
    refresh_list();
  }
  load(rel);
}

void BrandBook::refresh_list() {
  list_time_ = now_s();
  graphs_.clear();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(project_dir_) / "scripts", ec)) {
    const std::string name = e.path().filename().string();
    if (name.size() > 11 && name.compare(name.size() - 11, 11, ".graph.json") == 0) {
      graphs_.push_back("scripts/" + name);
    }
  }
  std::sort(graphs_.begin(), graphs_.end());
}

bool BrandBook::load(const std::string& rel) {
  std::string err;
  Graph g;
  if (!runtime::parse_graph(read_file(fs::path(project_dir_) / rel), &g, &err)) {
    status_ = rel + ": " + err;
    return false;
  }
  graph_ = std::move(g);
  rel_ = rel;
  dirty_ = false;
  selected_ = 0;
  status_ = "Opened " + rel;
  fit_pending_ = true;
  recheck();
  return true;
}

bool BrandBook::save() {
  if (rel_.empty()) return false;
  std::error_code ec;
  const fs::path p = fs::path(project_dir_) / rel_;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o << runtime::write_graph(graph_);
  if (!o) {
    status_ = "Could not write " + rel_;
    return false;
  }
  dirty_ = false;
  status_ = "Saved " + rel_ + " (a running ride hot-reloads it)";
  return true;
}

void BrandBook::recheck() {
  problems_.clear();
  runtime::check_graph(graph_, &problems_);
}

int BrandBook::add_node(const std::string& type, float x, float y) {
  GraphNode n;
  n.id = graph_.next_id();
  n.type = type;
  n.x = std::round(x / 10.0f) * 10.0f;
  n.y = std::round(y / 10.0f) * 10.0f;
  graph_.nodes.push_back(n);
  std::sort(graph_.nodes.begin(), graph_.nodes.end(),
            [](const GraphNode& a, const GraphNode& b) { return a.id < b.id; });
  dirty_ = true;
  recheck();
  return n.id;
}

void BrandBook::delete_node(int id) {
  graph_.nodes.erase(std::remove_if(graph_.nodes.begin(), graph_.nodes.end(),
                                    [id](const GraphNode& n) { return n.id == id; }),
                     graph_.nodes.end());
  graph_.links.erase(std::remove_if(graph_.links.begin(), graph_.links.end(),
                                    [id](const GraphLink& l) { return l.from_node == id || l.to_node == id; }),
                     graph_.links.end());
  if (selected_ == id) selected_ = 0;
  dirty_ = true;
  recheck();
}

void BrandBook::connect(const Pin& a, const Pin& b) {
  if (a.output == b.output || a.exec != b.exec || a.node == b.node) return;
  const Pin& from = a.output ? a : b;
  const Pin& to = a.output ? b : a;
  GraphLink l{from.node, from.name, to.node, to.name};
  if (std::find(graph_.links.begin(), graph_.links.end(), l) != graph_.links.end()) return;
  if (!to.exec) {  // a data input takes one wire
    graph_.links.erase(std::remove_if(graph_.links.begin(), graph_.links.end(),
                                      [&](const GraphLink& x) { return x.to_node == to.node && x.to_pin == to.name; }),
                       graph_.links.end());
  }
  graph_.links.push_back(l);
  dirty_ = true;
  recheck();
}

void BrandBook::draw() {
  if (!open_) return;
  if (now_s() - list_time_ > 2.0) refresh_list();
  if (fill_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
  } else {
    ImGui::SetNextWindowSize(ImVec2(1280, 760), ImGuiCond_FirstUseEver);
  }
  std::string title = "Brand Book - " + (rel_.empty() ? std::string("visual scripts") : rel_) +
                      (dirty_ ? " *" : "") + "###BrandBook";
  if (!ImGui::Begin(title.c_str(), &open_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::End();
    return;
  }
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
      ImGui::IsKeyPressed(ImGuiKey_S)) {
    save();
  }
  const float side = 250.0f, ledger = 270.0f;
  ImGui::BeginChild("##bb_side", ImVec2(side, 0), ImGuiChildFlags_Borders);
  draw_sidebar();
  ImGui::EndChild();
  ImGui::SameLine();
  const float canvas_w = std::max(200.0f, ImGui::GetContentRegionAvail().x - ledger - 8.0f);
  ImGui::BeginChild("##bb_canvas", ImVec2(canvas_w, 0), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
  draw_canvas();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##bb_ledger", ImVec2(0, 0), ImGuiChildFlags_Borders);
  draw_ledger();
  ImGui::EndChild();
  ImGui::End();
}

void BrandBook::draw_sidebar() {
  theme::BrandBlock("Brand Book", "visual scripts", ImGui::GetContentRegionAvail().x);
  theme::SectionHeader("Graphs", "scripts/*.graph.json");
  ImGui::BeginChild("##bb_list", ImVec2(0, 150), ImGuiChildFlags_Borders);
  if (graphs_.empty()) ImGui::TextDisabled("No graphs on this claim yet.");
  for (const std::string& g : graphs_) {
    if (ImGui::Selectable(g.c_str() + 8, g == rel_)) {
      if (g != rel_) load(g);
    }
  }
  ImGui::EndChild();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##bb_new", "new graph, e.g. old_well", new_name_, sizeof(new_name_));
  ImGui::BeginDisabled(new_name_[0] == '\0');
  if (theme::PrimaryButton("Stake New Graph", ImVec2(-1, 0))) {
    std::string stem;
    for (const char* p = new_name_; *p; ++p) {
      const char c = *p;
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') stem += c;
      else if (c == ' ') stem += '_';
    }
    if (!stem.empty()) {
      open_graph(project_dir_, project_name_, "scripts/" + stem + ".graph.json");
      new_name_[0] = '\0';
    }
  }
  ImGui::EndDisabled();
  ImGui::BeginDisabled(rel_.empty());
  if (theme::CopperButton(dirty_ ? "Save *  (Ctrl+S)" : "Save  (Ctrl+S)", ImVec2(-1, 0))) save();
  if (theme::SecondaryButton("Revert", ImVec2(-1, 0))) load(rel_);
  ImGui::EndDisabled();
  if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());

  theme::SectionHeader("Inspection", problems_.empty() ? "clean" : "problems");
  if (rel_.empty()) {
    ImGui::TextDisabled("Open or stake a graph.");
  } else if (problems_.empty()) {
    theme::StatusSuccess("Rides clean.");
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    for (const std::string& p : problems_) ImGui::TextWrapped("%s", p.c_str());
    ImGui::PopStyleColor();
  }
  draw_languages();
  theme::SectionHeader("Wrangling");
  theme::KeyHint("Right-click", "palette");
  theme::KeyHint("Drag pin", "wire");
  theme::KeyHint("Alt+click pin", "cut wires");
  theme::KeyHint("Middle / empty drag", "pan");
  theme::KeyHint("Wheel", "zoom");
  theme::KeyHint("Del", "remove node");
}

void BrandBook::draw_languages() {
  theme::SectionHeader("Languages", "pick by file extension");
  ImGui::TextColored(theme::Success(), "Lua");
  ImGui::SameLine(80);
  ImGui::TextDisabled(".lua  ready");
  ImGui::TextColored(theme::Success(), "Graph");
  ImGui::SameLine(80);
  ImGui::TextDisabled(".graph.json  ready");
  auto& st = language_status();
  if (st.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    ImGui::TextDisabled("C++ / C#: checking toolchains...");
    return;
  }
  const LanguageStatus& s = st.get();
  ImGui::TextColored(s.cpp_ok ? theme::Success() : theme::Warning(), "C++");
  ImGui::SameLine(80);
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled(".cpp  %s", s.cpp.c_str());
  ImGui::PopTextWrapPos();
  ImGui::TextColored(s.cs_ok ? theme::Success() : theme::Warning(), "C#");
  ImGui::SameLine(80);
  if (s.cs_ok) {
    ImGui::TextDisabled(".cs  %s", s.cs.c_str());
  } else {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::Warning(), "%s", s.cs.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) {
      ux::SetTooltip("C# scripts are disabled: %s", runtime::csharp_scripts::kInstallHint);
    }
  }
  const bool building = cpp_build_.valid();
  if (building && cpp_build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    cpp_status_ = cpp_build_.get();
  }
  ImGui::BeginDisabled(!s.cpp_ok || cpp_build_.valid() || project_dir_.empty() ||
                       !runtime::native_scripts::has_sources(project_dir_));
  if (theme::SecondaryButton(cpp_build_.valid() ? "Building C++ scripts..." : "Build C++ scripts", ImVec2(-1, 0))) {
    const std::string dir = project_dir_;
    cpp_build_ = std::async(std::launch::async, [dir] {
      const runtime::native_scripts::BuildResult b = runtime::native_scripts::build(dir, true);
      if (b.ok) return std::string("C++ module built: ") + fs::path(b.module).filename().string();
      std::string s = b.error;
      for (const runtime::LogEntry& p : b.problems) {
        s += "\n" + p.file + ":" + std::to_string(p.line) + " " + p.text;
      }
      return s;
    });
  }
  ImGui::EndDisabled();
  if (!cpp_status_.empty()) ImGui::TextWrapped("%s", cpp_status_.c_str());
}

void BrandBook::draw_canvas() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton("##bb_surface", size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  if (fit_pending_ && !graph_.nodes.empty() && size.x > 100 && size.y > 100) {
    fit_pending_ = false;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const GraphNode& n : graph_.nodes) {
      x0 = std::min(x0, n.x);
      y0 = std::min(y0, n.y);
      x1 = std::max(x1, n.x + kNodeW);
      y1 = std::max(y1, n.y + layout_of(n).h);
    }
    const float margin = 48.0f;
    zoom_ = std::clamp(std::min((size.x - 2 * margin) / std::max(1.0f, x1 - x0),
                                (size.y - 2 * margin - 30) / std::max(1.0f, y1 - y0)),
                       0.75f, 1.2f);
    pan_x_ = (size.x - (x1 - x0) * zoom_) * 0.5f - x0 * zoom_;
    pan_y_ = (size.y - 30 - (y1 - y0) * zoom_) * 0.5f - y0 * zoom_;
  }
  const float z = zoom_;
  auto to_screen = [&](float x, float y) { return ImVec2(origin.x + pan_x_ + x * z, origin.y + pan_y_ + y * z); };
  auto to_graph = [&](ImVec2 s) { return ImVec2((s.x - origin.x - pan_x_) / z, (s.y - origin.y - pan_y_) / z); };

  // Ground: canvas, survey grid, the headstone watermark.
  dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), theme::U32(theme::CanvasBg()));
  const float step = 24.0f * z;
  for (float x = std::fmod(pan_x_, step); x < size.x; x += step) {
    const bool major = static_cast<int>(std::round((x - pan_x_) / step)) % 5 == 0;
    dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y),
                theme::U32(theme::GridLine(), major ? 1.6f : 0.7f));
  }
  for (float y = std::fmod(pan_y_, step); y < size.y; y += step) {
    const bool major = static_cast<int>(std::round((y - pan_y_) / step)) % 5 == 0;
    dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y),
                theme::U32(theme::GridLine(), major ? 1.6f : 0.7f));
  }
  theme::TombstoneMark(dl, ImVec2(origin.x + size.x - 60, origin.y + size.y - 130), 100,
                       theme::U32(theme::Sand(), 0.06f), theme::U32(theme::Charcoal(), 0.10f));
  if (rel_.empty()) {
    theme::DrawSizedText(dl, ImVec2(origin.x + 30, origin.y + 30), 22, theme::U32(theme::TextMuted()),
                         "Open a graph or stake a new one.");
    dl->PopClipRect();
    return;
  }

  // Layout + pin positions.
  std::map<int, Layout> lay;
  for (const GraphNode& n : graph_.nodes) lay[n.id] = layout_of(n);
  auto pin_pos = [&](const GraphNode& n, const std::string& pin, bool output) {
    const Layout& l = lay[n.id];
    const auto& col = output ? l.right : l.left;
    for (std::size_t i = 0; i < col.size(); ++i) {
      if (col[i].first == pin) {
        return to_screen(n.x + (output ? kNodeW : 0.0f), n.y + kHeaderH + kRowH * (static_cast<float>(i) + 0.5f) + 4);
      }
    }
    return to_screen(n.x, n.y + kHeaderH * 0.5f);
  };

  // Pins under the mouse.
  Pin hover_pin;
  bool on_pin = false;
  int hover_node = 0;
  bool on_header = false;
  const float hit = (kPinR + 4.0f) * z;
  for (const GraphNode& n : graph_.nodes) {
    const Layout& l = lay[n.id];
    for (int side = 0; side < 2; ++side) {
      const auto& col = side ? l.right : l.left;
      for (const auto& [name, exec] : col) {
        const ImVec2 p = pin_pos(n, name, side == 1);
        if (std::fabs(io.MousePos.x - p.x) < hit && std::fabs(io.MousePos.y - p.y) < hit) {
          hover_pin = {n.id, name, side == 1, exec};
          on_pin = true;
        }
      }
    }
    const ImVec2 a = to_screen(n.x, n.y), b = to_screen(n.x + kNodeW, n.y + l.h);
    if (io.MousePos.x >= a.x && io.MousePos.x <= b.x && io.MousePos.y >= a.y && io.MousePos.y <= b.y) {
      hover_node = n.id;
      on_header = io.MousePos.y <= a.y + kHeaderH * z;
    }
  }

  // Input.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (on_pin && io.KeyAlt) {
      graph_.links.erase(std::remove_if(graph_.links.begin(), graph_.links.end(),
                                        [&](const GraphLink& l) {
                                          return hover_pin.output ? (l.from_node == hover_pin.node && l.from_pin == hover_pin.name)
                                                                  : (l.to_node == hover_pin.node && l.to_pin == hover_pin.name);
                                        }),
                         graph_.links.end());
      dirty_ = true;
      recheck();
    } else if (on_pin) {
      linking_ = true;
      link_from_ = hover_pin;
    } else if (hover_node) {
      selected_ = hover_node;
      dragging_ = hover_node;
      (void)on_header;
    } else {
      selected_ = 0;
      dragging_ = -1;  // pan
    }
  }
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragging_ > 0) {
    if (GraphNode* n = graph_.node(dragging_); n && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
      n->x += io.MouseDelta.x / z;
      n->y += io.MouseDelta.y / z;
      dirty_ = true;
    }
  }
  if ((ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragging_ == -1) ||
      (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
    pan_x_ += io.MouseDelta.x;
    pan_y_ += io.MouseDelta.y;
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (dragging_ > 0) {
      if (GraphNode* n = graph_.node(dragging_)) {
        n->x = std::round(n->x / 10.0f) * 10.0f;
        n->y = std::round(n->y / 10.0f) * 10.0f;
      }
    }
    dragging_ = 0;
    if (linking_ && on_pin) connect(link_from_, hover_pin);
    linking_ = false;
  }
  if (hovered && io.MouseWheel != 0.0f) {
    const ImVec2 before = to_graph(io.MousePos);
    zoom_ = std::clamp(zoom_ * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.4f, 1.8f);
    pan_x_ = io.MousePos.x - origin.x - before.x * zoom_;
    pan_y_ = io.MousePos.y - origin.y - before.y * zoom_;
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const ImVec2 g = to_graph(io.MousePos);
    palette_x_ = g.x;
    palette_y_ = g.y;
    palette_filter_[0] = '\0';
    ImGui::OpenPopup("##bb_palette");
  }
  if (ImGui::IsWindowFocused() && selected_ && ImGui::IsKeyPressed(ImGuiKey_Delete)) delete_node(selected_);
  const float zz = zoom_;  // (zoom may have changed: redraw with the new one next frame)
  (void)zz;

  // Wires.
  for (const GraphLink& l : graph_.links) {
    const GraphNode* a = graph_.node(l.from_node);
    const GraphNode* b = graph_.node(l.to_node);
    if (!a || !b) continue;
    const ImVec2 p0 = pin_pos(*a, l.from_pin, true), p1 = pin_pos(*b, l.to_pin, false);
    const bool exec = is_exec_out(graph_, l.from_node, l.from_pin);
    const float dx = std::max(40.0f * z, std::fabs(p1.x - p0.x) * 0.5f);
    ImU32 col = exec ? theme::U32(theme::Text(), 0.85f) : theme::U32(theme::Sand(), 0.85f);
    if (!exec) {
      const GraphNodeDef* d = runtime::find_graph_node(b->type);
      if (d) {
        for (const auto& p : d->data_ins) if (p.name == l.to_pin) col = theme::U32(value_color(p.def), 0.9f);
      }
    }
    if (selected_ && (l.from_node == selected_ || l.to_node == selected_)) col = theme::U32(theme::Accent());
    dl->AddBezierCubic(p0, ImVec2(p0.x + dx, p0.y), ImVec2(p1.x - dx, p1.y), p1, col, (exec ? 3.2f : 2.0f) * z);
  }
  if (linking_) {
    const GraphNode* a = graph_.node(link_from_.node);
    if (a) {
      const ImVec2 p0 = pin_pos(*a, link_from_.name, link_from_.output);
      const ImVec2 p1 = io.MousePos;
      const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.5f) * (link_from_.output ? 1.0f : -1.0f);
      dl->AddBezierCubic(p0, ImVec2(p0.x + dx, p0.y), ImVec2(p1.x - dx, p1.y), p1, theme::U32(theme::Accent()), 2.5f);
    }
  }

  // Node cards.
  for (const GraphNode& n : graph_.nodes) {
    const Layout& l = lay[n.id];
    const std::string cat = l.def ? l.def->category : "Unknown";
    const ImVec4 fam = l.def ? family_color(cat) : theme::Danger();
    const ImVec2 a = to_screen(n.x, n.y), b = to_screen(n.x + kNodeW, n.y + l.h);
    const float r = 6.0f * z;
    dl->AddRectFilled(ImVec2(a.x + 4 * z, a.y + 5 * z), ImVec2(b.x + 4 * z, b.y + 5 * z),
                      IM_COL32(0, 0, 0, 70), r);  // shadow
    dl->AddRectFilled(a, b, theme::U32(theme::PanelBg()), r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + kHeaderH * z), theme::U32(fam), r, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(a.x, a.y + kHeaderH * z - 3 * z), ImVec2(b.x, a.y + kHeaderH * z),
                      theme::U32(theme::Charcoal(), 0.35f));  // header rule
    const bool sel = n.id == selected_;
    dl->AddRect(a, b, sel ? theme::U32(theme::Accent()) : theme::U32(theme::Border()), r, 0, sel ? 2.5f : 1.0f);
    const std::string title = l.def ? l.def->title : n.type;
    theme::DrawSizedText(dl, ImVec2(a.x + 9 * z, a.y + 7 * z), 15.0f * z, theme::U32(theme::TextOnAccent()),
                         title.c_str());
    const std::string fam_label = upper(cat);
    const ImVec2 fl = theme::MeasureText(9.5f * z, fam_label.c_str());
    theme::DrawSizedText(dl, ImVec2(b.x - fl.x - 8 * z, a.y + 10 * z), 9.5f * z,
                         theme::U32(theme::TextOnAccent(), 0.65f), fam_label.c_str());
    for (int side = 0; side < 2; ++side) {
      const auto& col = side ? l.right : l.left;
      for (const auto& [name, exec] : col) {
        const ImVec2 p = pin_pos(n, name, side == 1);
        bool linked = false;
        for (const GraphLink& k : graph_.links) {
          linked = linked || (side ? (k.from_node == n.id && k.from_pin == name) : (k.to_node == n.id && k.to_pin == name));
        }
        ImU32 pc = theme::U32(theme::Text(), 0.9f);
        if (!exec && l.def && side == 0) {
          for (const auto& d : l.def->data_ins) if (d.name == name) pc = theme::U32(value_color(d.def));
        } else if (!exec) {
          pc = theme::U32(theme::Sand());
        }
        const bool hot = on_pin && hover_pin.node == n.id && hover_pin.name == name && hover_pin.output == (side == 1);
        if (exec) {
          const float s = (kPinR + 1.5f) * z;
          const ImVec2 t0(p.x - s * 0.6f, p.y - s), t1(p.x - s * 0.6f, p.y + s), t2(p.x + s * 0.8f, p.y);
          if (linked || hot) dl->AddTriangleFilled(t0, t1, t2, pc);
          else dl->AddTriangle(t0, t1, t2, pc, 1.6f * z);
        } else if (linked || hot) {
          dl->AddCircleFilled(p, kPinR * z, pc);
        } else {
          dl->AddCircle(p, kPinR * z, pc, 0, 1.6f * z);
        }
        // Label (+ the constant on unlinked inputs).
        std::string label = exec && name == "in" ? std::string() : name;
        if (side == 0 && !exec && !linked) {
          const auto it = n.inputs.find(name);
          ScriptValue v;
          bool any = false;
          if (it != n.inputs.end()) {
            v = it->second;
            any = true;
          } else if (l.def) {
            for (const auto& d : l.def->data_ins) if (d.name == name) { v = d.def; any = true; }
          }
          if (any) {
            std::string shown = describe(v);
            if (v.type == ScriptValue::Type::Text) shown = "\"" + shown + "\"";
            if (shown.size() > 18) shown = shown.substr(0, 16) + "..";
            label += "  " + shown;
          }
        }
        const float px = 12.5f * z;
        if (side == 0) {
          theme::DrawSizedText(dl, ImVec2(p.x + 10 * z, p.y - px * 0.55f), px, theme::U32(theme::Text(), 0.85f),
                               label.c_str());
        } else {
          const ImVec2 m = theme::MeasureText(px, label.c_str());
          theme::DrawSizedText(dl, ImVec2(p.x - 10 * z - m.x, p.y - px * 0.55f), px,
                               theme::U32(theme::Text(), 0.85f), label.c_str());
        }
      }
    }
  }
  // Brand strip.
  char info[160];
  std::snprintf(info, sizeof(info), "%s  |  %zu nodes  %zu wires  |  zoom %d%%", rel_.c_str(), graph_.nodes.size(),
                graph_.links.size(), static_cast<int>(std::round(zoom_ * 100)));
  theme::DrawSizedText(dl, ImVec2(origin.x + 12, origin.y + size.y - 24), 13, theme::U32(theme::TextMuted()), info);
  dl->PopClipRect();
  draw_palette_popup();
}

void BrandBook::draw_palette_popup() {
  if (!ImGui::BeginPopup("##bb_palette")) return;
  theme::SectionHeader("Palette", "every node family");
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(260);
  ImGui::InputTextWithHint("##bb_filter", "search nodes", palette_filter_, sizeof(palette_filter_));
  const std::string filter = palette_filter_;
  auto match = [&](const GraphNodeDef& d) {
    if (filter.empty()) return true;
    std::string hay = d.title + " " + d.type + " " + d.category;
    std::string f = filter;
    for (char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hay.find(f) != std::string::npos;
  };
  std::vector<std::string> cats;
  for (const GraphNodeDef& d : runtime::graph_node_catalog()) {
    if (std::find(cats.begin(), cats.end(), d.category) == cats.end()) cats.push_back(d.category);
  }
  for (const std::string& c : cats) {
    bool any = false;
    for (const GraphNodeDef& d : runtime::graph_node_catalog()) any = any || (d.category == c && match(d));
    if (!any) continue;
    ImGui::PushStyleColor(ImGuiCol_Text, family_color(c));
    const bool open = filter.empty() ? ux::BeginMenu(c.c_str()) : true;
    ImGui::PopStyleColor();
    if (!open) continue;
    for (const GraphNodeDef& d : runtime::graph_node_catalog()) {
      if (d.category != c || !match(d)) continue;
      if (ux::MenuItem(d.title.c_str(), filter.empty() ? nullptr : c.c_str())) {
        selected_ = add_node(d.type, palette_x_, palette_y_);
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::IsItemHovered()) ux::SetTooltip("%s\n%s", d.type.c_str(), d.blurb.c_str());
    }
    if (filter.empty()) ux::EndMenu();
  }
  ImGui::EndPopup();
}

void BrandBook::draw_ledger() {
  theme::SectionHeader("Ledger", selected_ ? "selected node" : "graph");
  GraphNode* n = selected_ ? graph_.node(selected_) : nullptr;
  if (!n) {
    if (rel_.empty()) return;
    ImGui::TextWrapped("%zu nodes, %zu wires, %zu variables, %zu props.", graph_.nodes.size(), graph_.links.size(),
                       graph_.variables.size(), graph_.props.size());
    theme::SectionHeader("Variables", "per instance, kept on reload");
    for (const ScriptProp& v : graph_.variables) ImGui::BulletText("%s = %s", v.name.c_str(), describe(v.value).c_str());
    theme::SectionHeader("Props", "Inspector overrides");
    for (const ScriptProp& v : graph_.props) ImGui::BulletText("%s = %s", v.name.c_str(), describe(v.value).c_str());
    ImGui::TextDisabled("Select a node to set its constants.");
    return;
  }
  const GraphNodeDef* d = runtime::find_graph_node(n->type);
  ImGui::PushStyleColor(ImGuiCol_Text, d ? family_color(d->category) : theme::Danger());
  ImGui::TextUnformatted(d ? d->title.c_str() : n->type.c_str());
  ImGui::PopStyleColor();
  ImGui::TextDisabled("#%d  %s", n->id, n->type.c_str());
  if (d) ImGui::TextWrapped("%s", d->blurb.c_str());
  theme::Divider();
  if (d) {
    for (const auto& p : d->data_ins) {
      bool linked = false;
      for (const GraphLink& l : graph_.links) linked = linked || (l.to_node == n->id && l.to_pin == p.name);
      ImGui::PushID(p.name.c_str());
      if (linked) {
        ImGui::TextDisabled("%s  (wired)", p.name.c_str());
        ImGui::PopID();
        continue;
      }
      ScriptValue v = p.def;
      if (auto it = n->inputs.find(p.name); it != n->inputs.end()) v = it->second;
      bool changed = false;
      ImGui::SetNextItemWidth(-90);
      switch (p.def.type) {
        case ScriptValue::Type::Bool: {
          bool b = v.type == ScriptValue::Type::Bool ? v.flag : v.number != 0;
          if (ImGui::Checkbox(p.name.c_str(), &b)) { v = ScriptValue::of_bool(b); changed = true; }
          break;
        }
        case ScriptValue::Type::Number: {
          double x = v.number;
          if (ImGui::DragScalar(p.name.c_str(), ImGuiDataType_Double, &x, 0.1f, nullptr, nullptr, "%.6g")) {
            v = ScriptValue::of_number(x);
            changed = true;
          }
          break;
        }
        case ScriptValue::Type::Text: {
          char buf[256];
          std::snprintf(buf, sizeof(buf), "%s", v.type == ScriptValue::Type::Text ? v.text.c_str() : describe(v).c_str());
          if (ImGui::InputText(p.name.c_str(), buf, sizeof(buf))) { v = ScriptValue::of_text(buf); changed = true; }
          break;
        }
      }
      if (changed) {
        n->inputs[p.name] = v;
        dirty_ = true;
      }
      ImGui::PopID();
    }
  }
  theme::Divider();
  if (theme::DangerButton("Remove node", ImVec2(-1, 0))) delete_node(n->id);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
