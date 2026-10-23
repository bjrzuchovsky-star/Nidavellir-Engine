#pragma once

// Readable errors. Every failure the editor shows a person goes through
// explain(): it sorts the raw message into a kind and answers three
// questions in plain words (what happened, why it usually happens, what to
// do now), keeping the raw text as "details" for a bug report.
//
//   ux::show_error(ux::explain(err, ux::ErrorKind::ImportFailed));  // modal
//   ux::ErrorBox("vault_err", ux::explain(error_, ux::ErrorKind::VaultConnection));  // inline
//
// Telegraph lines use friendly_line() for their hover card.

#include <functional>
#include <string>
#include <vector>

namespace ts::tombstone::editor::ux {

enum class ErrorKind {
  Unknown,
  MissingFile,
  MissingTexture,
  MissingModel,
  ImportFailed,
  ScriptCompile,
  ScriptRuntime,
  PluginMissing,
  ExportFailed,
  VaultConnection,
  BlenderNotFound,
  UpdateCheck,
  SceneVersion,
  SaveFailed,
};

const char* to_string(ErrorKind k);
std::vector<ErrorKind> all_error_kinds();  // every kind but Unknown

struct FriendlyError {
  ErrorKind kind = ErrorKind::Unknown;
  std::string title;    // "Blender wasn't found"
  std::string what;     // what happened, naming the file when there is one
  std::string why;      // the usual reason
  std::string fix;      // what to do now
  std::string details;  // the raw message (+ context), for Copy details
  std::string subject;  // file / plugin / address pulled out of the message
  // Optional one-click fix: a palette action id ("Settings/Tools") and its
  // button label. Empty = no button.
  std::string fix_action;
  std::string fix_label;

  // "What happened. Why. What to do." as one block (Telegraph hover, logs).
  std::string summary() const;
  // Everything, for the clipboard.
  std::string copy_text() const;
};

// Sort a raw message. `hint` is what the caller was doing; it decides when
// the message alone is too generic ("could not open x").
ErrorKind classify(const std::string& raw, ErrorKind hint = ErrorKind::Unknown);
FriendlyError explain(const std::string& raw, ErrorKind hint = ErrorKind::Unknown,
                      const std::string& context = {});

// Modal: queued, drawn by draw_error_modal() (EditorUx::frame() and the
// Project Manager call it). run_action runs fix_action; null hides the fix
// button.
void show_error(FriendlyError e);
void draw_error_modal(const std::function<bool(const std::string&)>& run_action = {});
bool error_modal_pending();
const FriendlyError* current_error();  // the one on screen / next up (smoke)
void clear_errors();

// Inline card for panels that keep their error on screen (Vault, Trading
// Post, update banner): title, what + fix, a Details expander and Copy
// details. Returns true when the fix button was pressed (caller acts).
bool ErrorBox(const char* id, const FriendlyError& e, bool show_fix_button = true);

// Telegraph: draw the hover card for one line (call after the line's item).
void FriendlyLineTooltip(const FriendlyError& e);

// Telegraph line helpers, shared by both editors.
//   const std::string label = ux::telegraph_label(text, problem, hint);
//   ImGui::Selectable(label.c_str());
//   ux::TelegraphLineHelp(text, problem, hint, context);
// `problem` = warning or error. A problem line gets a short plain-words
// tag ("-- a script stopped"), a hover card (what / why / what to do) and a
// right-click menu: Explain..., Copy line, Copy details.
ErrorKind telegraph_hint(const std::string& channel);
const char* short_title(ErrorKind k);  // "" for Unknown
std::string telegraph_label(const std::string& text, bool problem, ErrorKind hint);
// Returns true when the line's hover card was drawn (callers skip their own
// tooltip then).
bool TelegraphLineHelp(const std::string& text, bool problem, ErrorKind hint, const std::string& context = {});

}  // namespace ts::tombstone::editor::ux
