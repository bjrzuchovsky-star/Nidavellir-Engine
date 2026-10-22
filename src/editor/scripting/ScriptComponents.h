#pragma once

// Unity-style script components, shared by the 2D and 3D editors:
//
//  * an entity carries a stack of scripts (scene/SceneData.h script_stack),
//    any mix of Lua, visual graphs, C++ and C#, each with its own props,
//    an enable toggle, reorder and remove (draw_script_stack);
//  * Inspector > Add Component > New Script... writes scripts/<stem>.<ext>
//    from a template (Blank / Gate / Pickup / Sign / Ticker in every
//    language) and attaches it; Add Component > Script lists the claim's
//    scripts with a search box (AddScriptMenu);
//  * scripts drag from the Asset Browser or the Scripts panel onto an
//    entity (payload kScriptPayload: the project-relative path, NUL ended);
//  * "Open in Visual Studio" / "Open in VS Code": installs are detected
//    (vswhere; code on PATH, the registry or the usual folders), the claim
//    gets a small IDE setup (.vscode/, <claim>.code-workspace, a C#
//    .csproj referencing the SDK, compile_commands.json + c_cpp_properties
//    for C++ against script_sdk.h) and the claim folder opens on the file.

#include "scene/SceneData.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct ImVec4;

namespace ts {
namespace tombstone {
namespace runtime {
class ScriptLibrary;
}
namespace editor {

// Drag payload for a script file (data: project-relative path + NUL).
inline constexpr const char* kScriptPayload = "TS_SCRIPT";

// What a script file declares, for the Inspector and the Scripts panel.
struct ScriptInfo {
  bool ok = false;
  std::string error;                 // file:line when it does not run
  std::vector<ScriptProp> defaults;  // declared props, sorted by name
  std::uint64_t version = 0;         // ScriptLibrary version it was read at
};

// Reads + describes scripts on demand, re-reading when a file changes.
class ScriptInfoCache {
 public:
  ScriptInfoCache();
  ~ScriptInfoCache();
  const ScriptInfo& get(const std::string& project_dir, const std::string& rel);
  void clear();

