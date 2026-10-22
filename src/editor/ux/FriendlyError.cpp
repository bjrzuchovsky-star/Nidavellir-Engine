#include "editor/ux/FriendlyError.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <initializer_list>

namespace ts::tombstone::editor::ux {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool has(const std::string& low, std::initializer_list<const char*> needles) {
  for (const char* n : needles)
    if (low.find(n) != std::string::npos) return true;
  return false;
}

bool missing_words(const std::string& l) {
  return has(l, {"not found", "cannot open", "can't open", "could not open", "could not load", "could not read",
                 "no such file", "does not exist", "doesn't exist", "is missing", "missing file", "file is gone",
                 "could not be read", "or decode", "failed to load", "unreadable"});
}

bool texture_words(const std::string& l) {
  return has(l, {"texture", "image", "sprite", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".vtf", ".dds", ".hdr",
                 ".psd", ".ktx"});
}

bool model_words(const std::string& l) {
  return has(l, {"model", "mesh", ".glb", ".gltf", ".obj", ".fbx", ".mdl", ".blend", ".dae", ".ply", ".stl"});
}

// The first thing that looks like a file path ("scripts/gate.lua",
// "C:\x\y.png") or a quoted name ('ts.example').
std::string pull_subject(const std::string& raw) {
  const auto q = raw.find('\'');
  if (q != std::string::npos) {
    const auto e = raw.find('\'', q + 1);
    if (e != std::string::npos && e > q + 1 && e - q < 120) return raw.substr(q + 1, e - q - 1);
  }
  std::size_t i = 0;
  while (i < raw.size()) {
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '(' || raw[i] == '"' || raw[i] == ',')) ++i;
    std::size_t j = i;
    while (j < raw.size() && raw[j] != ' ' && raw[j] != '"' && raw[j] != ',' && raw[j] != ')' && raw[j] != '\n') ++j;
    std::string tok = raw.substr(i, j - i);
    while (!tok.empty() && (tok.back() == ':' || tok.back() == '.' || tok.back() == ';')) tok.pop_back();
    const bool sep = tok.find('/') != std::string::npos || tok.find('\\') != std::string::npos;
    const auto dot = tok.rfind('.');
    const bool ext = dot != std::string::npos && dot > 0 && dot + 1 < tok.size() && tok.size() - dot <= 6 &&
                     std::isalpha(static_cast<unsigned char>(tok[dot + 1]));
    if ((sep || ext) && tok.find("://") == std::string::npos && tok.size() > 2) {
      // "scripts/gate.lua:12" -> keep the line number, it helps.
      return tok;
    }
    if (tok.find("://") != std::string::npos) return tok;  // a URL is a fine subject too
    i = j + 1;
  }
  return {};
}

std::string quoted(const std::string& s) { return s.empty() ? std::string() : " (" + s + ")"; }

}  // namespace

const char* to_string(ErrorKind k) {
  switch (k) {
    case ErrorKind::Unknown: return "unknown";
    case ErrorKind::MissingFile: return "missing_file";
    case ErrorKind::MissingTexture: return "missing_texture";
    case ErrorKind::MissingModel: return "missing_model";
    case ErrorKind::ImportFailed: return "import_failed";
    case ErrorKind::ScriptCompile: return "script_compile";
    case ErrorKind::ScriptRuntime: return "script_runtime";
    case ErrorKind::PluginMissing: return "plugin_missing";
    case ErrorKind::ExportFailed: return "export_failed";
    case ErrorKind::VaultConnection: return "vault_connection";
    case ErrorKind::BlenderNotFound: return "blender_not_found";
    case ErrorKind::UpdateCheck: return "update_check";
    case ErrorKind::SceneVersion: return "scene_version";
    case ErrorKind::SaveFailed: return "save_failed";
  }
  return "unknown";
}

std::vector<ErrorKind> all_error_kinds() {
  return {ErrorKind::MissingFile,   ErrorKind::MissingTexture,  ErrorKind::MissingModel,
          ErrorKind::ImportFailed,  ErrorKind::ScriptCompile,   ErrorKind::ScriptRuntime,
          ErrorKind::PluginMissing, ErrorKind::ExportFailed,    ErrorKind::VaultConnection,
          ErrorKind::BlenderNotFound, ErrorKind::UpdateCheck,   ErrorKind::SceneVersion,
          ErrorKind::SaveFailed};
}

