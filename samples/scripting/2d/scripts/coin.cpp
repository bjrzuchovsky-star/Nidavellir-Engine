// C++ twin of coin.cs.
#include <tombstone/script_sdk.h>

#include <string>

struct Coin : ts::Script {
  void OnStart() override { Log("coin worth " + std::to_string(Prop("worth", 1))); }
  void on_trigger_enter(Entity) override {
    Toast("Coin", "Pocketed");
    set_visible(self(), false);
  }
};
TS_SCRIPT(Coin, "coin")
