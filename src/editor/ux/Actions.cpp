#include "editor/ux/Actions.h"

#include "editor/ux/Tooltip.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>

namespace ts::tombstone::editor::ux {

struct HarvestAccess {
  static void push(ActionRegistry* r, Action a) { r->harvested_[a.scope].push_back(std::move(a)); }
  static void unregistered(ActionRegistry* r, const std::string& scope, const std::string& label) {
    r->unregistered_[scope].push_back(label);
  }
  static void clear(ActionRegistry* r, const std::string& scope) {
    r->harvested_[scope].clear();
    r->unregistered_[scope].clear();
  }
};

namespace {

ActionRegistry* g_reg = nullptr;  // registry harvesting right now
std::string g_scope = "menu";
std::string g_invoke;
std::vector<std::string> g_path;
std::vector<bool> g_disabled;
std::vector<std::string> g_ctx_scopes;
int g_in_item = 0;  // > 0 inside BeginMenuOptions: option widgets, not actions
ImGuiWindow* g_host = nullptr;
std::map<std::string, int> g_ids;  // duplicate id guard per harvest

std::string join_path() {
  std::string out;
  for (const std::string& p : g_path) {
    if (!out.empty()) out += '/';
    out += p;
  }
  return out;
}

std::string current_scope() { return g_ctx_scopes.empty() ? g_scope : g_ctx_scopes.back(); }

bool currently_disabled() {
  if (!g_disabled.empty() && g_disabled.back()) return true;
  ImGuiContext* c = ImGui::GetCurrentContext();
  return c && (c->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
}

std::string clean_label(const char* label) {
  std::string k = label_key(label);
  if (k.rfind('#', 0) == 0) k = k.substr(1);
  return k;
}

void menu_tip(const char* label, const char* shortcut) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
  const char* t = lookup("Menu", clean_label(label));
  std::string text = t ? t : "";
  if (shortcut && *shortcut) {
    if (!text.empty()) text += "\n";
    text += "Shortcut: ";
    text += shortcut;
  }
  if (!text.empty()) SetTooltip("%s", text.c_str());
}

bool record(const char* label, const char* shortcut, bool checkable, bool checked, bool enabled) {
  Action a;
  std::string key = clean_label(label);
  a.label = key;
  const std::string raw(label ? label : "");
  const std::size_t triple = raw.find("###");
  if (triple != std::string::npos && triple > 0) a.label = raw.substr(0, triple);  // "Undo Move###EditUndo"
  a.path = join_path();
  a.scope = current_scope();
  std::string id = a.path.empty() ? key : a.path + "/" + key;
  const int n = ++g_ids[a.scope + "|" + id];
  if (n > 1) id += "#" + std::to_string(n);
  a.id = id;
  a.shortcut = shortcut ? shortcut : "";
  a.checkable = checkable;
  a.checked = checked;
  a.enabled = enabled && !currently_disabled();
  if (const char* t = lookup("Menu", key)) a.tip = t;
  const bool fire = a.enabled && !g_invoke.empty() && a.id == g_invoke;
  if (fire) {
    g_reg->note_ran(a.id);
    g_invoke.clear();
  }
  HarvestAccess::push(g_reg, std::move(a));
  return fire;
}

bool key_named(const std::string& name, ImGuiKey* out) {
  if (name.size() == 1) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (c >= 'A' && c <= 'Z') {
      *out = static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
      return true;
    }
    if (c >= '0' && c <= '9') {
      *out = static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
      return true;
    }
  }
  if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
    const int f = std::atoi(name.c_str() + 1);
    if (f >= 1 && f <= 12) {
      *out = static_cast<ImGuiKey>(ImGuiKey_F1 + (f - 1));
      return true;
    }
  }
  if (name == "/") *out = ImGuiKey_Slash;
  else if (name == "Del" || name == "Delete") *out = ImGuiKey_Delete;
  else if (name == "Esc") *out = ImGuiKey_Escape;
  else if (name == "Enter") *out = ImGuiKey_Enter;
  else if (name == "Tab") *out = ImGuiKey_Tab;
  else if (name == ",") *out = ImGuiKey_Comma;
  else return false;
  return true;
}

}  // namespace

// ---- registry -----------------------------------------------------------------

void ActionRegistry::add(Action a) {
  if (a.scope == "menu") a.scope = "palette";
  for (Action& s : statics_) {
    if (s.id == a.id) {
      s = std::move(a);
      return;
    }
  }
  statics_.push_back(std::move(a));
}

void ActionRegistry::remove_prefix(const std::string& p) {
  statics_.erase(std::remove_if(statics_.begin(), statics_.end(),
                                [&](const Action& a) { return a.id.rfind(p, 0) == 0; }),
                 statics_.end());
}