ErrorKind classify(const std::string& raw, ErrorKind hint) {
  const std::string l = lower(raw);
  if (has(l, {"blender was not found", "blender not found", "blender executable", "could not start blender"}))
    return ErrorKind::BlenderNotFound;
  if (has(l, {"newer than this build", "unsupported version", "made by a newer", "version mismatch",
              "is a 2d scene", "is a 3d scene"}))
    return ErrorKind::SceneVersion;
  if (has(l, {"missing plugin library", "isn't in the wagon", "which isn't installed", "plugin not found",
              "no such plugin", "plugin library"}))
    return ErrorKind::PluginMissing;
  if (hint == ErrorKind::VaultConnection || has(l, {"vault server", "ts_vault", "[vault]"}))
    if (has(l, {"connect", "reach", "resolve", "timed out", "timeout", "refused", "401", "403", "unauthor",
                "token", "server", "offline", "network"}))
      return ErrorKind::VaultConnection;
  if (has(l, {"could not reach the release", "sha256sums", "update check", "[update]", "release feed"}) ||
      (hint == ErrorKind::UpdateCheck && has(l, {"download failed", "could not reach", "could not fetch", "http"})))
    return ErrorKind::UpdateCheck;
  if (hint == ErrorKind::ExportFailed ||
      has(l, {"export failed", "packaging failed", "package failed", "iscc not found", "inno setup",
              "runtime not found", "could not stage", "[export]"}))
    return ErrorKind::ExportFailed;
  if (has(l, {"syntax error", "unexpected symbol", "near '", "near <eof>", "'end' expected", "expected near",
              "compile error", "failed to compile", "error cs", "error c2", "cmake could not configure",
              "unfinished string", "malformed number"}))
    return ErrorKind::ScriptCompile;
  if (has(l, {"attempt to ", "bad argument", "is stopped; the rest of the world rides on", "graph stopped",
              "script engine fell over", "stack overflow", "nullreferenceexception", "exception in"}))
    return ErrorKind::ScriptRuntime;
  if (has(l, {"import failed", "could not import", "import error"}) || hint == ErrorKind::ImportFailed) {
    if (missing_words(l) && texture_words(l)) return ErrorKind::MissingTexture;
    if (missing_words(l) && model_words(l) && !has(l, {"blender"})) return ErrorKind::MissingModel;
    return ErrorKind::ImportFailed;
  }
  if (has(l, {"script not found"})) return ErrorKind::MissingFile;
  if (missing_words(l)) {
    if (texture_words(l)) return ErrorKind::MissingTexture;
    if (model_words(l)) return ErrorKind::MissingModel;
    return ErrorKind::MissingFile;
  }
  if (has(l, {"could not write", "autosave failed", "save failed", "failed to write", "could not save",
              "could not replace", "could not create directory", "permission denied", "disk full"}))
    return ErrorKind::SaveFailed;
  if (hint == ErrorKind::ScriptRuntime || hint == ErrorKind::ScriptCompile) return hint;
  return hint;
}

