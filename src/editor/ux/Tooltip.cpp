#include "editor/ux/Tooltip.h"

#include "editor/ux/Actions.h"
#include "editor/ux/ImGuiItemHook.h"
#include "editor/ux/UxPrefs.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ts::tombstone::editor::ux {

namespace {

struct Scope {
  std::string name;
  std::string section;
  ImGuiWindow* root = nullptr;
};

std::vector<Scope> g_scopes;
bool g_audit = false;
std::vector<SeenItem> g_seen;
std::set<std::string> g_seen_keys;

// The hovered widget seen this frame inside a scope.
ImGuiID g_hover_id = 0;
std::string g_hover_tip;
// Hover timing for auto tips (our own: the scope sees the item after it is
// submitted, so ImGui's per-item delay is not available here).
struct Timer {
  ImGuiID id = 0;
  double start = 0.0;
};
std::map<std::string, Timer> g_timers;  // per scope
int g_explicit_frame = -1;  // a hand-written tooltip went up this frame

void wrapped_tooltip(const char* text) {
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void sync_style() {
  ImGui::GetStyle().HoverDelayNormal = prefs().tooltip_delay;
  ImGui::GetStyle().HoverDelayShort = prefs().tooltip_delay * 0.5f;
}

// Multi-part widgets (DragFloat3, ColorEdit4) report "" / "##X" for their
// parts; the label they were given was pushed on the ID stack, so match it
// against the dictionary keys one or two levels up.
std::string parent_label(ImGuiWindow* w) {
  const int n = w->IDStack.Size;
  for (int depth = 1; depth <= 2; ++depth) {
    if (n - depth - 1 < 0) break;
    const ImGuiID want = w->IDStack[n - depth];
    const ImGuiID seed = w->IDStack[n - depth - 1];
    for (const std::string& k : dictionary_labels()) {
      if (ImHashStr(k.c_str(), 0, seed) == want) return label_key(k.c_str());
      if (k[0] == '#' && ImHashStr(("#" + k).c_str(), 0, seed) == want) return k;
    }
  }
  return {};
}

bool inner_part(const char* label) {
  static const char* kParts[] = {"##X", "##Y", "##Z", "##W", "##ColorButton", "##picker"};
  for (const char* p : kParts) {
    if (std::strcmp(label, p) == 0) return true;
  }
  return false;
}

void on_item(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
  menu_audit_item(ctx, id, label, flags);
  if (g_scopes.empty() || !label) return;
  ImGuiContext& g = *ctx;
  ImGuiWindow* w = g.CurrentWindow;
  if (!w || id == w->ID) return;  // the window registering itself
  Scope& s = g_scopes.back();
  if (w->RootWindow != s.root) return;  // popups and other windows
  std::string key;
  if (label[0] == '\0' || inner_part(label)) {
    key = parent_label(w);
    if (key.empty()) key = label[0] ? label_key(label) : "<unlabelled>";
  } else {
    key = label_key(label);
  }
  if (key.empty()) return;
  if (flags & ImGuiItemStatusFlags_Openable) {
    // A component header: it names the section the fields under it live in.
    s.section = key;
    key = "Header/" + key;
  }
  const char* tip = lookup(s.section, key);
  if (g_audit) {
    const std::string sig = s.name + "|" + s.section + "|" + key;
    if (g_seen_keys.insert(sig).second) g_seen.push_back(SeenItem{s.name, s.section, key, tip != nullptr});
  }
  if (tip && (flags & ImGuiItemStatusFlags_HoveredRect) && g.HoveredWindow == w) {
    g_hover_id = id;
    g_hover_tip = tip;
  }
}

}  // namespace

bool TooltipsOn() { return prefs().tooltips; }

void SetTooltip(const char* fmt, ...) {
  if (!TooltipsOn()) return;
  sync_style();
  // When the caller's condition is the last item being hovered, honour the
  // delay; other callers (hover over a canvas region) show straight away.
  if (prefs().tooltip_delay > 0.0f &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
      !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal |
                            ImGuiHoveredFlags_NoSharedDelay)) {
    g_explicit_frame = ImGui::GetFrameCount();  // keep the auto tip away too
    return;
  }
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  g_explicit_frame = ImGui::GetFrameCount();
  wrapped_tooltip(buf);
}

void Tip(const char* key) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
  const char* t = lookup("", key);
  SetTooltip("%s", t ? t : key);
}

void TipText(const std::string& text) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
  SetTooltip("%s", text.c_str());
}

std::string label_key(const char* label) {
  if (!label) return {};
  std::string s(label);
  const std::size_t triple = s.find("###");
  if (triple != std::string::npos) return "#" + s.substr(triple + 3);
  const std::size_t dbl = s.find("##");
  if (dbl != std::string::npos) {
    std::string vis = s.substr(0, dbl);
    while (!vis.empty() && vis.back() == ' ') vis.pop_back();
    if (!vis.empty()) return vis;
    return "#" + s.substr(dbl + 2);
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

void install_hook() {
  g_ts_imgui_item_info_hook = &on_item;
  if (ImGuiContext* c = ImGui::GetCurrentContext()) c->TestEngineHookItems = true;
}

void BeginAutoTips(const char* scope) {
  install_hook();
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  g_scopes.push_back(Scope{scope ? scope : "", "", w ? w->RootWindow : nullptr});
  g_hover_id = 0;
  g_hover_tip.clear();
}

void AutoTipSection(const char* section) {
  if (!g_scopes.empty()) g_scopes.back().section = section ? section : "";
}

void EndAutoTips() {
  if (g_scopes.empty()) return;
  const std::string name = g_scopes.back().name;
  g_scopes.pop_back();
  if (!g_scopes.empty()) return;
  const ImGuiID id = g_hover_id;
  g_hover_id = 0;
  Timer& t = g_timers[name];
  if (id == 0 || !TooltipsOn() || ImGui::IsAnyItemActive() ||
      g_explicit_frame == ImGui::GetFrameCount()) {
    if (id == 0) t.id = 0;
    return;
  }
  const double now = ImGui::GetTime();
  if (id != t.id) {
    t.id = id;
    t.start = now;
  }
  if (now - t.start + 1e-6 >= prefs().tooltip_delay) wrapped_tooltip(g_hover_tip.c_str());
}

void set_audit(bool on) { g_audit = on; }
void clear_audit() {
  g_seen.clear();
  g_seen_keys.clear();
}
const std::vector<SeenItem>& audit_items() { return g_seen; }

const char* lookup(const std::string& section, const std::string& key) {
  if (!section.empty()) {
    if (const char* t = dictionary_text(section + "/" + key)) return t;
  }
  return dictionary_text(key);
}

}  // namespace ts::tombstone::editor::ux
