// Editor-side built-in plugins. The posse (multiplayer) is the proof that
// an existing engine feature can live behind the plugin interface: its
// Network panel, Play-menu entries and toolbar toggle are contributions,
// and with the plugin switched off the editor shows none of them and
// refuses to start a posse.

#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/AiPanels.h"
#include "editor/plugins/ProfilerPanels.h"
#include "editor/plugins/StoryPanels.h"
#include "editor/plugins/FrontierPanels.h"
#include "editor/plugins/ShipyardPanels.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

namespace ts {
namespace tombstone {
namespace editor {

class PossePlugin final : public plugin::Plugin {
 public:
  static const plugin::PluginInfo& static_info() {
    static const plugin::PluginInfo info = [] {
      plugin::PluginInfo i;
      i.id = "ts.posse";
      i.name = "Posse (Multiplayer)";
      i.description =
          "Play with posse: a local server and 1-4 riders in the editor, lag "
          "sim, the Network panel and ts_client windows.";
      i.author = "TombStone";
      i.version = plugin::Version{1, 0, 0};
      i.category = plugin::PluginCategory::Engine;
      i.project_types = plugin::kType2D;
      i.default_enabled = true;  // shipped before plugins: on by default
      return i;
    }();
    return info;
  }
  const plugin::PluginInfo& info() const override { return static_info(); }

  bool on_load(const plugin::PluginContext& ctx, std::string* /*error*/) override {
    screen_ = ctx.host == "editor2d"
                  ? static_cast<Editor2DScreen*>(ctx.host_object)
                  : nullptr;
    return true;
  }

  void on_register(plugin::Registrar& reg) override {
    if (!screen_) return;  // headless hosts: nothing to show
    Editor2DScreen* s = screen_;

    plugin::EditorPanel panel;
    panel.id = "posse.network";
    panel.title = "Network";
    panel.menu_label = "Network (Posse)";
    panel.open = &s->show_network_;
    panel.before_begin = [s]() {
      if (s->focus_network_) {
        ImGui::SetNextWindowFocus();
        s->focus_network_ = false;
      }
      ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    };
    panel.draw = [s]() { s->draw_network_panel(); };
    reg.add_editor_panel(std::move(panel));

    plugin::EditorMenuItem play;
    play.menu = "Play";
    play.draw = [s]() { s->draw_posse_menu_items(); };
    reg.add_menu_item(std::move(play));

    plugin::EditorMenuItem toolbar;
    toolbar.menu = "Toolbar";
    toolbar.draw = [s]() { s->draw_posse_toolbar_button(); };
    reg.add_menu_item(std::move(toolbar));
  }

  void on_unload() override {
    if (screen_ && screen_->posse_running()) {
      screen_->stop_posse();
    }
    screen_ = nullptr;
  }

 private:
  Editor2DScreen* screen_ = nullptr;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(posse, ::ts::tombstone::editor::PossePlugin)
TS_PLUGIN_DECLARE(posse);

namespace ts {
namespace tombstone {
namespace editor {

void register_editor_builtin_plugins(plugin::Catalog& catalog,
                                     std::vector<std::string>* errors) {
  install_frontier_panels();
  install_ai_panels();
  install_story_panels();
  install_profiler_panels();
  install_shipyard_panels();
  std::string err;
  if (!TS_PLUGIN_LINK(posse, catalog, &err) && errors) {
    errors->push_back(err);
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