FriendlyError explain(const std::string& raw, ErrorKind hint, const std::string& context) {
  FriendlyError f;
  f.kind = classify(raw, hint);
  f.subject = pull_subject(raw);
  f.details = context.empty() ? raw : context + "\n" + raw;
  const std::string s = quoted(f.subject);
  switch (f.kind) {
    case ErrorKind::MissingFile:
      f.title = "A file is missing";
      f.what = "TombStone went looking for a file" + s + " and it wasn't there.";
      f.why = "It was probably moved, renamed or deleted outside the editor, or the claim was copied without it.";
      f.fix = "Put the file back, or point the thing that uses it at the new file. Asset Browser > Rescan picks "
              "up files moved by hand.";
      break;
    case ErrorKind::MissingTexture:
      f.title = "A texture couldn't be loaded";
      f.what = "An image" + s + " couldn't be opened, so a placeholder is drawn instead.";
      f.why = "The file was moved or deleted, or it's in a format TombStone can't read (PNG, JPG, TGA, BMP and "
              "VTF are fine).";
      f.fix = "Re-import the image (right-click it in the Asset Browser > Reimport), or pick another texture in "
              "the Inspector.";
      break;
    case ErrorKind::MissingModel:
      f.title = "A model couldn't be loaded";
      f.what = "A 3D model" + s + " couldn't be opened, so the object shows as an empty box.";
      f.why = "The model file was moved or deleted, or a texture or .bin file it depends on is missing.";
      f.fix = "Re-import the model from the Asset Browser, or keep the .gltf, .bin and textures together in one "
              "folder and import again.";
      break;
    case ErrorKind::ImportFailed:
      f.title = "Import didn't work";
      f.what = "The file" + s + " couldn't be brought into the claim.";
      f.why = "The file may be damaged, still being written by another program, or in a format TombStone doesn't "
              "import.";
      f.fix = "Check the file opens in the program that made it, save it again (PNG for images, glTF/GLB or OBJ "
              "for models), then use Import file... again.";
      break;
    case ErrorKind::ScriptCompile:
      f.title = "A script has a mistake in it";
      f.what = "The script" + s + " couldn't be read, so it won't run until it's fixed.";
      f.why = "Usually a typo: a missing 'end', bracket or quote, or a misspelt keyword near the line shown.";
      f.fix = "Open the script at that line (double-click the Telegraph line), fix it and save. It reloads by "
              "itself.";
      break;
    case ErrorKind::ScriptRuntime:
      f.title = "A script stopped while running";
      f.what = "The script" + s + " hit a problem during play and was stopped. The rest of the world keeps going.";
      f.why = "Most often it used something that isn't there (a nil value), such as an entity that was deleted "
              "or a property with a different name.";
      f.fix = "Double-click the Telegraph line to open the script at that line. Check the names, and guard "
              "values that might be nil with 'if x then'.";
      break;
    case ErrorKind::PluginMissing:
      f.title = "A plugin is missing";
      f.what = "This claim asks for a plugin" + s + " that isn't installed, so it rides on without it.";
      f.why = "The claim was made on another PC, or the plugin was uninstalled or failed to download.";
      f.fix = "Open the Trading Post and install (hitch) the plugin, or switch it off for this claim.";
      f.fix_action = "Panel/Trading Post";
      f.fix_label = "Open Trading Post";
      break;
    case ErrorKind::ExportFailed:
      f.title = "Export didn't finish";
      f.what = "The game couldn't be packaged" + s + ".";
      f.why = "A tool the export needs is missing (the game runtime, or Inno Setup for an installer), or the "
              "output folder can't be written.";
      f.fix = "Check the output folder exists and isn't open in another program, then export again. For a "
              "setup.exe, install Inno Setup 6.";
      break;
    case ErrorKind::VaultConnection:
      f.title = "Can't reach the Vault";
      f.what = "The Vault server" + s + " didn't answer, so shared assets can't be listed or sent.";
      f.why = "The server is off or unreachable from this PC, the address is wrong, or the access token has "
              "changed.";
      f.fix = "Check the address and token in Vault settings, or switch the Vault to a local folder for now.";
      f.fix_label = "Vault settings";
      break;
    case ErrorKind::BlenderNotFound:
      f.title = "Blender wasn't found";
      f.what = "This file needs Blender to import, and TombStone couldn't find Blender on this PC.";
      f.why = ".blend files (and some other formats) are converted by Blender in the background.";
      f.fix = "Install Blender, or set its path in Settings > Tools > Blender executable. You can also export "
              "glTF from Blender yourself and import that.";
      f.fix_action = "Settings/Tools";
      f.fix_label = "Open Settings > Tools";
      break;
    case ErrorKind::UpdateCheck:
      f.title = "Couldn't check for updates";
      f.what = "TombStone couldn't reach the release server" + s + ". Nothing has changed on this PC.";
      f.why = "Usually no internet connection, a proxy or firewall, or the server is busy.";
      f.fix = "Try again later. You can turn update checks off in Settings > Supply Run.";
      f.fix_action = "Settings/Supply Run";
      f.fix_label = "Open Settings > Supply Run";
      break;
    case ErrorKind::SceneVersion:
      if (raw.find("is a 2D scene") != std::string::npos || raw.find("is a 3D scene") != std::string::npos) {
        const bool two = raw.find("is a 2D scene") != std::string::npos;
        f.title = two ? "This is a 2D scene" : "This is a 3D scene";
        f.what = std::string("This claim's scene.json was made by the ") + (two ? "2D" : "3D") +
                 " editor, so this editor didn't open it.";
        f.why = "The scene file was copied in from another claim, or the claim's kind was changed by hand.";
        f.fix = std::string("Open it from a ") + (two ? "2D" : "3D") +
                " claim, or move scene.json out of the way to start this claim fresh.";
        break;
      }
      f.title = "This scene is from a newer TombStone";
      f.what = "The scene" + s + " was saved by a newer version of TombStone than this one, so it wasn't opened.";
      f.why = "Newer versions add things older ones can't read. Opening it here could lose them.";
      f.fix = "Update TombStone (the Project Manager shows a banner when a newer version is out), then open the "
              "claim again. Saving is switched off for this scene, so your file hasn't been touched.";
      break;
    case ErrorKind::SaveFailed:
      f.title = "Couldn't save";
      f.what = "A file" + s + " couldn't be written, so your latest changes aren't on disk yet.";
      f.why = "The folder is read-only, the disk is full, or another program (a sync tool or antivirus) has the "
              "file open.";
      f.fix = "Close programs that might have the file open, check there's free space, then save again (Ctrl+S).";
      break;
    case ErrorKind::Unknown:
      f.title = "Something went wrong";
      f.what = "TombStone hit a problem it doesn't have a plain explanation for yet.";
      f.why = "See Details for the exact message.";
      f.fix = "Try again. If it keeps happening, use Copy details and include it when you report the bug.";
      break;
  }
  return f;
}

