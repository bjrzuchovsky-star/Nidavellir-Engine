#pragma once

// Field Guide tooltips. Every editor tooltip goes through ux::SetTooltip so
// Settings > Field Guide can switch them off or slow them down, and the
// text for toolbar buttons and Inspector fields comes from one dictionary
// (UxStrings.cpp) keyed by the widget's label, so a field added later only
// needs a dictionary line.
//
//   ux::BeginAutoTips("Inspector");   // every labelled widget drawn in this
//   ...draw fields...                 // window (not its popups) looks its
//   ux::EndAutoTips();                // label up and shows the tip on hover
//
// --smoke-ux turns on the audit to list every widget a scope drew and check
// each one has text.

#include <string>
#include <vector>

namespace ts::tombstone::editor::ux {

bool TooltipsOn();
// Wrapped, delay-aware ImGui::SetTooltip. Call it as you would ImGui's
// (after your own IsItemHovered() check).
void SetTooltip(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
// Tooltip for the last item: dictionary text by key ("Toolbar2D/Undo").
void Tip(const char* key);
// Tooltip for the last item with literal text (dynamic tips).
void TipText(const std::string& text);

// Label -> dictionary key: "Grid##tb" -> "Grid", "##grid_size" -> "#grid_size",
// "Telegraph: 2 errors###tg_badge" -> "#tg_badge".
std::string label_key(const char* label);

void BeginAutoTips(const char* scope);
void AutoTipSection(const char* section);  // e.g. the component being drawn
void EndAutoTips();

// The item hook (installed once per ImGui context: sets TestEngineHookItems).
void install_hook();

// Audit (smoke): what each scope drew and whether it had text.
struct SeenItem {
  std::string scope;
  std::string section;
  std::string key;
  bool has_tip = false;
};
void set_audit(bool on);
void clear_audit();
const std::vector<SeenItem>& audit_items();

// Text for a key in a section (section/key first, then key). nullptr = none.
const char* lookup(const std::string& section, const std::string& key);

}  // namespace ts::tombstone::editor::ux
