// Sample *dynamic* plugin (ts_plugin_lantern.dll / .so): built as a CMake
// MODULE, found by Catalog::scan_folder, handshakes through
// TS_DYNAMIC_PLUGIN. It needs only engine headers -- every host call is
// virtual -- and links nothing from the engine.
//
// "Lanterns" depends on ts.tumbleweed (>= 1.0) to show dependency order
// across the static / dynamic line: it counts lit lanterns (entities named
// Lantern*) each tick and logs once when a ride starts.

#include "plugin/PluginAbi.h"

#include <cctype>
#include <string>

namespace {

class LanternPlugin final : public ts::tombstone::plugin::Plugin {
 public:
  LanternPlugin() {
    info_.id = "sample.lantern";
    info_.name = "Lanterns";
    info_.description =
        "Dynamic-library sample: keeps count of lanterns (Lantern* props). "
        "Rides along behind Tumbleweeds.";
    info_.author = "TombStone";
    info_.version = ts::tombstone::plugin::Version{0, 3, 0};
    info_.category = ts::tombstone::plugin::PluginCategory::Sample;
    info_.project_types = ts::tombstone::plugin::kType2D;
    info_.deps.push_back({"ts.tumbleweed", ts::tombstone::plugin::Version{1, 0, 0}, false});
  }

  const ts::tombstone::plugin::PluginInfo& info() const override { return info_; }

  void on_register(ts::tombstone::plugin::Registrar& reg) override {
    reg.add_component("Lantern", "Counted by the Lanterns sample plugin");
    reg.add_asset_type({".lantern.json", "Lantern light settings"});
    reg.add_system("lantern.count",
                   [this](ts::tombstone::runtime::World&, double) { ++ticks_; });
  }
  void on_play_begin(ts::tombstone::runtime::World&) override { ticks_ = 0; }

 private:
  ts::tombstone::plugin::PluginInfo info_;
  unsigned long long ticks_ = 0;
};

}  // namespace

TS_DYNAMIC_PLUGIN(LanternPlugin)