const Action* ActionRegistry::find_static(const std::string& id) const {
  for (const Action& a : statics_) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

void ActionRegistry::begin_harvest(const std::string& scope, const std::string& invoke_id) {
  harvesting_ = true;
  g_reg = this;
  g_scope = scope;
  g_invoke = invoke_id;
  g_path.clear();
  g_disabled.clear();
  g_ctx_scopes.clear();
  g_ids.clear();
  HarvestAccess::clear(this, scope);
}

void ActionRegistry::end_harvest() {
  harvesting_ = false;
  g_reg = nullptr;
  g_invoke.clear();
  g_path.clear();
  g_disabled.clear();
  g_ctx_scopes.clear();
}

const std::vector<Action>& ActionRegistry::harvested(const std::string& scope) const {
  static const std::vector<Action> kNone;
  auto it = harvested_.find(scope);
  return it == harvested_.end() ? kNone : it->second;
}

const std::vector<std::string>& ActionRegistry::unregistered(const std::string& scope) const {
  static const std::vector<std::string> kNone;
  auto it = unregistered_.find(scope);
  return it == unregistered_.end() ? kNone : it->second;
}

std::vector<Action> ActionRegistry::palette_actions() const {
  std::vector<Action> out = harvested("menu");
  for (const Action& s : statics_) {
    Action a = s;
    a.enabled = !s.is_enabled || s.is_enabled();
    a.run = nullptr;
    out.push_back(std::move(a));
  }
  return out;
}

bool ActionRegistry::run(const std::string& id) {
  for (const Action& s : statics_) {
    if (s.id != id) continue;
    if ((s.is_enabled && !s.is_enabled()) || !s.run) return false;
    ++runs_[id];
    const std::function<void()> fn = s.run;  // run() may re-register statics
    fn();
    return true;
  }
  for (const Action& a : harvested("menu")) {
    if (a.id == id) {
      if (!a.enabled) return false;
      pending_ = id;
      return true;
    }
  }
  return false;
}

std::string ActionRegistry::take_pending() {
  std::string p;
  p.swap(pending_);
  return p;
}

int ActionRegistry::run_count(const std::string& id) const {
  auto it = runs_.find(id);
  return it == runs_.end() ? 0 : it->second;
}

void ActionRegistry::handle_shortcuts() {
  for (const Action& s : statics_) {
    if (s.shortcut.empty() || !s.run) continue;
    if (ChordPressed(s.shortcut)) run(s.id);
  }
}

bool ChordPressed(const std::string& chord) {
  ImGuiContext* c = ImGui::GetCurrentContext();
  if (!c) return false;
  const ImGuiIO& io = ImGui::GetIO();
  bool ctrl = false, shift = false, alt = false;
  std::string key;
  std::size_t start = 0;
  while (start <= chord.size()) {
    std::size_t plus = chord.find('+', start);
    if (plus == start) plus = chord.find('+', start + 1);  // "Ctrl++"
    std::string part = chord.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
    if (part == "Ctrl") ctrl = true;
    else if (part == "Shift") shift = true;
    else if (part == "Alt") alt = true;
    else key = part;
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  ImGuiKey k = ImGuiKey_None;
  if (key.empty() || !key_named(key, &k)) return false;
  if (io.KeyCtrl != ctrl || io.KeyShift != shift || io.KeyAlt != alt) return false;
  return ImGui::IsKeyPressed(k, false);
}

// ---- wrappers -----------------------------------------------------------------

bool BeginMenu(const char* label, bool enabled) {
  if (g_reg) {
    g_path.push_back(clean_label(label));
    g_disabled.push_back(!enabled || currently_disabled());
    return true;
  }
  return ImGui::BeginMenu(label, enabled);
}

void EndMenu() {
  if (g_reg) {
    if (!g_path.empty()) g_path.pop_back();
    if (!g_disabled.empty()) g_disabled.pop_back();
    return;
  }
  ImGui::EndMenu();
}

bool MenuItem(const char* label, const char* shortcut, bool selected, bool enabled) {
  if (g_reg) return record(label, shortcut, selected, selected, enabled);
  const bool r = ImGui::MenuItem(label, shortcut, selected, enabled);
  menu_tip(label, shortcut);
  return r;
}

bool MenuItem(const char* label, const char* shortcut, bool* p_selected, bool enabled) {
  if (g_reg) {
    const bool fire = record(label, shortcut, true, p_selected && *p_selected, enabled);
    if (fire && p_selected) *p_selected = !*p_selected;
    return fire;
  }
  const bool r = ImGui::MenuItem(label, shortcut, p_selected, enabled);
  menu_tip(label, shortcut);
  return r;
}

void BeginContextScope(const char* scope) {
  if (g_reg) {
    g_ctx_scopes.push_back(scope);
    HarvestAccess::clear(g_reg, scope);
  }
}

void EndContextScope() {
  if (g_reg && !g_ctx_scopes.empty()) g_ctx_scopes.pop_back();
}

void BeginHarvestWindow() {
  ImGui::SetNextWindowPos(ImVec2(-20000.0f, -20000.0f));
  ImGui::SetNextWindowSize(ImVec2(600.0f, 600.0f));
  ImGui::Begin("##ux_harvest", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
  g_host = ImGui::GetCurrentWindow();
  install_hook();
}

void EndHarvestWindow() {
  g_host = nullptr;
  ImGui::End();
}

void BeginMenuOptions() { ++g_in_item; }
void EndMenuOptions() {
  if (g_in_item > 0) --g_in_item;
}

void menu_audit_item(ImGuiContext* ctx, unsigned int id, const char* label, int) {
  if (!g_reg || !g_host || g_in_item > 0 || !label) return;
  ImGuiWindow* w = ctx->CurrentWindow;
  if (!w || w->RootWindow != g_host || id == w->ID) return;
  HarvestAccess::unregistered(g_reg, current_scope(), label);
}

}  // namespace ts::tombstone::editor::ux
