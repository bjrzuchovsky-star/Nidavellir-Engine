#include "editor/ux/UxStrings.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace ts::tombstone::editor::ux {

namespace {

// Key -> text. Keys are widget labels as ux::label_key() cleans them
// ("Grid##tb" -> "Grid", "##grid_size" -> "#grid_size"), optionally
// prefixed with the section they live in ("Collider/Offset",
// "Toolbar2D/Grid", "Menu/Save Scene", "Header/Mesh").
const std::unordered_map<std::string, std::string>& table() {
  static const std::unordered_map<std::string, std::string> t = {
#include "editor/ux/UxTips.inc"
  };
  return t;
}

}  // namespace

const char* dictionary_text(const std::string& key) {
  const auto& t = table();
  auto it = t.find(key);
  return it == t.end() ? nullptr : it->second.c_str();
}

const std::vector<std::string>& dictionary_labels() {
  static const std::vector<std::string> labels = [] {
    std::set<std::string> s;
    for (const auto& [k, v] : table()) {
      // "Inspector3D/Position" -> "Position"; but "Size s/m/e" is a label
      // with slashes in it, so keep every candidate (matching is by hash).
      s.insert(k);
      const std::size_t first = k.find('/');
      if (first != std::string::npos) s.insert(k.substr(first + 1));
    }
    return std::vector<std::string>(s.begin(), s.end());
  }();
  return labels;
}

std::size_t dictionary_size() { return table().size(); }

EmptyText empty_text(const std::string& id) {
  static const std::unordered_map<std::string, EmptyText> t = {
      {"hierarchy2d",
       {"No objects yet.",
        "Create an entity, camera or tile map below (also in the Edit menu), or drag an image from "
        "the Supply Wagon or Asset Browser into the viewport."}},
      {"hierarchy3d",
       {"No objects yet.",
        "Add a cube, camera or light below (more in the Create menu), or drag a model from the "
        "Assets panel into the viewport."}},
      {"assets",
       {"No assets in this claim yet.",
        "Import brings in images, models, sounds and fonts (or drop files onto the window). Files "
        "copied into the claim's assets folder show up after Rescan. Pull from the Vault to reuse "
        "assets from your other claims."}},
      {"scripts",
       {"No scripts yet.",
        "Scripts make objects do things in Play. Write one here (name, language and template "
        "above), then drag it onto an object or attach it with Add Component in the Inspector."}},
      {"vault",
       {"The Vault is empty.",
        "The Vault keeps assets you share between claims. Right-click an asset in the Asset "
        "Browser and choose Send to Vault to put the first one in."}},
      {"telegraph",
       {"Nothing on the wire yet.",
        "Script messages, warnings and errors show here. Press Play (F5) and anything your scripts "
        "log will appear. Hover an error for what it means and how to fix it."}},
      {"inspector2d",
       {"Nothing selected.",
        "Click an object in the viewport or the Hierarchy to edit it here. Ctrl+click or drag a box "
        "to select several."}},
      {"inspector3d",
       {"Nothing selected.",
        "Click an object in the viewport or the Hierarchy to edit its transform and components "
        "here."}},
      {"search",
       {"No matches.", "Nothing in the scene matches the search."}},
      {"asset_search",
       {"Nothing here.",
        "This folder or filter has no assets. Show all, or drop files on this panel to import them "
        "into the folder."}},
      {"vault_search",
       {"No matches.", "Nothing in the Vault matches the search, tag or filters."}},
      {"telegraph_filter",
       {"Nothing matches the filter.", "Lines are hidden by the search or the Info / Warn / Error toggles."}},
      {"prefabs",
       {"No prefabs yet.",
        "A prefab is a saved object you can stamp out again. Select an object and use Make Prefab "
        "to save one."}},
  };
  auto it = t.find(id);
  if (it != t.end()) return it->second;
  return {"Nothing here yet.", ""};
}

const std::vector<WalkStep>& walkthrough_steps(bool three_d) {
  static const std::vector<WalkStep> k2d = {
      {"Hierarchy", "Hierarchy",
       "Every object in the scene is listed here. Click to select, drag to re-parent, right-click "
       "for more. Start with Create Entity or Create Camera."},
      {"Viewport2D", "Viewport",
       "This is the scene. Drag to move the selection; G, R and S move, rotate and scale with the "
       "mouse (type a number for an exact value). Right-click for the context menu, the mouse "
       "wheel zooms."},
      {"Inspector", "Inspector",
       "Edit the selected object here. Hover any field for what it does and its units; Add "
       "Component gives it a sprite, collider, script and more. Right-click a component header to "
       "reset, copy or remove it."},
      {"Asset Browser", "Asset Browser",
       "Your claim's images, sounds, models and scripts. Import (or drop files on the window) to "
       "bring new ones in; drag an image into the viewport to place it."},
      {"", "Play",
       "Press F5 (or Ctrl+P) to ride the scene, F5 again to stop. Stopping puts everything back "
       "the way it was."},
      {"Plugins", "Trading Post",
       "The Trading Post has plugins, templates and assets for your claim. Open it from the "
       "Plugins panel (View > Plugins): Visit the Trading Post..."},
      {"Vault", "Vault",
       "The Vault keeps assets you share between claims. Send an asset there from its right-click "
       "menu; pull it into another claim later."},
      {"", "Find anything",
       "Press Ctrl+Shift+P for the command palette: every menu item, panel and setting by name. "
       "Window > Layouts saves panel arrangements; Help > Walkthrough shows this again."},
  };
  static const std::vector<WalkStep> k3d = {
      {"Hierarchy###Hierarchy3D", "Hierarchy",
       "Every object in the scene. Click to select, right-click for more. Create > Camera or "
       "Create > Cube puts the first thing in."},
      {"Viewport###Viewport3D", "Viewport",
       "The scene in 3D. Right-drag to look, WASD to fly. W/E/R switch the gizmo, G, R and S "
       "move, rotate and scale with the mouse. Right-click an object for its menu."},
      {"Inspector###Inspector3D", "Inspector",
       "Edit the selected object. Hover any field for what it does and its units; Add Component "
       "gives it a mesh, light, collider or script. Right-click a component header for reset, "
       "copy and paste."},
      {"Asset Browser###AssetBrowser3D", "Asset Browser",
       "Models, textures, sounds and scripts in this claim. Import (or drop files) to bring new "
       "ones in; drag a model into the viewport to place it."},
      {"", "Play",
       "Press F5 to play the scene, F5 again to stop; the scene goes back to how it was."},
      {"Plugins###Plugins3D", "Trading Post",
       "The Trading Post has plugins, templates and assets ready to use. Open it from the Plugins "
       "panel (View > Plugins): Visit the Trading Post..."},
      {"Vault###Vault3D", "Vault",
       "Assets shared between all your claims. Send one there from the Asset Browser's "
       "right-click menu."},
      {"", "Find anything",
       "Ctrl+Shift+P opens the command palette. Window > Layouts saves panel arrangements; Help > "
       "Walkthrough shows this tour again."},
  };
  return three_d ? k3d : k2d;
}

const std::vector<SettingsSection>& settings_sections() {
  static const std::vector<SettingsSection> s = {
      {"Territory", "Where claims are kept on disk"},
      {"Rider", "Your name and dev login"},
      {"Lamplight", "Theme"},
      {"Supply Run", "Update checks"},
      {"Tools", "Blender, script editor, Source content folders"},
      {"Field Guide", "Tooltips, walkthroughs"},
      {"Trail", "Where settings live"},
  };
  return s;
}

}  // namespace ts::tombstone::editor::ux