std::string FriendlyError::summary() const { return what + "\nWhy: " + why + "\nWhat to do: " + fix; }

std::string FriendlyError::copy_text() const {
  return "TombStone error: " + title + " [" + to_string(kind) + "]\n" + summary() + "\n\nDetails:\n" + details + "\n";
}

// ---- modal ------------------------------------------------------------

namespace {
std::deque<FriendlyError> g_queue;
bool g_open = false;

void body(const FriendlyError& e, float wrap) {
  ImGui::PushTextWrapPos(wrap);
  ImGui::TextUnformatted(e.what.c_str());
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  ImGui::TextUnformatted("Why");
  ImGui::PopStyleColor();
  ImGui::TextUnformatted(e.why.c_str());
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  ImGui::TextUnformatted("What to do");
  ImGui::PopStyleColor();
  ImGui::TextUnformatted(e.fix.c_str());
  ImGui::PopTextWrapPos();
}

void details(const FriendlyError& e) {
  if (ImGui::TreeNodeEx("Details", ImGuiTreeNodeFlags_SpanAvailWidth)) {
    std::string d = e.details;
    ImGui::InputTextMultiline("##details", d.data(), d.size() + 1, ImVec2(-1, ImGui::GetTextLineHeight() * 6),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::TreePop();
  }
}
}  // namespace

void show_error(FriendlyError e) { g_queue.push_back(std::move(e)); }
bool error_modal_pending() { return !g_queue.empty(); }
const FriendlyError* current_error() { return g_queue.empty() ? nullptr : &g_queue.front(); }
void clear_errors() {
  g_queue.clear();
  g_open = false;
}

void draw_error_modal(const std::function<bool(const std::string&)>& run_action) {
  if (g_queue.empty()) return;
  const FriendlyError& e = g_queue.front();
  const std::string title = e.title + "###ts_friendly_error";
  if (!g_open) {
    ImGui::OpenPopup(title.c_str());
    g_open = true;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
  bool keep = true;
  if (ImGui::BeginPopupModal(title.c_str(), &keep, ImGuiWindowFlags_NoSavedSettings)) {
    body(e, ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX());
    ImGui::Spacing();
    details(e);
    ImGui::Separator();
    bool close = false;
    if (theme::PrimaryButton("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
    ImGui::SameLine();
    if (theme::SecondaryButton("Copy details")) ImGui::SetClipboardText(e.copy_text().c_str());
    if (!e.fix_action.empty() && run_action) {
      ImGui::SameLine();
      if (theme::SecondaryButton(e.fix_label.c_str())) {
        const std::string id = e.fix_action;
        close = true;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        g_queue.pop_front();
        g_open = false;
        run_action(id);
        return;
      }
    }
    if (close) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      g_queue.pop_front();
      g_open = false;
      return;
    }
    ImGui::EndPopup();
  }
  if (!keep) {
    g_queue.pop_front();
    g_open = false;
  }
}

bool ErrorBox(const char* id, const FriendlyError& e, bool show_fix_button) {
  ImGui::PushID(id);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
  ImGui::TextUnformatted(e.title.c_str());
  ImGui::PopStyleColor();
  ImGui::TextUnformatted(e.what.c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  ImGui::TextUnformatted(("What to do: " + e.fix).c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  bool hit = false;
  if (show_fix_button && !e.fix_label.empty()) {
    hit = theme::SecondaryButton(e.fix_label.c_str());
    ImGui::SameLine();
  }
  if (theme::SecondaryButton("Copy details")) ImGui::SetClipboardText(e.copy_text().c_str());
  ImGui::SameLine();
  if (theme::SecondaryButton("Why?")) show_error(e);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.why.c_str());
  details(e);
  ImGui::PopID();
  return hit;
}

void FriendlyLineTooltip(const FriendlyError& e) {
  if (!ImGui::BeginTooltip()) return;
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted(e.title.c_str());
  ImGui::PopStyleColor();
  body(e, ImGui::GetFontSize() * 32.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  ImGui::TextUnformatted("Right-click: Explain... / Copy details");
  ImGui::PopStyleColor();
  ImGui::EndTooltip();
}

ErrorKind telegraph_hint(const std::string& channel) {
  const std::string c = lower(channel);
  if (has(c, {"script", "lua", "graph", "csharp", "c#", "native"})) return ErrorKind::ScriptRuntime;
  if (has(c, {"import", "asset"})) return ErrorKind::ImportFailed;
  if (has(c, {"vault"})) return ErrorKind::VaultConnection;
  if (has(c, {"export", "package"})) return ErrorKind::ExportFailed;
  if (has(c, {"update", "supply"})) return ErrorKind::UpdateCheck;
  return ErrorKind::Unknown;
}

const char* short_title(ErrorKind k) {
  switch (k) {
    case ErrorKind::MissingFile: return "a file is missing";
    case ErrorKind::MissingTexture: return "texture missing";
    case ErrorKind::MissingModel: return "model missing";
    case ErrorKind::ImportFailed: return "import didn't work";
    case ErrorKind::ScriptCompile: return "mistake in the script";
    case ErrorKind::ScriptRuntime: return "script stopped";
    case ErrorKind::PluginMissing: return "plugin not installed";
    case ErrorKind::ExportFailed: return "export didn't finish";
    case ErrorKind::VaultConnection: return "can't reach the Vault";
    case ErrorKind::BlenderNotFound: return "Blender not found";
    case ErrorKind::UpdateCheck: return "update check failed";
    case ErrorKind::SceneVersion: return "scene from a newer TombStone";
    case ErrorKind::SaveFailed: return "couldn't save";
    case ErrorKind::Unknown: return "";
  }
  return "";
}

std::string telegraph_label(const std::string& text, bool problem, ErrorKind hint) {
  if (!problem) return text;
  const char* t = short_title(classify(text, hint));
  if (!*t) return text;
  return text + "   -- " + t + " (hover for help)";
}

bool TelegraphLineHelp(const std::string& text, bool problem, ErrorKind hint, const std::string& context) {
  bool shown = false;
  if (problem && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_NoSharedDelay)) {
    FriendlyLineTooltip(explain(text, hint, context));
    shown = true;
  }
  if (ImGui::BeginPopupContextItem("##tg_line_ctx")) {
    if (problem && ImGui::MenuItem("Explain...")) show_error(explain(text, hint, context));
    if (ImGui::MenuItem("Copy line")) ImGui::SetClipboardText(text.c_str());
    if (problem && ImGui::MenuItem("Copy details")) ImGui::SetClipboardText(explain(text, hint, context).copy_text().c_str());
    ImGui::EndPopup();
  }
  return shown;
}

}  // namespace ts::tombstone::editor::ux