 private:
  std::unique_ptr<runtime::ScriptLibrary> lib_;
  std::map<std::string, ScriptInfo> infos_;
};
// The shared body of ScriptInfoCache::get (the 2D editor has its own library).
void describe_script_into(runtime::ScriptLibrary& lib, const std::string& project_dir,
                          const std::string& rel, ScriptInfo* info);

// --- Languages + templates -------------------------------------------------------

enum class ScriptLang { Lua = 0, Graph, Cpp, CSharp };
inline constexpr int kScriptLangCount = 4;
const char* lang_label(ScriptLang l);  // "Lua", "Visual graph", "C++", "C#"
ImVec4 lang_colour(ScriptLang l);  // UI tint per language
const char* lang_tag(ScriptLang l);    // "Lua", "Graph", "C++", "C#"
const char* lang_ext(ScriptLang l);    // ".lua", ".graph.json", ".cpp", ".cs"
ScriptLang lang_of(const std::string& rel);  // by extension (unknown -> Lua)
// "scripts/gate_keeper.graph.json" -> "gate_keeper".
std::string script_display_name(const std::string& rel);

struct ScriptTemplateInfo {
  const char* id;     // "blank", "gate", "pickup", "sign", "ticker"
  const char* label;  // "Gate"
  const char* blurb;
};
const std::vector<ScriptTemplateInfo>& script_templates();

// "Gate Keeper" -> "gate_keeper" (letters, digits, _ and -; known script
// extensions dropped). Empty when nothing usable is left.
std::string script_stem(const std::string& name);
// "gate_keeper" -> "GateKeeper" (C++ / C# class name; "Script" prefix when
// the stem starts with a digit).
std::string script_class_name(const std::string& stem);
// The template's source for that language, named after the stem.
bool script_template_source(ScriptLang lang, const std::string& template_id, const std::string& stem,
                            std::string* out);

struct NewScriptResult {
  bool ok = false;
  std::string rel;      // scripts/<stem><ext>
  std::string message;  // what happened, readable (shown as a note)
};
// Writes <project>/scripts/<stem><ext> from a template. Never overwrites.
NewScriptResult create_script_file(const std::string& project_dir, const std::string& name, ScriptLang lang,
                                   const std::string& template_id);

// <project>/scripts/** files the runtime can run, project-relative, sorted.
std::vector<std::string> list_script_files(const std::string& project_dir, std::size_t max = 2000);

// --- External editors ------------------------------------------------------------

enum class ExternalEditor { Auto = 0, VSCode, VisualStudio, Os };
// Settings > Tools > "Open scripts in": "auto" | "vscode" | "vs" | "os".
void set_script_editor_preference(const std::string& pref);
ExternalEditor script_editor_preference();
const char* external_editor_label(ExternalEditor e);
ExternalEditor external_editor_from_name(const std::string& s);
const char* external_editor_name(ExternalEditor e);  // "auto", "vscode", ...

struct EditorInstall {
  bool found = false;
  std::string path;    // code / code.cmd / devenv.exe
  std::string detail;  // where it was found, or why not
};
// Detected once per process; refresh = probe again.
const EditorInstall& find_vscode(bool refresh = false);
const EditorInstall& find_visual_studio(bool refresh = false);
// Tests: replace the detection (empty path = "not installed").
void override_editor_install(ExternalEditor which, const std::string& path);

struct IdeFiles {
  std::string workspace;  // <claim>/<name>.code-workspace
  std::string csproj;     // <claim>/.tombstone/ide/<Name>.Scripts.csproj ("" without C#)
  std::vector<std::string> written;  // files (re)written this call
};
// The claim's IDE setup: .vscode/settings.json + extensions.json +
// c_cpp_properties.json (written once, then left for the user),
// <claim>.code-workspace, compile_commands.json and CppProperties.json for
// C++ scripts, and a C# project referencing the SDK binding.
bool write_ide_files(const std::string& project_dir, const std::string& project_name, IdeFiles* out,
                     std::string* error);

// Builds the launch line without starting anything (tests, docs).
struct LaunchPlan {
  std::string program;
  std::vector<std::string> args;
};
bool plan_open_in_editor(ExternalEditor which, const std::string& project_dir, const std::string& project_name,
                         const std::string& rel, LaunchPlan* plan, std::string* error);
// Opens the claim folder on the file in VS Code / Visual Studio, or hands
// the file to the OS. Auto: the preference, else VS Code, else Visual
// Studio (C# / C++), else the OS. Graphs always open in the Brand Book.
// *message: a readable line for the editor's notes either way.
bool open_script_external(ExternalEditor which, const std::string& project_dir, const std::string& project_name,
                          const std::string& rel, std::string* message);
// Tests: no process is started; open_script_external records the plan.
void set_external_launch_dry_run(bool on);
const LaunchPlan& last_external_launch();

// --- Inspector widgets -----------------------------------------------------------

struct ScriptStackHost {
  std::string project_dir;
  std::string project_name;
  const std::vector<std::string>* scripts = nullptr;  // files to pick from
  std::function<const ScriptInfo&(const std::string& rel)> info;
  std::function<void(const char* label)> after_item;  // undo tracking per widget
  std::function<void(const std::string& message)> note;
  bool read_only = false;  // playing
};

enum class StackChange { None, Live, Commit };
struct StackResult {
  StackChange change = StackChange::None;
  std::string label;  // undo label for Commit
};
// The script components of one entity, edited in place. Live = a drag or
// text edit in progress (after_item does the undo bookkeeping); Commit =
// one discrete step (enable, reorder, remove, file, reset prop).
StackResult draw_script_stack(std::vector<ScriptData>& stack, const ScriptStackHost& host);

// Inspector "Add Component" for scripts: a searchable list of the claim's
// scripts plus New Script... (a modal: name, language, template).
struct AddScriptMenu {
  char search[64] = "";
  char name[64] = "";
  int lang = 0;
  int templ = 0;
  bool want_modal = false;
  std::string error;
  // Inside an open popup / menu: draws "New Script..." + the searchable
  // list. Returns the picked existing script ("" = none this frame).
  std::string draw_items(const ScriptStackHost& host);
  // Every frame (outside any popup). Returns the created script's rel.
  std::string draw_modal(const ScriptStackHost& host);
};

// The Scripts panel (both editors): write a script in any language from a
// template, the claim's scripts (drag one onto an entity; double-click
// opens it), and the selected file's status, Open / VS Code / Visual
// Studio buttons and "Attach to <selection>".
struct ScriptsPanelState {
  char* name = nullptr;
  std::size_t name_size = 0;
  int* lang = nullptr;
  int* templ = nullptr;
  std::string* selected = nullptr;
};
struct ScriptsPanelHost {
  ScriptStackHost stack;
  bool playing = false;
  std::function<bool(const std::string& name, ScriptLang lang, const std::string& template_id)> create;
  std::string target_name;  // "" = nothing attachable selected
  std::function<bool(const std::string& rel)> target_has;
  std::function<void(const std::string& rel)> attach;
};
void draw_scripts_panel_body(ScriptsPanelState& st, const ScriptsPanelHost& host);

// Drop target helper: after an item, accepts a dragged script.
// Returns the rel ("" when nothing dropped this frame).
std::string accept_script_drop();
// True while a script (TS_SCRIPT payload) is being dragged.
bool dragging_script();
// Inside an open BeginDragDropTarget: accept a script payload ("" if none).
std::string accept_script_payload();
// Makes the last item a drag source for a script.
void script_drag_source(const std::string& rel);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
